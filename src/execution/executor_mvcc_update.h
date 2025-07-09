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

#include "execution_common.h"
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "parser/ast.h"
#include "recovery/log_manager.h"
#include "system/sm.h"

class MvccUpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                                     // 表的元数据
    std::vector<Condition> conds_;                    // update的where条件
    std::vector<SetClause> set_clauses_;              // update的set子句
    std::vector<Rid> rids_;                           // 需要更新的记录的位置
    std::string tab_name_;                            // 表名称
    std::vector<std::unique_ptr<RmRecord>> records_;  // 更新前的记录
    RmFileHandle *fh_;                                // 表的数据文件句柄
    SmManager *sm_manager_;
    TransactionManager *txn_mgr_;  // 事务管理器指针

   public:
    MvccUpdateExecutor(SmManager *sm_manager, TransactionManager *txn_mgr, const std::string &tab_name, std::vector<SetClause> set_clauses,
                       std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        txn_mgr_ = txn_mgr;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;

        // 间隙锁：如果没有找到匹配的记录，需要对WHERE条件加间隙锁
        // 防止其他事务插入满足条件的记录
        if (!conds_.empty()) {
            int tab_fd = fh_->GetFd();
            context_->lock_mgr_->lock_gap(context_->txn_, tab_fd, conds_);
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        for (auto &rid : rids_) {
            // 1. 可见性检查：跳过对当前事务不可见的记录
            if (!IsRecordVisible(rid, fh_, txn_mgr_, context_)) {
                continue;
            }

            // 2. 写锁冲突检测：如果有写锁冲突则抛出异常
            if (!CheckWriteConflict(rid, fh_, txn_mgr_, context_)) {
                throw TransactionAbortException(context_->txn_->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
            // 1. 获取当前记录用于生成新记录
            auto record = fh_->get_record(rid, context_);
            auto old_record = *record;
            records_.push_back(std::make_unique<RmRecord>(old_record));

            // Debug: 打印原始记录内容
            if (tab_.cols.size() >= 2) {
                int old_id = *(int *)(old_record.data + tab_.cols[0].offset);
                int old_score = *(int *)(old_record.data + tab_.cols[1].offset);
            }

            // 2. 构造新记录（根据SET子句修改相应字段）
            RmRecord new_record = old_record;
            for (const auto &set_clause : set_clauses_) {
                auto lhs_col = tab_.get_col(set_clause.lhs.col_name);

                Value value;
                if (set_clause.rhs_type == SET_RHS_VALUE) {
                    value = set_clause.rhs_val;
                } else if (set_clause.rhs_type == SET_RHS_EXPR) {
                    ExprTerm temp_expr_term(set_clause.rhs_expr);
                    value = EvaluateExpr(temp_expr_term, old_record, tab_.cols);
                } else if (set_clause.rhs_type == SET_RHS_COL) {
                    const ColMeta *rhs_col_meta = nullptr;
                    for (const auto &meta : tab_.cols) {
                        if (meta.tab_name == set_clause.rhs_col.tab_name && meta.name == set_clause.rhs_col.col_name) {
                            rhs_col_meta = &meta;
                            break;
                        }
                    }
                    if (!rhs_col_meta) {
                        throw RMDBError("RHS column not found in SET clause: " + set_clause.rhs_col.tab_name + "." +
                                        set_clause.rhs_col.col_name);
                    }
                    value = GetColumnValue(old_record, *rhs_col_meta);
                } else {
                    throw RMDBError("Unsupported SetRhsType");
                }

                // 类型转换
                if (lhs_col->type != value.type) {
                    if (lhs_col->type == ColType::TYPE_INT && value.type == ColType::TYPE_FLOAT) {
                        value.set_int(static_cast<int>(value.float_val));
                    } else if (lhs_col->type == ColType::TYPE_FLOAT && value.type == ColType::TYPE_INT) {
                        value.set_float(static_cast<float>(value.int_val));
                    } else if (lhs_col->type != value.type) {
                        throw IncompatibleTypeError(coltype2str(lhs_col->type), coltype2str(value.type));
                    }
                }

                value.raw.reset();
                value.init_raw(lhs_col->len);
                memcpy(new_record.data + lhs_col->offset, value.raw->data, lhs_col->len);
            }

            // Debug: 打印新记录内容
            if (tab_.cols.size() >= 2) {
                int new_id = *(int *)(new_record.data + tab_.cols[0].offset);
                int new_score = *(int *)(new_record.data + tab_.cols[1].offset);
            }

            // 3. 构造新记录的字段值
            std::vector<Value> new_values;
            for (size_t i = 0; i < tab_.cols.size(); ++i) {
                Value val;
                if (tab_.cols[i].type == TYPE_INT) {
                    val.set_int(*(int *)(new_record.data + tab_.cols[i].offset));
                } else if (tab_.cols[i].type == TYPE_FLOAT) {
                    val.set_float(*(float *)(new_record.data + tab_.cols[i].offset));
                } else if (tab_.cols[i].type == TYPE_STRING) {
                    std::string str_val(new_record.data + tab_.cols[i].offset, tab_.cols[i].len);
                    str_val = str_val.c_str();
                    val.set_str(str_val);
                }
                new_values.push_back(val);
            }
            // 4. 执行DELETE操作（删除旧记录）
            std::vector<Rid> delete_rids = {rid};
            std::vector<Condition> empty_conds;
            MvccDeleteExecutor delete_executor(sm_manager_, txn_mgr_, tab_name_, empty_conds, delete_rids, context_);
            delete_executor.Next();

            // 5. 执行INSERT操作（插入新记录）
            MvccInsertExecutor insert_executor(sm_manager_, txn_mgr_, tab_name_, new_values, context_);
            insert_executor.Next();
        }
        return nullptr;
    }

    Rid &rid() override {
        static Rid dummy_rid;
        return dummy_rid;
    }
};
