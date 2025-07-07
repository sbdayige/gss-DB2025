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
            if (!IsRecordVisible(rid)) {
                continue;
            }

            // 2. 写锁冲突检测：如果有写锁冲突则抛出异常
            if (!CheckWriteConflict(rid)) {
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

   private:
    /**
     * 检查记录对当前事务的可见性
     * 使用与SELECT相同的MVCC可见性规则
     */
    bool IsRecordVisible(const Rid &rid) {
        auto version_link = txn_mgr_->GetVersionLink(rid);
        if (!version_link.has_value()) {
            return true;  // 没有版本链，基础版本可见
        }

        UndoLink link = version_link->prev_;
        while (true) {
            if (link.prev_txn_ == INVALID_TXN_ID) {
                // 回溯到链尾，基础版本可见
                return true;
            }

            auto undo_log_opt = txn_mgr_->GetUndoLogOptional(link);
            if (!undo_log_opt.has_value()) {
                break;
            }
            auto &undo_log = undo_log_opt.value();

            // 1. 本事务产生的版本
            if (link.prev_txn_ == context_->txn_->get_transaction_id()) {
                if (undo_log.is_deleted_) {
                    return false;  // 本事务DELETE，记录不可见
                } else {
                    return true;  // 本事务INSERT/UPDATE，记录可见
                }
            }

            // 2. 其他事务产生的版本
            Transaction *log_txn = txn_mgr_->find_transaction(link.prev_txn_);
            if (log_txn && log_txn->get_state() == TransactionState::COMMITTED) {
                // 已提交版本：只有提交时间早于当前事务start_ts才可见
                if (log_txn->get_commit_ts() < context_->txn_->get_start_ts()) {
                    if (undo_log.is_deleted_) {
                        return false;  // 已被删除，不可见
                    }
                    return true;  // 可见已提交版本
                } else {
                    // 提交时间晚于start_ts，继续回溯
                    link = undo_log.prev_version_;
                    continue;
                }
            } else {
                // 未提交或已回滚的事务，继续回溯
                link = undo_log.prev_version_;
                continue;
            }
        }
        return false;  // 没有可见版本
    }

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
