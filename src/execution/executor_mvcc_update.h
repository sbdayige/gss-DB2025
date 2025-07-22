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
            if (!context_->lock_mgr_->lock_gap(context_->txn_, tab_fd, conds_)) {
                throw TransactionAbortException(context_->txn_->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
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

            // 3. 获取当前记录用于生成新记录
            auto record = fh_->get_record(rid, context_);
            auto old_record = *record;
            records_.push_back(std::make_unique<RmRecord>(old_record));

            // 4. 构造新记录（根据SET子句修改相应字段）
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

                // 确保value有效且可以初始化raw数据
                if (!value.raw) {
                    value.raw.reset();
                    value.init_raw(lhs_col->len);
                }

                memcpy(new_record.data + lhs_col->offset, value.raw->data, lhs_col->len);
            }

            // 5. 检查唯一索引约束（在删除前检查）
            for (const auto &index : tab_.indexes) {
                auto key = std::make_unique<char[]>(index.col_tot_len);
                int offset = 0;
                for (size_t j = 0; j < static_cast<size_t>(index.col_num); ++j) {
                    memcpy(key.get() + offset, new_record.data + index.cols[j].offset, index.cols[j].len);
                    offset += index.cols[j].len;
                }

                // 检查索引是否已存在
                std::vector<Rid> result;
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                bool is_exist = ih->get_value(key.get(), &result, context_->txn_);

                if (is_exist) {
                    // 检查是否存在除当前记录外的其他记录
                    for (const auto &r : result) {
                        if (r.page_no != rid.page_no || r.slot_no != rid.slot_no) {
                            throw RMDBError("Failed to insert into index, rolled back record insertion at AbstractExecutor");
                        }
                    }
                }
            }

            // 6. 执行DELETE操作（删除旧记录和索引项）
            // 6.1 删除索引项
            for (const auto &index : tab_.indexes) {
                auto key = std::make_unique<char[]>(index.col_tot_len);
                int offset = 0;
                for (size_t j = 0; j < static_cast<size_t>(index.col_num); ++j) {
                    memcpy(key.get() + offset, old_record.data + index.cols[j].offset, index.cols[j].len);
                    offset += index.cols[j].len;
                }

                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                ih->delete_entry_with_rid(key.get(), rid, context_->txn_);
            }

            // 6.2 创建删除的undo log
            UndoLog undo_log = CreateDeleteUndoLog(old_record, rid, tab_, fh_, txn_mgr_, context_);
            context_->txn_->AppendUndoLog(undo_log);

            UndoLink new_link;
            new_link.prev_txn_ = context_->txn_->get_transaction_id();
            new_link.prev_log_idx_ = context_->txn_->GetUndoLogNum() - 1;

            VersionUndoLink version_link;
            version_link.prev_ = new_link;
            version_link.in_progress_ = true;

            txn_mgr_->UpdateVersionLink(fh_->GetFd(), rid, version_link, [](std::optional<VersionUndoLink>) { return true; });

            // 6.3 记录DELETE日志
            if (context_ && context_->txn_ && context_->log_mgr_) {
                DeleteLogRecord log_record(context_->txn_->get_transaction_id(), old_record, rid, tab_name_);
                log_record.prev_lsn_ = context_->txn_->get_prev_lsn();
                lsn_t lsn = context_->log_mgr_->add_log_to_buffer(&log_record);
                context_->txn_->set_prev_lsn(lsn);
            }

            WriteRecord *write_record = new WriteRecord(WType::DELETE_TUPLE, tab_name_, rid, old_record);
            context_->txn_->append_write_record(write_record);

            // 7. 执行INSERT操作（插入新记录和索引项）
            // 7.1 间隙锁冲突检测
            CheckGapLockConflict(new_record, tab_, fh_, context_);

            // 7.2 插入新记录到表中
            Rid new_rid = fh_->insert_record(new_record.data, context_);

            // 7.3 插入索引项
            for (const auto &index : tab_.indexes) {
                auto key = std::make_unique<char[]>(index.col_tot_len);
                int offset = 0;
                for (size_t j = 0; j < static_cast<size_t>(index.col_num); ++j) {
                    memcpy(key.get() + offset, new_record.data + index.cols[j].offset, index.cols[j].len);
                    offset += index.cols[j].len;
                }

                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                page_id_t res = ih->insert_entry(key.get(), new_rid, context_->txn_);

                if (res == INVALID_PAGE_ID) {
                    throw RMDBError("Failed to insert into index, rolled back record insertion at AbstractExecutor");
                }
            }

            // 7.4 创建INSERT操作的UndoLog
            UndoLog insert_undo_log = CreateInsertUndoLog(new_record, new_rid, tab_, txn_mgr_, context_);
            context_->txn_->AppendUndoLog(insert_undo_log);

            // 7.5 创建INSERT的版本链
            UndoLink insert_link;
            insert_link.prev_txn_ = context_->txn_->get_transaction_id();
            insert_link.prev_log_idx_ = context_->txn_->GetUndoLogNum() - 1;

            VersionUndoLink insert_version_link;
            insert_version_link.prev_ = insert_link;
            insert_version_link.in_progress_ = true;

            txn_mgr_->UpdateVersionLink(fh_->GetFd(), new_rid, insert_version_link, [](std::optional<VersionUndoLink>) { return true; });

            // 7.6 记录INSERT日志
            if (context_ && context_->txn_ && context_->log_mgr_) {
                InsertLogRecord log_record(context_->txn_->get_transaction_id(), new_record, new_rid, tab_name_);
                log_record.prev_lsn_ = context_->txn_->get_prev_lsn();
                lsn_t lsn = context_->log_mgr_->add_log_to_buffer(&log_record);
                context_->txn_->set_prev_lsn(lsn);
            }

            WriteRecord *insert_write_record = new WriteRecord(WType::INSERT_TUPLE, tab_name_, new_rid, new_record);
            context_->txn_->append_write_record(insert_write_record);
        }
        return nullptr;
    }

    Rid &rid() override {
        static Rid dummy_rid;
        return dummy_rid;
    }
};
