/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"

#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction*> TransactionManager::txn_map = {};

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
Transaction* TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    // Todo:
    // 1. 判断传入事务参数是否为空指针
    // 2. 如果为空指针，创建新事务
    // 3. 把开始事务加入到全局事务表中
    // 4. 返回当前事务指针
    if (txn == nullptr) {
        txn = new Transaction(next_txn_id_++);
    }
    std::unique_lock<std::mutex> lock(latch_);
    txn_map.emplace(txn->get_transaction_id(), txn);

    auto* log = new BeginLogRecord(txn->get_transaction_id());
    log->prev_lsn_ = txn->get_prev_lsn();
    log_manager->add_log_to_buffer(log);

    txn->set_prev_lsn(log->lsn_);

    return txn;
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    // Todo:
    // 1. 如果存在未提交的写操作，提交所有的写操作
    // 2. 释放所有锁
    // 3. 释放事务相关资源，eg.锁集
    // 4. 把事务日志刷入磁盘中
    // 5. 更新事务状态
    auto lock_set = txn->get_lock_set();
    for (auto i : *lock_set) {
        lock_manager_->unlock(txn, i);
    }
    txn->clear();

    auto* log = new CommitLogRecord(txn->get_transaction_id());
    log->prev_lsn_ = txn->get_prev_lsn();
    log_manager->add_log_to_buffer(log);
    txn->set_prev_lsn(log->lsn_);

    txn->set_state(TransactionState::COMMITTED);
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction* transaction, LogManager* log_manager) {
    if (transaction == nullptr) return;

    // 1. 回滚所有写操作（逆序执行）
    auto write_set = transaction->get_write_set();
    for (auto it = write_set->rbegin(); it != write_set->rend(); ++it) {
        WriteRecord* write_record = *it;

        switch (write_record->GetWriteType()) {
            case WType::INSERT_TUPLE: {
                // 插入操作的回滚：删除该记录
                auto fh = sm_manager_->fhs_.at(write_record->GetTableName()).get();
                // 先删除索引
                auto record = fh->get_record(write_record->GetRid(), nullptr);
                delete_record_in_index(transaction, write_record->GetTableName(),
                                       record.get(), write_record->GetRid());
                // 再删除记录
                fh->delete_record(write_record->GetRid(), nullptr);
                break;
            }
            case WType::DELETE_TUPLE: {
                // 删除操作的回滚：重新插入该记录
                auto fh = sm_manager_->fhs_.at(write_record->GetTableName()).get();
                // 重新插入记录
                fh->insert_record(write_record->GetRid(), write_record->GetRecord().data);
                // 重新插入索引
                insert_record_in_index(transaction, write_record->GetTableName(),
                                       &write_record->GetRecord(), write_record->GetRid());
                break;
            }
            case WType::UPDATE_TUPLE: {
                // 更新操作的回滚：恢复到原来的值
                auto fh = sm_manager_->fhs_.at(write_record->GetTableName()).get();
                // 删除新索引
                auto new_record = fh->get_record(write_record->GetRid(), nullptr);
                delete_record_in_index(transaction, write_record->GetTableName(),
                                       new_record.get(), write_record->GetRid());
                // 恢复记录
                fh->update_record(write_record->GetRid(), write_record->GetRecord().data, nullptr);
                // 重新插入旧索引
                insert_record_in_index(transaction, write_record->GetTableName(),
                                       &write_record->GetRecord(), write_record->GetRid());
                break;
            }
            default:
                throw InternalError("Unexpected write type");
        }
    }

    // 2. 释放所有锁
    auto lock_set = transaction->get_lock_set();
    for (auto i : *lock_set) {
        lock_manager_->unlock(transaction, i);
    }
    transaction->clear();
    // 4. 更新事务状态
    auto log = std::make_unique<AbortLogRecord>(transaction->get_transaction_id());
    log->prev_lsn_ = transaction->get_prev_lsn();
    log_manager->add_log_to_buffer(log.get());
    transaction->set_prev_lsn(log->lsn_);
    transaction->set_state(TransactionState::ABORTED);
}

/**
 * @description: 删除索引中的记录
 * @param {const std::string&} table_name 表名
 * @param {RmRecord*} rec 记录指针
 * @param {Rid} rid_ 记录的Rid
 */
void TransactionManager::delete_record_in_index(Transaction* transaction,
                                                const std::string& table_name,
                                                RmRecord* rec, Rid rid_) {
    auto& tab = sm_manager_->db_.get_table(table_name);
    for (auto& index : tab.indexes) {
        auto index_name = sm_manager_->get_ix_manager()->get_index_name(
            table_name, index.cols);
        auto index_handle = sm_manager_->get_index_handle(index_name);
        auto key = std::make_unique<char[]>(index.col_tot_len);
        int offset = 0;
        for (int j = 0; j < index.col_num; ++j) {
            memcpy(key.get() + offset, rec->data + index.cols[j].offset,
                   index.cols[j].len);
            offset += index.cols[j].len;
        }
        index_handle->delete_entry(key.get(), transaction);
    }
}

/**
 * @description: 在索引中插入记录
 * @param {const std::string&} table_name 表名
 * @param {RmRecord*} rec 记录指针
 * @param {Rid} rid_ 记录的Rid
 */
void TransactionManager::insert_record_in_index(Transaction* transaction,
                                                const std::string& table_name,
                                                RmRecord* rec, Rid rid_) {
    auto& tab = sm_manager_->db_.get_table(table_name);
    for (auto& index : tab.indexes) {
        auto index_name = sm_manager_->get_ix_manager()->get_index_name(
            table_name, index.cols);
        auto index_handle = sm_manager_->get_index_handle(index_name);
        auto key = std::make_unique<char[]>(index.col_tot_len);
        int offset = 0;
        for (int j = 0; j < index.col_num; ++j) {
            memcpy(key.get() + offset, rec->data + index.cols[j].offset,
                   index.cols[j].len);
            offset += index.cols[j].len;
        }
        index_handle->insert_entry(key.get(), rid_, transaction);
    }
}