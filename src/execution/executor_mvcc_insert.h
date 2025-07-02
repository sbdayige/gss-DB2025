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
        for (size_t i = 0; i < values_.size(); ++i) {
            auto &col = tab_.cols[i];
            auto &val = values_[i];

            if (col.type != val.type) {
                throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
            }

            if (col.type == TYPE_STRING && val.str_val.size() > col.len) {
                throw StringOverflowError();
            }
        }
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
        std::cout << "DEBUG: Before insert_record - file pages: " << fh_->get_file_hdr().num_pages << std::endl;

        rid_ = fh_->insert_record(rec.data, context_);

        std::cout << "DEBUG: After insert_record - file pages: " << fh_->get_file_hdr().num_pages << std::endl;

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
};
