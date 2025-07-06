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
#include "recovery/log_manager.h"
#include "system/sm.h"

class MvccInsertExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                // 表的元数据
    std::vector<Value> values_;  // 需要插入的数据
    RmFileHandle *fh_;           // 表的数据文件句柄
    std::string tab_name_;       // 表名称
    Rid rid_;                    // 插入的位置，在字段不为空时赋值
    SmManager *sm_manager_;
    TransactionManager *txn_mgr_;  // 事务管理器指针

   public:
    MvccInsertExecutor(SmManager *sm_manager, TransactionManager *txn_mgr, const std::string &tab_name, std::vector<Value> values, Context *context) {
        sm_manager_ = sm_manager;
        txn_mgr_ = txn_mgr;
        tab_ = sm_manager_->db_.get_table(tab_name);
        values_ = values;
        tab_name_ = tab_name;
        context_ = context;
        fh_ = sm_manager_->fhs_.at(tab_name).get();
    };

    std::unique_ptr<RmRecord> Next() override {
        if (values_.size() != tab_.cols.size()) {
            throw InvalidValueCountError();
        }

        // 间隙锁冲突检测：检查插入的记录是否与现有的间隙锁冲突
        for (size_t i = 0; i < values_.size(); ++i) {
            auto &col = tab_.cols[i];
            auto &val = values_[i];
            // 类型兼容：float->int、int->float自动转换
            if (col.type == TYPE_INT && val.type == TYPE_FLOAT) {
                int v = (int)val.float_val;
                val.set_int(v);
            } else if (col.type == TYPE_FLOAT && val.type == TYPE_INT) {
                float v = (float)val.int_val;
                val.set_float(v);
            } else if (col.type != val.type) {
                throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
            }
            if (col.type == TYPE_STRING && val.str_val.size() > col.len) {
                throw StringOverflowError();
            }
        }

        CheckGapLockConflict();

        RmRecord rec(fh_->get_file_hdr().record_size);
        for (size_t i = 0; i < values_.size(); ++i) {
            auto &col = tab_.cols[i];
            auto &val = values_[i];
            val.init_raw(col.len);
            memcpy(rec.data + col.offset, val.raw->data, col.len);
        }
        for (auto &index : tab_.indexes) {
            auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();

            // 构造索引键
            char *key = new char[index.col_tot_len];
            int offset = 0;
            for (int j = 0; j < index.col_num; ++j) {
                memcpy(key + offset, rec.data + index.cols[j].offset, index.cols[j].len);
                offset += index.cols[j].len;
            }

            // MVCC: 检查键是否已存在（考虑版本可见性）
            if (CheckKeyExistsWithMvcc(ih, key, index.col_tot_len)) {
                delete[] key;
                throw InternalError("Duplicate key");
            }

            delete[] key;
        }

        rid_ = fh_->insert_record(rec.data, context_);

        // WAL: 记录INSERT日志
        if (context_ && context_->txn_ && context_->log_mgr_) {
            InsertLogRecord log_record(context_->txn_->get_transaction_id(), rec, rid_, tab_name_);
            log_record.prev_lsn_ = context_->txn_->get_prev_lsn();
            lsn_t lsn = context_->log_mgr_->add_log_to_buffer(&log_record);
            context_->txn_->set_prev_lsn(lsn);
        }

        for (auto &index : tab_.indexes) {
            auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();

            char *key = new char[index.col_tot_len];
            int offset = 0;
            for (int j = 0; j < index.col_num; ++j) {
                memcpy(key + offset, rec.data + index.cols[j].offset, index.cols[j].len);
                offset += index.cols[j].len;
            }

            ih->insert_entry(key, rid_, context_->txn_);
            delete[] key;
        }

        WriteRecord *write_record = new WriteRecord(WType::INSERT_TUPLE, tab_name_, rid_, rec);
        context_->txn_->append_write_record(write_record);

        // MVCC: 创建版本链
        UndoLink insert_link;
        insert_link.prev_txn_ = context_->txn_->get_transaction_id();
        insert_link.prev_log_idx_ = context_->txn_->GetUndoLogNum();  // 将要添加的UndoLog的索引

        // 创建UndoLog（INSERT操作的UndoLog记录被插入的记录）
        UndoLog undo_log;
        undo_log.is_deleted_ = false;  // INSERT操作，撤销时需要删除
        undo_log.tuple_test_ = new RmRecord(rec);
        // 分配全局唯一递增的写时间戳
        undo_log.ts_ = txn_mgr_->get_next_timestamp();
        undo_log.txn_id = context_->txn_->get_transaction_id();
        undo_log.modified_fields_.resize(tab_.cols.size(), true);  // INSERT影响所有字段
        undo_log.prev_version_.prev_txn_ = INVALID_TXN_ID;         // INSERT操作没有前一个版本
        undo_log.prev_version_.prev_log_idx_ = -1;
        undo_log.tuple_.clear();
        for (size_t i = 0; i < tab_.cols.size(); ++i) {
            Value val;
            if (tab_.cols[i].type == TYPE_INT) {
                val.set_int(*(int *)(rec.data + tab_.cols[i].offset));
            } else if (tab_.cols[i].type == TYPE_FLOAT) {
                val.set_float(*(float *)(rec.data + tab_.cols[i].offset));
            } else if (tab_.cols[i].type == TYPE_STRING) {
                std::string str_val(rec.data + tab_.cols[i].offset, tab_.cols[i].len);
                str_val = str_val.c_str();
                val.set_str(str_val);
            }
            undo_log.tuple_.push_back(val);
        }

        context_->txn_->AppendUndoLog(undo_log);

        // 创建版本链
        VersionUndoLink version_link;
        version_link.prev_ = insert_link;
        version_link.in_progress_ = true;

        txn_mgr_->UpdateVersionLink(rid_, version_link, [](std::optional<VersionUndoLink>) { return true; });

        std::cout << "DEBUG: INSERT created version link for rid " << rid_.page_no << "," << rid_.slot_no
                  << " with txn " << context_->txn_->get_transaction_id() << std::endl;

        return nullptr;
    }

    Rid &rid() override { return rid_; }

   private:
    bool CheckKeyExistsWithMvcc(IxIndexHandle *ih, char *key, int key_len) {
        // MVCC版本：检查键是否在当前事务的快照中可见
        std::vector<Rid> result;
        bool found = ih->get_value(key, &result, context_->txn_);

        if (!found) {
            return false;  // 键不存在
        }

        for (auto &rid : result) {
            if (IsVisible(rid, context_->txn_)) {
                return true;
            }
        }
        return false;
    }

    bool IsVisible(const Rid &rid, Transaction *txn) {
        // 检查记录版本是否对当前事务可见
        auto version_link = txn_mgr_->GetVersionLink(rid);
        if (!version_link.has_value()) {
            // 没有版本链，说明是基础版本，可见
            return true;
        }

        // 检查版本链中的事务状态
        auto prev_txn_id = version_link->prev_.prev_txn_;
        if (prev_txn_id == INVALID_TXN_ID) {
            return true;
        }

        if (prev_txn_id == txn->get_transaction_id()) {
            // 自己的修改，可见
            return true;
        }

        // 检查其他事务的状态
        auto prev_txn = GetTransaction(prev_txn_id);
        if (!prev_txn) {
            // 事务不存在，可能已经提交或回滚
            return true;
        }

        if (prev_txn->get_state() == TransactionState::COMMITTED) {
            // 已提交的事务，检查时间戳
            return prev_txn->get_commit_ts() < txn->get_read_ts();
        }

        // 未提交的事务，不可见
        return false;
    }

    Transaction *GetTransaction(txn_id_t txn_id) {
        return txn_mgr_->find_transaction(txn_id);
    }

    void CheckGapLockConflict() {
        // 检查插入的记录是否与现有的间隙锁冲突
        int tab_fd = fh_->GetFd();
        auto gap_conditions = context_->lock_mgr_->get_gap_condition(tab_fd, context_->txn_);

        if (!gap_conditions.empty()) {
            // 为插入的记录构造一个临时记录用于条件检查
            RmRecord temp_record(fh_->get_file_hdr().record_size);
            for (size_t i = 0; i < values_.size(); ++i) {
                auto &col = tab_.cols[i];
                auto &val = values_[i];
                if (!val.raw) {
                    val.init_raw(col.len);
                }
                memcpy(temp_record.data + col.offset, val.raw->data, col.len);
            }

            // 检查插入的记录是否满足任何间隙锁条件
            for (const auto &condition : gap_conditions) {
                if (EvaluateCondition(temp_record, condition, tab_)) {
                    // 插入的记录满足其他事务的间隙锁条件，触发写冲突
                    throw TransactionAbortException(context_->txn_->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
                }
            }
        }
    }
};
