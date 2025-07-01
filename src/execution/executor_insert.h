/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL
v2. You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

// 分支预测优化宏
#ifndef LIKELY
#define LIKELY(x) __builtin_expect(!!(x), 1)
#endif
#ifndef UNLIKELY
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

/**
 * @brief 插入执行器，负责实现INSERT语句的功能
 */
class InsertExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                // 表的元数据
    std::vector<Value> values_;  // 待插入的值列表
    RmFileHandle* fh_;           // 表的数据文件句柄
    std::string tab_name_;       // 表名
    Rid rid_;                    // 插入记录的位置(插入成功后赋值)
    SmManager* sm_manager_;      // 系统管理器指针

   public:
    /**
     * @brief 构造函数
     * @param sm_manager 系统管理器指针
     * @param tab_name 目标表名
     * @param values 要插入的值列表
     * @param context 执行上下文
     * @throw InvalidValueCountError 当值的数量与表的列数不匹配时
     */
    InsertExecutor(SmManager* sm_manager, const std::string& tab_name, std::vector<Value> values, Context* context) {
        sm_manager_ = sm_manager;
        tab_ = sm_manager_->db_.get_table(tab_name);
        values_ = values;
        tab_name_ = tab_name;
        // 检查插入值的数量是否与表的列数匹配
        if (values.size() != tab_.cols.size()) {
            throw InvalidValueCountError();
        }
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        context_ = context;
    };

    /**
     * @brief 为新记录创建所有索引项
     * @param rec 要创建索引的记录指针
     * @param rid 记录的RID
     * @return true表示所有索引创建成功，false表示失败
     */
    bool insert_index(RmRecord* rec, Rid rid) {
        // 预分配足够大的键值缓冲区，避免频繁内存分配
        static thread_local std::vector<char> key_buffer;

        // 预先缓存索引句柄，避免重复查找
        std::vector<IxIndexHandle*> index_handles;
        index_handles.reserve(tab_.indexes.size());

        // 预处理索引句柄和键值大小
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

        // 插入索引
        for (size_t i = 0; i < tab_.indexes.size(); ++i) {
            const auto& index = tab_.indexes[i];
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
                // 插入失败，回滚已插入的索引
                for (size_t rollback_i = 0; rollback_i < i; ++rollback_i) {
                    const auto& rollback_index = tab_.indexes[rollback_i];
                    auto* rollback_ih = index_handles[rollback_i];

                    // 重新构造键值进行删除
                    int rollback_offset = 0;
                    for (int j = 0; j < rollback_index.col_num; ++j) {
                        memcpy(key + rollback_offset, rec->data + rollback_index.cols[j].offset, rollback_index.cols[j].len);
                        rollback_offset += rollback_index.cols[j].len;
                    }
                    rollback_ih->delete_entry(key, context_->txn_);
                }
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 执行INSERT操作的核心方法
     *
     * @return std::unique_ptr<RmRecord> 对于INSERT操作返回nullptr
     *
     * 执行流程优化：
     * 1. 预分配记录缓冲区并直接写入
     * 2. 优化类型转换和数据复制
     * 3. 高效的索引维护
     * 4. 最小化内存分配和复制
     */
    std::unique_ptr<RmRecord> Next() override {
        // 预分配记录缓冲区
        RmRecord rec(fh_->get_file_hdr().record_size);

        // 数据填充
        for (size_t i = 0; i < values_.size(); ++i) {
            const auto& col = tab_.cols[i];
            auto& val = values_[i];

            // 类型不匹配的情况
            if (col.type != val.type) {
                switch (col.type) {
                    case ColType::TYPE_INT:
                        if (val.type == ColType::TYPE_FLOAT) {
                            val.set_int(static_cast<int>(val.float_val));
                        } else {
                            throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
                        }
                        break;
                    case ColType::TYPE_FLOAT:
                        if (val.type == ColType::TYPE_INT) {
                            val.set_float(static_cast<float>(val.int_val));
                        } else {
                            throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
                        }
                        break;
                    default:
                        throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
                }
            }

            // 复制值到记录中
            val.init_raw(col.len);
            memcpy(rec.data + col.offset, val.raw->data, col.len);
        }

        // 插入记录
        rid_ = fh_->insert_record(rec.data, context_);
        // 记录插入操作到事务日志
        if (context_ && context_->txn_) {
            WriteRecord* write_record = new WriteRecord(WType::INSERT_TUPLE, tab_name_, rid_, rec);
            context_->txn_->append_write_record(write_record);
        }
        // 索引插入，失败时回滚
        if (!insert_index(&rec, rid_)) {
            fh_->delete_record(rid_, context_);
            throw RMDBError("Insert Error, rollback to previous state");
        }

        return nullptr;
    }

    /**
     * @brief 获取插入记录的RID
     * @return 插入记录的RID引用
     */
    Rid& rid() override { return rid_; }

    /**
     * @brief 获取执行器类型名称
     * @return 执行器的类型字符串
     */
    std::string getType() override { return "InsertExecutor"; }
};
