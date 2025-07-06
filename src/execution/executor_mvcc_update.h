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
        // MVCC Update实现：UPDATE = DELETE + INSERT
        for (auto &rid : rids_) {
            // 1. 获取当前记录用于生成新记录
            auto record = fh_->get_record(rid, context_);
            auto old_record = *record;
            records_.push_back(std::make_unique<RmRecord>(old_record));

            // 2. 构造新记录（根据SET子句修改相应字段）
            RmRecord new_record = old_record;
            for (auto &set_clause : set_clauses_) {
                auto lhs_col = tab_.get_col(set_clause.lhs.col_name);

                if (set_clause.rhs_type == SET_RHS_VALUE) {
                    if (lhs_col->type == TYPE_INT && set_clause.rhs_val.type == TYPE_FLOAT) {
                        int v = (int)set_clause.rhs_val.float_val;
                        memcpy(new_record.data + lhs_col->offset, &v, lhs_col->len);
                    } else if (lhs_col->type == TYPE_FLOAT && set_clause.rhs_val.type == TYPE_INT) {
                        float v = (float)set_clause.rhs_val.int_val;
                        memcpy(new_record.data + lhs_col->offset, &v, lhs_col->len);
                    } else if (lhs_col->type != set_clause.rhs_val.type) {
                        throw IncompatibleTypeError(coltype2str(lhs_col->type), coltype2str(set_clause.rhs_val.type));
                    } else {
                        if (!set_clause.rhs_val.raw) {
                            set_clause.rhs_val.init_raw(lhs_col->len);
                        }
                        memcpy(new_record.data + lhs_col->offset, set_clause.rhs_val.raw->data, lhs_col->len);
                    }
                } else if (set_clause.rhs_type == SET_RHS_COL) {
                    auto rhs_col = tab_.get_col(set_clause.rhs_col.col_name);
                    if (lhs_col->type != rhs_col->type) {
                        throw IncompatibleTypeError(coltype2str(lhs_col->type), coltype2str(rhs_col->type));
                    }
                    memcpy(new_record.data + lhs_col->offset, new_record.data + rhs_col->offset, lhs_col->len);
                } else if (set_clause.rhs_type == SET_RHS_EXPR) {
                    Value expr_result = EvaluateArithExpr(set_clause.rhs_expr, new_record, tab_.cols);
                    if (lhs_col->type == TYPE_INT && expr_result.type == TYPE_FLOAT) {
                        expr_result.set_int((int)expr_result.float_val);
                    } else if (lhs_col->type == TYPE_FLOAT && expr_result.type == TYPE_INT) {
                        expr_result.set_float((float)expr_result.int_val);
                    } else if (lhs_col->type != expr_result.type) {
                        throw IncompatibleTypeError(coltype2str(lhs_col->type), coltype2str(expr_result.type));
                    }
                    if (!expr_result.raw) {
                        expr_result.init_raw(lhs_col->len);
                    }
                    memcpy(new_record.data + lhs_col->offset, expr_result.raw->data, lhs_col->len);
                }
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

   private:
    bool CheckWriteConflict(const Rid &rid) {
        auto version_link = txn_mgr_->GetVersionLink(rid);
        if (!version_link.has_value()) {
            return true;  // 没有版本链，可以更新
        }
        auto prev_txn_id = version_link->prev_.prev_txn_;

        if (prev_txn_id == INVALID_TXN_ID) {
            return true;  // 基础版本，可以更新
        }

        if (prev_txn_id == context_->txn_->get_transaction_id()) {
            return true;  // 自己的修改，可以更新
        }

        // 检查其他事务的状态
        auto prev_txn = txn_mgr_->find_transaction(prev_txn_id);
        if (!prev_txn) {
            return true;  // 事务不存在，可能已经提交
        }

        if (prev_txn->get_state() == TransactionState::COMMITTED) {
            if (prev_txn->get_commit_ts() > context_->txn_->get_read_ts()) {
                return false;  // 写入冲突
            }
            return true;
        } else if (prev_txn->get_state() == TransactionState::ABORTED) {
            return true;  // ABORTED的事务不应该造成冲突
        }

        return false;
    }

    bool CheckUniqueConstraintWithMvcc(IxIndexHandle *ih, char *key, int key_len, const Rid &exclude_rid) {
        std::vector<Rid> result;
        bool found = ih->get_value(key, &result, context_->txn_);

        if (!found) {
            return false;  // 键不存在，没有重复
        }

        for (auto &rid : result) {
            if (rid == exclude_rid) continue;  // 排除自己

            if (IsVisible(rid, context_->txn_)) {
                return true;  // 发现重复键
            }
        }
        return false;
    }

    bool IsVisible(const Rid &rid, Transaction *txn) {
        // UPDATE操作需要看到所有记录（包括未提交的INSERT）以便进行写冲突检测
        // 这与SELECT的可见性规则不同，UPDATE必须能看到所有存在的记录
        auto version_link = txn_mgr_->GetVersionLink(rid);
        if (!version_link.has_value()) {
            return true;  // 没有版本链，记录可见
        }

        auto prev_txn_id = version_link->prev_.prev_txn_;
        if (prev_txn_id == INVALID_TXN_ID) {
            return true;  // 无效事务ID，记录可见
        }

        if (prev_txn_id == txn->get_transaction_id()) {
            // 自己的修改，检查是否被删除
            auto undo_log = GetUndoLog(version_link->prev_);
            if (undo_log && undo_log->is_deleted_) {
                return false;  // 被自己删除了，不可见
            }
            return true;  // 自己的修改，可见
        }

        return true;
    }

    std::unique_ptr<UndoLog> GetUndoLog(const UndoLink &link) {
        auto undo_log_opt = txn_mgr_->GetUndoLogOptional(link);
        if (undo_log_opt.has_value()) {
            return std::make_unique<UndoLog>(undo_log_opt.value());
        }
        return nullptr;
    }

    void UpdateIndicesForMvcc(const RmRecord &old_record, const RmRecord &new_record, const Rid &rid) {
        // 更新所有索引
        for (auto &index : tab_.indexes) {
            auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();

            // 构造旧索引键
            char *old_key = new char[index.col_tot_len];
            int offset = 0;
            for (int j = 0; j < index.col_num; ++j) {
                memcpy(old_key + offset, old_record.data + index.cols[j].offset, index.cols[j].len);
                offset += index.cols[j].len;
            }

            // 构造新索引键
            char *new_key = new char[index.col_tot_len];
            offset = 0;
            for (int j = 0; j < index.col_num; ++j) {
                memcpy(new_key + offset, new_record.data + index.cols[j].offset, index.cols[j].len);
                offset += index.cols[j].len;
            }

            // 如果键值不同，需要更新索引
            if (memcmp(old_key, new_key, index.col_tot_len) != 0) {
                ih->delete_entry(old_key, context_->txn_);
                ih->insert_entry(new_key, rid, context_->txn_);
            }

            delete[] old_key;
            delete[] new_key;
        }
    }
};
