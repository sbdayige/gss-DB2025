/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include <memory>
#include <vector>

#include "execution/execution_common.h"  // 添加包含
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

/**
 * @brief UPDATE操作的执行器类
 *
 * UpdateExecutor是RMDB数据库系统中负责执行UPDATE语句的核心组件。
 * 它继承自AbstractExecutor，遵循火山模型的执行框架。
 *
 * 主要功能：
 * 1. 更新表中满足条件的记录
 * 2. 维护表上的所有索引，确保数据一致性
 * 3. 处理数据类型转换和验证
 * 4. 在失败时进行事务回滚
 */
class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                         // 表的元数据信息
    std::vector<Condition> conds_;        // WHERE条件列表
    RmFileHandle* fh_;                    // 表的数据文件句柄
    std::vector<Rid> rids_;               // 需要更新的记录位置列表
    std::string tab_name_;                // 表名
    std::vector<SetClause> set_clauses_;  // SET子句，指定要更新的列和新值
    SmManager* sm_manager_;               // 系统管理器

   public:
    /**
     * @brief UpdateExecutor构造函数
     *
     * @param sm_manager 系统管理器指针
     * @param tab_name 目标表名称
     * @param set_clauses SET子句列表，指定要更新的列和新值
     * @param conds WHERE条件列表
     * @param rids 需要更新的记录位置列表
     * @param context 执行上下文
     */
    UpdateExecutor(SmManager* sm_manager, const std::string& tab_name, std::vector<SetClause> set_clauses, std::vector<Condition> conds, std::vector<Rid> rids, Context* context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = std::move(set_clauses);
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = std::move(conds);
        rids_ = std::move(rids);
        context_ = context;
    }

    void delete_index(RmRecord* rec, Rid rid) {
        // 使用线程本地缓冲区避免频繁内存分配
        static thread_local std::vector<char> key_buffer;

        // 遍历所有索引
        for (auto& index : tab_.indexes) {
            // 获取索引句柄
            auto ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
            auto ih = sm_manager_->ihs_.at(ix_name).get();

            // 确保缓冲区足够大
            if (key_buffer.size() < static_cast<size_t>(index.col_tot_len)) {
                key_buffer.resize(index.col_tot_len);
            }

            // 构造索引键值
            char* key = key_buffer.data();
            int offset = 0;
            for (int j = 0; j < index.col_num; ++j) {
                memcpy(key + offset, rec->data + index.cols[j].offset, index.cols[j].len);
                offset += index.cols[j].len;
            }

            // 从索引中删除条目
            ih->delete_entry(key, context_->txn_);
        }
    }

    void restore_index(RmRecord* rec, Rid rid) {
        // 使用线程本地缓冲区避免频繁内存分配
        static thread_local std::vector<char> key_buffer;

        for (auto& index : tab_.indexes) {
            auto ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);  // 获取索引名称
            auto ih = sm_manager_->ihs_.at(ix_name).get();                                        // 获取索引句柄

            // 确保缓冲区足够大
            if (key_buffer.size() < static_cast<size_t>(index.col_tot_len)) {
                key_buffer.resize(index.col_tot_len);
            }

            char* key = key_buffer.data();  // 构造索引键值
            int offset = 0;
            for (int j = 0; j < index.col_num; ++j) {
                // 从记录数据中拷贝对应列的数据到key
                memcpy(key + offset, rec->data + index.cols[j].offset, index.cols[j].len);
                offset += index.cols[j].len;
            }
            // 将键值插入索引
            ih->insert_entry(key, rid, context_->txn_);
        }
    }

    bool insert_index(RmRecord* rec, Rid rid) {
        // 使用线程本地缓冲区避免频繁内存分配
        static thread_local std::vector<char> key_buffer;

        // 预先缓存索引句柄，避免重复查找
        std::vector<IxIndexHandle*> index_handles;
        index_handles.reserve(tab_.indexes.size());

        // 预处理：缓存索引句柄并确定最大键值大小
        size_t max_key_size = 0;
        for (const auto& index : tab_.indexes) {
            auto ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
            index_handles.push_back(sm_manager_->ihs_.at(ix_name).get());
            max_key_size = std::max(max_key_size, static_cast<size_t>(index.col_tot_len));
        }

        // 确保缓冲区足够大
        if (key_buffer.size() < max_key_size) {
            key_buffer.resize(max_key_size);
        }

        // 记录失败位置
        int fail_pos = -1;

        // 遍历所有索引
        for (size_t i = 0; i < tab_.indexes.size(); ++i) {
            auto& index = tab_.indexes[i];
            auto* ih = index_handles[i];

            // 使用预分配的缓冲区构造索引键值
            char* key = key_buffer.data();
            int offset = 0;
            for (int j = 0; j < index.col_num; ++j) {
                memcpy(key + offset, rec->data + index.cols[j].offset, index.cols[j].len);
                offset += index.cols[j].len;
            }

            // 尝试插入新索引
            auto result = ih->insert_entry(key, rid, context_->txn_);

            if (result == -1) {
                fail_pos = i;
                break;
            }
        }

        if (fail_pos != -1) {
            // 插入失败，快速回滚已插入的索引
            for (int i = 0; i < fail_pos; ++i) {
                auto& index = tab_.indexes[i];
                auto* ih = index_handles[i];

                // 重新构造键值进行删除
                char* key = key_buffer.data();
                int offset = 0;
                for (int j = 0; j < index.col_num; ++j) {
                    memcpy(key + offset, rec->data + index.cols[j].offset, index.cols[j].len);
                    offset += index.cols[j].len;
                }
                ih->delete_entry(key, context_->txn_);
            }
            return false;
        }
        return true;
    }

    /**
     * @brief 执行UPDATE操作的核心方法（优化版本）
     *
     * @return std::unique_ptr<RmRecord> 对于UPDATE操作返回nullptr
     *
     * 执行流程优化：
     * 1. 预处理列元数据映射
     * 2. 批量准备记录并优化内存使用
     * 3. 高效的索引维护
     * 4. 最小化内存分配和复制
     */
    std::unique_ptr<RmRecord> Next() override {
        // 预处理：创建列元数据映射（只计算一次）
        std::map<TabCol, ColMeta> col_map;
        for (const auto& set_clause : set_clauses_) {
            ColMeta col = *get_col(tab_.cols, set_clause.lhs);
            col_map[set_clause.lhs] = col;
        }

        // 预分配记录向量
        std::vector<std::unique_ptr<RmRecord>> old_records;
        std::vector<std::unique_ptr<RmRecord>> new_records;
        const size_t record_count = rids_.size();
        old_records.reserve(record_count);
        new_records.reserve(record_count);

        // 第一阶段：优化的记录准备过程
        for (size_t i = 0; i < record_count; ++i) {
            const auto& rid = rids_[i];
            // 获取旧记录并创建新记录的副本
            auto old_rec = fh_->get_record(rid, context_);
            auto new_rec = fh_->get_record(rid, context_);

            // 优化的SET子句处理
            for (const auto& set_clause : set_clauses_) {
                const auto& col = col_map[set_clause.lhs];
                Value value;  // 声明 value 变量

                // 根据 rhs_type 获取值
                if (set_clause.rhs_type == SetRhsType::SET_RHS_VALUE) {
                    // 简单值赋值
                    value = set_clause.rhs_val;
                } else if (set_clause.rhs_type == SetRhsType::SET_RHS_EXPR) {
                    // 算术表达式赋值
                    ExprTerm temp_expr_term(set_clause.rhs_expr);
                    value = EvaluateExpr(temp_expr_term, *old_rec, tab_.cols);
                } else if (set_clause.rhs_type == SetRhsType::SET_RHS_COL) {
                    // 列引用赋值
                    const ColMeta* rhs_col_meta = nullptr;
                    for (const auto& meta : tab_.cols) {
                        if (meta.tab_name == set_clause.rhs_col.tab_name && meta.name == set_clause.rhs_col.col_name) {
                            rhs_col_meta = &meta;
                            break;
                        }
                    }
                    if (!rhs_col_meta) {
                        throw RMDBError("RHS column not found in SET clause: " + set_clause.rhs_col.tab_name + "." + set_clause.rhs_col.col_name);
                    }
                    value = GetColumnValue(*old_rec, *rhs_col_meta);
                } else {
                    throw RMDBError("Unsupported SetRhsType");
                }

                value.raw.reset();

                // 优化的类型转换
                if (value.type != col.type) {
                    switch (col.type) {
                        case ColType::TYPE_INT:
                            if (value.type == ColType::TYPE_FLOAT) {
                                value.set_int(static_cast<int>(value.float_val));
                            } else {
                                throw IncompatibleTypeError(coltype2str(col.type), coltype2str(value.type));
                            }
                            break;
                        case ColType::TYPE_FLOAT:
                            if (value.type == ColType::TYPE_INT) {
                                value.set_float(static_cast<float>(value.int_val));
                            } else {
                                throw IncompatibleTypeError(coltype2str(col.type), coltype2str(value.type));
                            }
                            break;
                        default:
                            throw IncompatibleTypeError(coltype2str(col.type), coltype2str(value.type));
                    }
                }

                // 直接更新新记录中的值
                value.init_raw(col.len);
                memcpy(new_rec->data + col.offset, value.raw->data, col.len);
            }

            old_records.emplace_back(std::move(old_rec));
            new_records.emplace_back(std::move(new_rec));
        }

        bool is_fail = false;
        size_t processed_records = 0;

        // 第二阶段：高效的批量处理
        for (size_t i = 0; i < record_count; ++i) {
            // 删除旧索引
            delete_index(old_records[i].get(), rids_[i]);
            processed_records++;

            // 尝试插入新索引
            if (!insert_index(new_records[i].get(), rids_[i])) {
                // 索引插入失败，需要恢复旧索引
                restore_index(old_records[i].get(), rids_[i]);
                is_fail = true;
                processed_records--;
                break;
            }

            // 更新记录数据
            fh_->update_record(rids_[i], new_records[i]->data, context_);

            // 记录写操作到事务日志
            if (context_ && context_->txn_) {
                WriteRecord* write_record = new WriteRecord(WType::UPDATE_TUPLE, tab_name_, rids_[i], *old_records[i]);
                context_->txn_->append_write_record(write_record);
            }
        }

        if (is_fail) {
            // 优化的回滚：批量恢复已处理的记录
            for (size_t i = processed_records; i > 0; --i) {
                size_t idx = i - 1;

                // 删除新索引
                delete_index(new_records[idx].get(), rids_[idx]);

                // 恢复旧索引
                restore_index(old_records[idx].get(), rids_[idx]);

                // 恢复记录数据
                fh_->update_record(rids_[idx], old_records[idx]->data, context_);
            }
            throw RMDBError("Update Error, rollback to previous state");
        }

        return nullptr;
    }

    /**
     * @brief 获取当前记录的RID
     * @return 抽象RID的引用
     */
    Rid& rid() override {
        return _abstract_rid;
    }

    /**
     * @brief 获取执行器的类型名称
     * @return std::string 返回执行器的类型标识
     */
    std::string getType() override {
        return "UpdateExecutor";
    }
};
