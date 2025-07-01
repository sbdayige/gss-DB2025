/**
 * @file transaction_manager.cpp
 * @author RMDB Development Team
 * @brief 事务管理器实现文件
 * @version 0.1
 * @date 2023-12-01
 *
 * @copyright Copyright (c) 2023 Renmin University of China
 * @license Mulan PSL v2 (http://license.coscl.org.cn/MulanPSL2)
 *
 * 该文件实现了事务管理器的核心功能，包括：
 * - 事务的创建、提交和回滚
 * - 并发控制（2PL、MVCC）
 * - 日志管理与恢复
 * - 锁管理
 *
 * 事务管理器是数据库系统的核心组件之一，负责保证：
 * 1. 原子性(Atomicity)：事务要么完全执行，要么完全不执行
 * 2. 一致性(Consistency)：事务执行前后数据库状态保持一致
 * 3. 隔离性(Isolation)：并发事务互不干扰
 * 4. 持久性(Durability)：已提交事务的修改永久保存
 */

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
    // 如果需要支持MVCC请在上述过程中添加代码

    if (txn == nullptr) {
        // 创建新事务，并分配递增的事务ID
        txn = new Transaction(next_txn_id_.fetch_add(1));
    }

    // 将当前事务添加到全局事务映射表中，便于后续通过事务ID查找
    std::unique_lock<std::mutex> lock(latch_);
    txn_map.emplace(txn->get_transaction_id(), txn);
    lock.unlock();

    timestamp_t start_ts = next_timestamp_.fetch_add(1);
    txn->set_start_ts(start_ts);  // 设置事务开始时间戳
    txn->set_txn_mode(false);
    txn->set_state(TransactionState::DEFAULT);

    // MVCC相关设置
    if (concurrency_mode_ == ConcurrencyMode::MVCC) {
        running_txns_.AddTxn(start_ts);  // 添加事务到水位线
        AssignReadTimestamp(txn);        // 分配读时间戳
    }

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
    // 如果需要支持MVCC请在上述过程中添加代码

    txn->set_state(TransactionState::COMMITTED);

    // MVCC: 分配提交时间戳
    timestamp_t commit_ts = INVALID_TS;
    if (concurrency_mode_ == ConcurrencyMode::MVCC) {
        commit_ts = next_timestamp_.fetch_add(1);
        last_commit_ts_.store(std::max(last_commit_ts_.load(), commit_ts));
    }

    std::shared_ptr<std::unordered_set<LockDataId>> lock_set = txn->get_lock_set();

    auto lock_set_copy = *lock_set;  // 复制锁集合以避免迭代时修改
    for (const LockDataId& lock : lock_set_copy) {
        lock_manager_->unlock(txn, lock);
    }

    auto lock_gap_set = txn->get_lock_gap_set();
    auto lock_gap_set_copy = *lock_gap_set;  // 复制间隙锁集合以避免迭代时修改
    for (const int& tab_fd : lock_gap_set_copy) {
        lock_manager_->unlock_gap(txn, tab_fd);
    }

    // 清空事务相关的集合
    txn->clear();

    // MVCC相关清理
    if (concurrency_mode_ == ConcurrencyMode::MVCC) {
        running_txns_.UpdateCommitTs(commit_ts);      // 更新水位线的提交时间戳
        running_txns_.RemoveTxn(txn->get_read_ts());  // 从水位线中移除事务
    }
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction* txn, LogManager* log_manager) {
    // Todo:
    // 1. 回滚所有写操作
    // 2. 释放所有锁
    // 3. 清空事务相关资源，eg.锁集
    // 4. 把事务日志刷入磁盘中
    // 5. 更新事务状态
    // 如果需要支持MVCC请在上述过程中添加代码

    auto write_set = txn->get_write_set();

    // 反向遍历写集合，回滚所有写操作
    for (auto iter = write_set->rbegin(); iter != write_set->rend(); iter++) {
        const auto write_type = (*iter)->GetWriteType();
        const auto table_name = (*iter)->GetTableName();
        const auto rid = (*iter)->GetRid();
        std::unique_ptr<RmFileHandle>& handle = sm_manager_->fhs_.at(table_name);

        // 创建Context对象
        Context context(lock_manager_, log_manager, txn);

        if (write_type == WType::INSERT_TUPLE) {
            // 对于INSERT操作，回滚时删除插入的记录
            auto rec = (*iter)->GetRecord();

            // 删除所有索引项
            for (const auto& index : sm_manager_->db_.get_table(table_name).indexes) {
                auto ix_name = sm_manager_->get_ix_manager()->get_index_name(table_name, index.cols);
                auto* ih = sm_manager_->ihs_.at(ix_name).get();

                // 构造索引键值
                std::vector<char> key_buffer(index.col_tot_len);
                char* key = key_buffer.data();
                int offset = 0;
                for (int j = 0; j < index.col_num; ++j) {
                    memcpy(key + offset, rec.data + index.cols[j].offset, index.cols[j].len);
                    offset += index.cols[j].len;
                }
                ih->delete_entry(key, txn);
            }

            // 删除记录
            handle->delete_record(rid, &context);

        } else if (write_type == WType::UPDATE_TUPLE) {
            // 对于UPDATE操作，恢复到旧值
            auto new_rec = handle->get_record(rid, &context);
            auto old_rec = (*iter)->GetRecord();

            // 删除新记录的索引项，插入旧记录的索引项
            for (const auto& index : sm_manager_->db_.get_table(table_name).indexes) {
                auto ix_name = sm_manager_->get_ix_manager()->get_index_name(table_name, index.cols);
                auto* ih = sm_manager_->ihs_.at(ix_name).get();

                // 构造新记录的索引键值并删除
                std::vector<char> new_key_buffer(index.col_tot_len);
                char* new_key = new_key_buffer.data();
                int offset = 0;
                for (int j = 0; j < index.col_num; ++j) {
                    memcpy(new_key + offset, new_rec->data + index.cols[j].offset, index.cols[j].len);
                    offset += index.cols[j].len;
                }
                ih->delete_entry(new_key, txn);

                // 构造旧记录的索引键值并插入
                std::vector<char> old_key_buffer(index.col_tot_len);
                char* old_key = old_key_buffer.data();
                offset = 0;
                for (int j = 0; j < index.col_num; ++j) {
                    memcpy(old_key + offset, old_rec.data + index.cols[j].offset, index.cols[j].len);
                    offset += index.cols[j].len;
                }
                ih->insert_entry(old_key, rid, txn);
            }

            // 更新记录
            handle->update_record(rid, old_rec.data, &context);

        } else if (write_type == WType::DELETE_TUPLE) {
            // 对于DELETE操作，重新插入被删除的记录
            auto rec = (*iter)->GetRecord();

            // 重新插入记录
            handle->insert_record(rid, rec.data);

            // 重新插入索引项
            for (const auto& index : sm_manager_->db_.get_table(table_name).indexes) {
                auto ix_name = sm_manager_->get_ix_manager()->get_index_name(table_name, index.cols);
                auto* ih = sm_manager_->ihs_.at(ix_name).get();

                // 构造索引键值
                std::vector<char> key_buffer(index.col_tot_len);
                char* key = key_buffer.data();
                int offset = 0;
                for (int j = 0; j < index.col_num; ++j) {
                    memcpy(key + offset, rec.data + index.cols[j].offset, index.cols[j].len);
                    offset += index.cols[j].len;
                }
                ih->insert_entry(key, rid, txn);
            }
        }
    }

    // 清空写集合
    txn->get_write_set()->clear();

    // 释放所有锁
    std::shared_ptr<std::unordered_set<LockDataId>> lock_set = txn->get_lock_set();
    auto lock_set_copy = *lock_set;  // 复制锁集合以避免迭代时修改
    for (const LockDataId& lock : lock_set_copy) {
        lock_manager_->unlock(txn, lock);
    }

    auto lock_gap_set = txn->get_lock_gap_set();
    auto lock_gap_set_copy = *lock_gap_set;  // 复制间隙锁集合以避免迭代时修改
    for (const int& tab_fd : lock_gap_set_copy) {
        lock_manager_->unlock_gap(txn, tab_fd);
    }

    // 清空事务状态并设置为ABORTED
    txn->set_state(TransactionState::ABORTED);

    // MVCC相关清理
    if (concurrency_mode_ == ConcurrencyMode::MVCC) {
        running_txns_.RemoveTxn(txn->get_read_ts());  // 从水位线中移除事务
    }
}

/**
 * @brief 更新一个撤销链接，该链接将表堆元组与第一个撤销日志连接起来。
 * 在更新之前，将调用 `check` 函数以确保有效性。
 */
bool TransactionManager::UpdateUndoLink(Rid rid, std::optional<UndoLink> prev_link,
                                        std::function<bool(std::optional<UndoLink>)>&& check) {
    std::optional<VersionUndoLink> prev_version = VersionUndoLink::FromOptionalUndoLink(prev_link);
    return UpdateVersionLink(rid, prev_version, std::move(check) ? [check = std::move(check)](std::optional<VersionUndoLink> v) {
        return check(v.has_value() ? std::optional<UndoLink>{v->prev_} : std::nullopt);
    }
                                                                 : std::function<bool(std::optional<VersionUndoLink>)>{});
}

/**
 * @brief 更新一个撤销链接，该链接将表堆元组与第一个撤销日志连接起来。
 * 在更新之前，将调用 `check` 函数以确保有效性。
 */
bool TransactionManager::UpdateVersionLink(Rid rid, std::optional<VersionUndoLink> prev_version,
                                           std::function<bool(std::optional<VersionUndoLink>)>&& check) {
    // 获取对应的版本信息
    std::shared_lock<std::shared_mutex> lock(version_info_mutex_);
    page_id_t page_id = rid.page_no;
    auto it = version_info_.find(page_id);
    if (it == version_info_.end()) {
        // 如果没有找到对应的版本信息，则创建一个新的
        lock.unlock();
        std::unique_lock<std::shared_mutex> write_lock(version_info_mutex_);
        auto new_version_info = std::make_shared<PageVersionInfo>();
        version_info_[page_id] = new_version_info;
        it = version_info_.find(page_id);
        write_lock.unlock();
        lock.lock();
    }
    auto& version_info = it->second;
    lock.unlock();
    std::unique_lock<std::shared_mutex> version_lock(version_info->mutex_);

    // 检查函数验证
    if (check) {
        auto current_version_it = version_info->prev_version_.find(rid.slot_no);
        std::optional<VersionUndoLink> current_version =
            current_version_it != version_info->prev_version_.end() ? std::optional<VersionUndoLink>{current_version_it->second} : std::nullopt;
        if (!check(current_version)) {
            return false;
        }
    }

    // 更新版本链接
    auto& prev_version_map = version_info->prev_version_;
    if (prev_version.has_value()) {
        // 如果提供了版本链接，则更新或插入
        prev_version_map[rid.slot_no] = *prev_version;
    } else {
        // 如果没有提供版本链接，则删除现有链接
        prev_version_map.erase(rid.slot_no);
    }
    return true;  // 更新成功，返回 true
}

/** @brief 获取表堆元组的第一个撤销日志。 */
std::optional<UndoLink> TransactionManager::GetUndoLink(Rid rid) {
    std::optional<VersionUndoLink> version_link = GetVersionLink(rid);
    if (!version_link.has_value()) {
        return std::nullopt;  // 如果没有找到对应的版本链接，则返回 nullopt
    }
    return version_link->prev_;  // 返回前一个版本链接的撤销日志
}

/** @brief 获取表堆元组的第一个撤销日志。 */
std::optional<VersionUndoLink> TransactionManager::GetVersionLink(Rid rid) {
    std::shared_lock<std::shared_mutex> lock(version_info_mutex_);
    page_id_t page_id = rid.page_no;
    auto it = version_info_.find(page_id);
    if (it == version_info_.end()) {
        return std::nullopt;  // 如果没有找到对应的版本信息，则返回 nullopt
    }
    auto& version_info = it->second;
    lock.unlock();
    std::shared_lock<std::shared_mutex> version_lock(version_info->mutex_);
    auto prev_version_it = version_info->prev_version_.find(rid.slot_no);
    if (prev_version_it == version_info->prev_version_.end()) {
        return std::nullopt;  // 如果没有找到对应的前一个版本链接，则返回 nullopt
    }
    return prev_version_it->second;  // 返回前一个版本链接
}

/** @brief 访问事务撤销日志缓冲区并获取撤销日志。如果事务不存在，返回 nullopt。
 * 如果索引超出范围仍然会抛出异常。 */
std::optional<UndoLog> TransactionManager::GetUndoLogOptional(UndoLink link) {
    // 检查事务是否存在
    if (link.prev_txn_ == INVALID_TXN_ID) return std::nullopt;
    std::unique_lock<std::mutex> lock(latch_);
    auto it = TransactionManager::txn_map.find(link.prev_txn_);
    if (it == TransactionManager::txn_map.end()) {
        lock.unlock();
        return std::nullopt;  // 如果事务不存在，则返回 nullopt
    }
    auto txn = it->second;
    lock.unlock();

    // 检查撤销日志索引是否有效
    if (link.prev_log_idx_ < 0 || link.prev_log_idx_ >= txn->GetUndoLogNum()) {
        throw std::runtime_error("Invalid undo log index: " + std::to_string(link.prev_log_idx_));
    }

    // 返回对应的撤销日志
    return txn->GetUndoLog(link.prev_log_idx_);
}

/** @brief 访问事务撤销日志缓冲区并获取撤销日志。除非访问当前事务缓冲区，
 * 否则应该始终调用此函数以获取撤销日志，而不是手动检索事务 shared_ptr 并访问缓冲区。 */
UndoLog TransactionManager::GetUndoLog(UndoLink link) {
    // 检查事务是否存在
    if (link.prev_txn_ == INVALID_TXN_ID) {
        throw std::runtime_error("Invalid transaction ID: " + std::to_string(link.prev_txn_));
    }
    std::unique_lock<std::mutex> lock(latch_);
    auto it = TransactionManager::txn_map.find(link.prev_txn_);
    if (it == TransactionManager::txn_map.end()) {
        throw std::runtime_error("Transaction not found: " + std::to_string(link.prev_txn_));
    }
    auto txn = it->second;
    lock.unlock();

    // 检查撤销日志索引是否有效
    if (link.prev_log_idx_ < 0 || link.prev_log_idx_ >= txn->GetUndoLogNum()) {
        throw std::runtime_error("Invalid undo log index: " + std::to_string(link.prev_log_idx_));
    }

    // 返回对应的撤销日志
    return txn->GetUndoLog(link.prev_log_idx_);
}

/** @brief 获取系统中的最低读时间戳。 */
timestamp_t TransactionManager::GetWatermark() {
    return running_txns_.GetWatermark();
}

/** @brief 垃圾回收。仅在所有事务都未访问时调用。 */
void TransactionManager::GarbageCollection() {
    // 留空，按用户要求不实现GC
}

/** @brief MVCC: 检查写入冲突，支持快照隔离级别 */
bool TransactionManager::CheckWriteConflict(Transaction* txn, const Rid& rid) {
    if (concurrency_mode_ != ConcurrencyMode::MVCC) {
        return true;  // 非MVCC模式下不检查写入冲突
    }

    // 获取元组的最新版本信息
    auto [has_conflict, latest_txn_id] = GetLatestVersionInfo(rid);

    if (!has_conflict) {
        return true;  // 没有冲突
    }

    // 情况1：最新版本属于未提交的事务
    std::unique_lock<std::mutex> lock(latch_);
    auto it = txn_map.find(latest_txn_id);
    if (it != txn_map.end()) {
        Transaction* latest_txn = it->second;
        lock.unlock();

        if (latest_txn->get_transaction_id() == txn->get_transaction_id()) {
            return true;  // 是同一个事务，允许写入
        }

        if (latest_txn->get_state() != TransactionState::COMMITTED &&
            latest_txn->get_state() != TransactionState::ABORTED) {
            // 另一个未提交的事务持有该元组
            return false;  // 写入冲突
        }
    }
    lock.unlock();

    // 情况2：最新版本属于已提交的事务，检查提交时间戳
    timestamp_t latest_commit_ts = last_commit_ts_.load();
    if (latest_commit_ts > txn->get_read_ts()) {
        // 已提交事务的提交时间戳大于当前事务的读时间戳
        return false;  // 写入冲突
    }

    return true;  // 无冲突
}

/** @brief MVCC: 为事务分配读时间戳 */
void TransactionManager::AssignReadTimestamp(Transaction* txn) {
    if (concurrency_mode_ == ConcurrencyMode::MVCC) {
        timestamp_t read_ts = txn->get_start_ts();
        txn->set_read_ts(read_ts);
    }
}

/** @brief MVCC: 为事务分配提交时间戳 */
timestamp_t TransactionManager::AssignCommitTimestamp(Transaction* txn) {
    if (concurrency_mode_ == ConcurrencyMode::MVCC) {
        timestamp_t commit_ts = next_timestamp_.fetch_add(1);
        return commit_ts;
    }
    return INVALID_TS;
}

/** @brief MVCC: 获取元组的最新版本及其事务信息 */
std::pair<bool, txn_id_t> TransactionManager::GetLatestVersionInfo(const Rid& rid) {
    auto version_link = GetVersionLink(rid);
    if (!version_link.has_value()) {
        return {false, INVALID_TXN_ID};  // 没有版本信息
    }

    if (version_link->in_progress_) {
        return {true, version_link->prev_.prev_txn_};  // 有正在进行的事务
    }

    return {false, INVALID_TXN_ID};  // 无冲突
}

/** @brief MVCC: 检查版本可见性 */
bool TransactionManager::IsVersionVisible(Transaction* txn, timestamp_t version_ts, txn_id_t version_txn_id) {
    if (concurrency_mode_ != ConcurrencyMode::MVCC) {
        return true;  // 非MVCC模式下所有版本都可见
    }

    // 如果是同一个事务，版本可见
    if (version_txn_id == txn->get_transaction_id()) {
        return true;
    }

    // 检查版本事务是否已提交
    std::unique_lock<std::mutex> lock(latch_);
    auto it = txn_map.find(version_txn_id);
    if (it != txn_map.end()) {
        Transaction* version_txn = it->second;
        lock.unlock();

        // 如果版本事务未提交，版本不可见
        if (version_txn->get_state() != TransactionState::COMMITTED) {
            return false;
        }
    }
    lock.unlock();

    // 如果版本时间戳小于等于当前事务的读时间戳，版本可见
    return version_ts <= txn->get_read_ts();
}