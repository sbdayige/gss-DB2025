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
    }

    std::unique_ptr<RmRecord> Next() override {
        std::cout << "DEBUG: MvccDeleteExecutor::Next() started, processing " << rids_.size() << " records" << std::endl;

        for (auto &rid : rids_) {
            std::cout << "DEBUG: Processing DELETE for rid: " << rid.page_no << "," << rid.slot_no << std::endl;

            if (!CheckWriteConflict(rid)) {
                std::cout << "DEBUG: Write conflict detected for rid: " << rid.page_no << "," << rid.slot_no << std::endl;
                throw TransactionAbortException(context_->txn_->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }

            auto record = fh_->get_record(rid, context_);
            std::cout << "DEBUG: Retrieved record for deletion" << std::endl;

            auto delete_record = *record;
            records_.push_back(std::make_unique<RmRecord>(delete_record));
            UndoLog undo_log;
            undo_log.is_deleted_ = true;  // 表示这是删除操作
            undo_log.tuple_test_ = new RmRecord(delete_record);
            undo_log.ts_ = context_->txn_->get_start_ts();
            std::cout << "DEBUG: Created undo log with is_deleted_ = true" << std::endl;

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
            std::cout << "DEBUG: Appended undo log to transaction" << std::endl;

            UndoLink new_link;
            new_link.prev_txn_ = context_->txn_->get_transaction_id();
            new_link.prev_log_idx_ = context_->txn_->GetUndoLogNum() - 1;
            std::cout << "DEBUG: Created new undo link: txn=" << new_link.prev_txn_ << ", log_idx=" << new_link.prev_log_idx_ << std::endl;

            auto old_link = txn_mgr_->GetUndoLink(rid);
            VersionUndoLink version_link;
            version_link.prev_ = new_link;
            version_link.in_progress_ = true;

            txn_mgr_->UpdateVersionLink(rid, version_link, [](std::optional<VersionUndoLink>) { return true; });
            std::cout << "DEBUG: Updated version link for rid: " << rid.page_no << "," << rid.slot_no << std::endl;

            MarkRecordAsDeleted(rid);
            std::cout << "DEBUG: Marked record as deleted" << std::endl;

            DeleteFromIndices(delete_record, rid);
            std::cout << "DEBUG: Deleted from indices" << std::endl;

            WriteRecord *write_record = new WriteRecord(WType::DELETE_TUPLE, tab_name_, rid, delete_record);
            context_->txn_->append_write_record(write_record);
            std::cout << "DEBUG: Added write record to transaction" << std::endl;
        }

        std::cout << "DEBUG: MvccDeleteExecutor::Next() completed successfully" << std::endl;
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
        }

        // 未提交的事务，写入冲突
        return false;
    }

    void MarkRecordAsDeleted(const Rid &rid) {
        // 在MVCC中，删除通常是逻辑删除
        // 这里可以实现一个删除标记位
        // 由于当前框架没有提供删除标记，我们使用版本链来表示删除状态

        // 实际实现中，可能需要在记录头部添加删除标记
        // 或者通过版本链来判断记录是否被删除
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
        auto version_link = txn_mgr_->GetVersionLink(rid);
        if (!version_link.has_value()) {
            return true;
        }

        auto prev_txn_id = version_link->prev_.prev_txn_;
        if (prev_txn_id == INVALID_TXN_ID) {
            return true;
        }

        if (prev_txn_id == txn->get_transaction_id()) {
            return true;
        }

        auto prev_txn = txn_mgr_->find_transaction(prev_txn_id);
        if (!prev_txn) {
            return true;
        }

        if (prev_txn->get_state() == TransactionState::COMMITTED) {
            return prev_txn->get_commit_ts() < txn->get_read_ts();
        }

        return false;
    }
};
