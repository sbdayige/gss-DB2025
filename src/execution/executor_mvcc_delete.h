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

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "recovery/log_manager.h"
#include "system/sm.h"

class MvccDeleteExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                                     // 表的元数据
    std::vector<Condition> conds_;                    // delete的where条件
    std::vector<Rid> rids_;                           // 需要删除的记录的位置
    std::string tab_name_;                            // 表名称
    std::vector<std::unique_ptr<RmRecord>> records_;  // 删除前的记录
    RmFileHandle *fh_;                                // 表的数据文件句柄
    SmManager *sm_manager_;
    TransactionManager *txn_mgr_;  // 事务管理器指针

   public:
    MvccDeleteExecutor(SmManager *sm_manager, TransactionManager *txn_mgr, const std::string &tab_name, std::vector<Condition> conds,
                       std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        txn_mgr_ = txn_mgr;
        tab_name_ = tab_name;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;

        // 间隙锁：如果没有找到匹配的记录，需要加间隙锁防止其他事务插入满足删除条件的记录
        if (!conds_.empty()) {
            // DELETE语句有WHERE条件但没找到匹配记录，加间隙锁
            if (!context_->lock_mgr_->lock_gap(context_->txn_, fh_->GetFd(), conds_)) {
                throw TransactionAbortException(context_->txn_->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
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

            auto record = fh_->get_record(rid, context_);

            auto delete_record = *record;
            records_.push_back(std::make_unique<RmRecord>(delete_record));
            UndoLog undo_log;
            undo_log.is_deleted_ = true;  // 表示这是删除操作
            undo_log.tuple_test_ = new RmRecord(delete_record);
            // 分配全局唯一递增的写时间戳
            undo_log.ts_ = txn_mgr_->get_next_timestamp();
            undo_log.txn_id = context_->txn_->get_transaction_id();

            // 设置前一个版本的链接信息
            auto old_version_link = txn_mgr_->GetVersionLink(fh_->GetFd(), rid);
            if (old_version_link.has_value()) {
                undo_log.prev_version_ = old_version_link->prev_;
            } else {
                undo_log.prev_version_.prev_txn_ = INVALID_TXN_ID;
                undo_log.prev_version_.prev_log_idx_ = -1;
            }

            // 填充修改字段信息（删除操作影响所有字段）
            undo_log.modified_fields_.resize(tab_.cols.size(), true);
            undo_log.tuple_.clear();
            for (size_t i = 0; i < tab_.cols.size(); ++i) {
                Value val;
                // 从记录数据中提取值
                if (tab_.cols[i].type == TYPE_INT) {
                    val.set_int(*(int *)(delete_record.data + tab_.cols[i].offset));
                } else if (tab_.cols[i].type == TYPE_FLOAT) {
                    val.set_float(*(float *)(delete_record.data + tab_.cols[i].offset));
                } else if (tab_.cols[i].type == TYPE_STRING) {
                    std::string str_val(delete_record.data + tab_.cols[i].offset, tab_.cols[i].len);
                    // 移除末尾的空字符
                    str_val = str_val.c_str();
                    val.set_str(str_val);
                }
                undo_log.tuple_.push_back(val);
            }

            context_->txn_->AppendUndoLog(undo_log);

            UndoLink new_link;
            new_link.prev_txn_ = context_->txn_->get_transaction_id();
            new_link.prev_log_idx_ = context_->txn_->GetUndoLogNum() - 1;

            auto old_link = txn_mgr_->GetUndoLink(fh_->GetFd(), rid);
            VersionUndoLink version_link;
            version_link.prev_ = new_link;
            version_link.in_progress_ = true;

            txn_mgr_->UpdateVersionLink(fh_->GetFd(), rid, version_link, [](std::optional<VersionUndoLink>) { return true; });

            MarkRecordAsDeleted(rid);

            // WAL: 记录DELETE日志
            if (context_ && context_->txn_ && context_->log_mgr_) {
                DeleteLogRecord log_record(context_->txn_->get_transaction_id(), delete_record, rid, tab_name_);
                log_record.prev_lsn_ = context_->txn_->get_prev_lsn();
                lsn_t lsn = context_->log_mgr_->add_log_to_buffer(&log_record);
                context_->txn_->set_prev_lsn(lsn);
            }

            DeleteFromIndices(delete_record, rid);

            WriteRecord *write_record = new WriteRecord(WType::DELETE_TUPLE, tab_name_, rid, delete_record);
            context_->txn_->append_write_record(write_record);
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
        auto version_link = txn_mgr_->GetVersionLink(fh_->GetFd(), rid);
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
        auto version_link = txn_mgr_->GetVersionLink(fh_->GetFd(), rid);
        if (!version_link.has_value()) {
            return true;  // 没有版本链，可以删除
        }

        auto prev_txn_id = version_link->prev_.prev_txn_;
        if (prev_txn_id == INVALID_TXN_ID) {
            return true;  // 基础版本，可以删除
        }

        if (prev_txn_id == context_->txn_->get_transaction_id()) {
            return true;  // 自己的修改，可以删除
        }

        // 检查其他事务的状态
        auto prev_txn = txn_mgr_->find_transaction(prev_txn_id);
        if (!prev_txn) {
            return true;  // 事务不存在，可能已经提交
        }

        if (prev_txn->get_state() == TransactionState::COMMITTED) {
            // 已提交的事务，检查时间戳冲突
            if (prev_txn->get_commit_ts() > context_->txn_->get_read_ts()) {
                return false;  // 写入冲突
            }
            return true;
        } else if (prev_txn->get_state() == TransactionState::ABORTED) {
            return true;  // ABORTED的事务不应该造成冲突
        }
        return false;
    }

    void MarkRecordAsDeleted(const Rid &rid) {
        // 在MVCC中，DELETE操作不应该物理删除记录，只在版本链中标记删除
        // 物理删除由垃圾回收器在事务提交后执行
        // 这里不需要做任何操作，删除标记已经在版本链和UndoLog中设置了
    }

    void DeleteFromIndices(const RmRecord &record, const Rid &rid) {
        // 从所有索引中删除记录
        for (auto &index : tab_.indexes) {
            auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();

            // 构造索引键
            char *key = new char[index.col_tot_len];
            int offset = 0;
            for (int j = 0; j < index.col_num; ++j) {
                memcpy(key + offset, record.data + index.cols[j].offset, index.cols[j].len);
                offset += index.cols[j].len;
            }

            // 从索引中删除
            ih->delete_entry(key, context_->txn_);

            delete[] key;
        }
    }

    bool IsVisible(const Rid &rid, Transaction *txn) {
        // DELETE操作需要看到所有记录（包括未提交的INSERT）以便进行写冲突检测
        // 这与SELECT的可见性规则不同，DELETE必须能看到所有存在的记录
        auto version_link = txn_mgr_->GetVersionLink(fh_->GetFd(), rid);
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

        // 对于其他事务的修改（包括未提交的INSERT），DELETE操作都应该能看到
        // 这样才能在CheckWriteConflict中正确检测写冲突并触发回滚
        return true;
    }

    std::unique_ptr<UndoLog> GetUndoLog(const UndoLink &link) {
        auto undo_log_opt = txn_mgr_->GetUndoLogOptional(link);
        if (undo_log_opt.has_value()) {
            return std::make_unique<UndoLog>(undo_log_opt.value());
        }
        return nullptr;
    }
};
