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

extern SmManager sm_manager;
extern LockManager lock_manager;

std::unordered_map<txn_id_t, Transaction*> TransactionManager::txn_map = {};

TransactionManager::TransactionManager(ConcurrencyMode concurrency_mode) { concurrency_mode_ = concurrency_mode; }
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
    // 不要强制设置txn_mode，保持调用者设置的值
    txn->set_state(TransactionState::DEFAULT);

    // MVCC相关设置
    if (concurrency_mode_ == ConcurrencyMode::MVCC) {
        running_txns_.AddTxn(start_ts);  // 添加事务到水位线
        AssignReadTimestamp(txn);        // 分配读时间戳
    }

    // 记录BEGIN日志
    if (log_manager != nullptr) {
        BeginLogRecord log_record(txn->get_transaction_id());
        lsn_t lsn = log_manager->add_log_to_buffer(&log_record);
        txn->set_prev_lsn(lsn);
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
        txn->set_commit_ts(commit_ts);  // 设置事务的提交时间戳

        // 更新所有相关版本链的状态为已完成
        auto write_set = txn->get_write_set();
        for (auto write_record : *write_set) {
            // 通过表名获取fd - 需要使用sm_manager_来查找
            int fd = -1;
            try {
                auto& fh = sm_manager.fhs_.at(write_record->GetTableName());
                fd = fh->GetFd();
            } catch (const std::exception& e) {
                // 表文件未找到，跳过
                continue;
            }

            auto version_link_opt = GetVersionLink(fd, write_record->GetRid());
            if (version_link_opt.has_value() &&
                version_link_opt->prev_.prev_txn_ == txn->get_transaction_id() &&
                version_link_opt->in_progress_) {
                // 创建新的版本链，设置in_progress为false
                VersionUndoLink updated_link = version_link_opt.value();
                updated_link.in_progress_ = false;

                // 更新版本链状态
                UpdateVersionLink(fd, write_record->GetRid(), updated_link,
                                  [](std::optional<VersionUndoLink>) { return true; });
            }
        }
    }

    std::shared_ptr<std::unordered_set<LockDataId>> lock_set = txn->get_lock_set();

    auto lock_set_copy = *lock_set;  // 复制锁集合以避免迭代时修改
    for (const LockDataId& lock : lock_set_copy) {
        lock_manager.unlock(txn, lock);
    }

    auto lock_gap_set = txn->get_lock_gap_set();
    auto lock_gap_set_copy = *lock_gap_set;  // 复制间隙锁集合以避免迭代时修改
    for (const int& tab_fd : lock_gap_set_copy) {
        lock_manager.unlock_gap(txn, tab_fd);
    }

    // 清空事务相关的集合
    txn->clear();

    // 记录COMMIT日志
    if (log_manager != nullptr) {
        CommitLogRecord log_record(txn->get_transaction_id());
        log_record.prev_lsn_ = txn->get_prev_lsn();
        log_manager->add_log_to_buffer(&log_record);
        log_manager->flush_log_to_disk();  // 提交时强制刷盘
    }

    // MVCC相关清理
    running_txns_.UpdateCommitTs(commit_ts);      // 更新水位线的提交时间戳
    running_txns_.RemoveTxn(txn->get_read_ts());  // 从水位线中移除事务
}

/**
 * @brief 恢复记录的索引项（用于事务回滚）
 */
void TransactionManager::RestoreIndexForRecord(const std::string& table_name, const RmRecord& rec,
                                               const Rid& rid, Context* context) {
    TabMeta& tab_meta = sm_manager.db_.get_table(table_name);

    // 遍历表的所有索引
    for (size_t i = 0; i < tab_meta.indexes.size(); ++i) {
        auto& index = tab_meta.indexes[i];
        auto ih = sm_manager.ihs_.at(sm_manager.get_ix_manager()->get_index_name(table_name, index.cols)).get();
        auto key = std::make_unique<char[]>(index.col_tot_len);
        int offset = 0;
        for (size_t j = 0; j < static_cast<size_t>(index.col_num); ++j) {
            memcpy(key.get() + offset, rec.data + index.cols[j].offset, index.cols[j].len);
            offset += index.cols[j].len;
        }

        // 检查索引项是否已存在，避免重复插入
        std::vector<Rid> existing_rids;
        if (ih->get_value(key.get(), &existing_rids, context->txn_)) {
            // 检查是否已经存在指向该RID的索引项
            bool already_exists = false;
            for (const auto& existing_rid : existing_rids) {
                if (existing_rid.page_no == rid.page_no && existing_rid.slot_no == rid.slot_no) {
                    already_exists = true;
                    break;
                }
            }
            if (!already_exists) {
                ih->insert_entry_force(key.get(), rid, context->txn_);
            }
        } else {
            // 索引项不存在，直接插入
            ih->insert_entry_force(key.get(), rid, context->txn_);
        }
    }
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Context *} context 包含事务信息的上下文
 * @param {LogManager} *log_manager 日志管理器指针
 */

void TransactionManager::abort(Context* context, LogManager* log_manager) {
    // 1. 回滚所有写操作
    Transaction* txn = context->txn_;
    auto write_set = txn->get_write_set();

    for (auto iter = write_set->rbegin(); iter != write_set->rend(); iter++) {
        const auto write_type = (*iter)->GetWriteType();
        const auto table_name = (*iter)->GetTableName();
        const auto rid = (*iter)->GetRid();
        std::unique_ptr<RmFileHandle>& handle = sm_manager.fhs_.at(table_name);

        if (write_type == WType::INSERT_TUPLE) {
            auto rec = (*iter)->GetRecord();
            handle->delete_record(rid, context);
            sm_manager.delete_index_with_rid(table_name, rec, rid, context->txn_);
        } else if (write_type == WType::DELETE_TUPLE) {
            auto rec = (*iter)->GetRecord();
            RestoreIndexForRecord(table_name, rec, rid, context);
        }
        // 删除版本链记录
        DeleteUpdateVersionLink(handle->GetFd(), rid, context->txn_);
    }

    // 释放所有锁
    std::shared_ptr<std::unordered_set<LockDataId>> lock_set = txn->get_lock_set();
    auto lock_set_copy = *lock_set;  // 复制锁集合以避免迭代时修改
    for (const LockDataId& lock : lock_set_copy) {
        lock_manager.unlock(txn, lock);
    }

    auto lock_gap_set = txn->get_lock_gap_set();
    auto lock_gap_set_copy = *lock_gap_set;  // 复制间隙锁集合以避免迭代时修改
    for (const int& tab_fd : lock_gap_set_copy) {
        lock_manager.unlock_gap(txn, tab_fd);
    }

    // 清空事务状态并设置为ABORTED
    txn->set_state(TransactionState::ABORTED);

    // 记录ABORT日志
    if (log_manager != nullptr) {
        AbortLogRecord log_record(txn->get_transaction_id());
        log_record.prev_lsn_ = txn->get_prev_lsn();
        log_manager->add_log_to_buffer(&log_record);
        log_manager->flush_log_to_disk();  // 中止时强制刷盘
    }

    // MVCC相关清理
    if (concurrency_mode_ == ConcurrencyMode::MVCC) {
        running_txns_.RemoveTxn(txn->get_read_ts());  // 从水位线中移除事务

        // 清理版本链：恢复到该事务修改前的状态
        // 注意：使用write_set而不是txn->get_write_set()，因为后者可能已被清空
        for (auto iter = write_set->rbegin(); iter != write_set->rend(); iter++) {
            const auto table_name = (*iter)->GetTableName();
            const auto rid = (*iter)->GetRid();
            int fd = -1;
            try {
                auto& fh = sm_manager.fhs_.at(table_name);
                fd = fh->GetFd();
            } catch (const std::exception& e) {
                // 表文件未找到，跳过
                continue;
            }

            // 获取当前版本链
            auto current_version_link = GetVersionLink(fd, rid);
            if (current_version_link.has_value() &&
                current_version_link->prev_.prev_txn_ == txn->get_transaction_id()) {
                // 如果当前版本链的最新版本确实是该事务创建的，则恢复到前一个版本
                auto undo_log_opt = GetUndoLogOptional(current_version_link->prev_);
                if (undo_log_opt.has_value() && undo_log_opt->prev_version_.IsValid()) {
                    // 恢复到前一个版本
                    VersionUndoLink prev_version_link;
                    prev_version_link.prev_ = undo_log_opt->prev_version_;
                    prev_version_link.in_progress_ = false;
                    UpdateVersionLink(fd, rid, prev_version_link, [](std::optional<VersionUndoLink>) { return true; });
                } else {
                    // 没有前一个版本，删除版本链
                    UpdateVersionLink(fd, rid, std::nullopt, [](std::optional<VersionUndoLink>) { return true; });
                }
            }
        }
    }

    // 最后清空写集合（在所有清理工作完成后）
    txn->get_write_set()->clear();
}
/**
 * @brief 更新一个撤销链接，该链接将表堆元组与第一个撤销日志连接起来。
 * 在更新之前，将调用 `check` 函数以确保有效性。
 */
bool TransactionManager::UpdateUndoLink(int fd, Rid rid, std::optional<UndoLink> prev_link,
                                        std::function<bool(std::optional<UndoLink>)>&& check) {
    std::optional<VersionUndoLink> prev_version = VersionUndoLink::FromOptionalUndoLink(prev_link);
    return UpdateVersionLink(fd, rid, prev_version, std::move(check) ? [check = std::move(check)](std::optional<VersionUndoLink> v) {
        return check(v.has_value() ? std::optional<UndoLink>{v->prev_} : std::nullopt);
    }
                                                                     : std::function<bool(std::optional<VersionUndoLink>)>{});
}

/**
 * @brief 更新一个撤销链接，该链接将表堆元组与第一个撤销日志连接起来。
 * 在更新之前，将调用 `check` 函数以确保有效性。
 */
bool TransactionManager::UpdateVersionLink(int fd, Rid rid, std::optional<VersionUndoLink> prev_version,
                                           std::function<bool(std::optional<VersionUndoLink>)>&& check) {
    TableRid table_rid{fd, rid};
    std::unique_lock<std::shared_mutex> lock(version_info_mutex_);

    // 检查函数验证
    if (check) {
        auto current_version_it = version_info_.find(table_rid);
        std::optional<VersionUndoLink> current_version =
            current_version_it != version_info_.end() ? std::optional<VersionUndoLink>{current_version_it->second} : std::nullopt;
        if (!check(current_version)) {
            return false;
        }
    }

    // 更新版本链接
    if (prev_version.has_value()) {
        // 如果提供了版本链接，则更新或插入
        version_info_[table_rid] = *prev_version;
    } else {
        // 如果没有提供版本链接，则删除现有链接
        version_info_.erase(table_rid);
    }
    return true;  // 更新成功，返回 true
}

/** @brief 获取表堆元组的第一个撤销日志。 */
std::optional<UndoLink> TransactionManager::GetUndoLink(int fd, Rid rid) {
    std::optional<VersionUndoLink> version_link = GetVersionLink(fd, rid);
    if (!version_link.has_value()) {
        return std::nullopt;  // 如果没有找到对应的版本链接，则返回 nullopt
    }
    return version_link->prev_;  // 返回前一个版本链接的撤销日志
}

/** @brief 获取表堆元组的第一个撤销日志。 */
std::optional<VersionUndoLink> TransactionManager::GetVersionLink(int fd, Rid rid) {
    /**
     * @brief 删除指定(fd, rid)的版本链，并返回原有UndoLink
     */
    std::shared_lock<std::shared_mutex> lock(version_info_mutex_);
    TableRid table_rid{fd, rid};
    auto it = version_info_.find(table_rid);
    if (it == version_info_.end()) {
        return std::nullopt;  // 如果没有找到对应的版本信息，则返回 nullopt
    }
    return it->second;  // 返回版本链接
}

/**
 * @brief 删除指定(fd, rid)的版本链，并返回原有UndoLink
 */
UndoLink TransactionManager::DeleteUpdateVersionLink(const int& fd, Rid rid, Transaction* txn) {
    auto version_link_opt = GetVersionLink(fd, rid);
    UndoLink prev_undo_link;
    if (version_link_opt.has_value()) {
        prev_undo_link = version_link_opt->prev_;
        // 删除版本链
        UpdateVersionLink(fd, rid, std::nullopt, [](std::optional<VersionUndoLink>) { return true; });
    } else {
        prev_undo_link = UndoLink{};  // 返回默认空UndoLink
    }
    return prev_undo_link;
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
bool TransactionManager::CheckWriteConflict(Transaction* txn, int fd, const Rid& rid) {
    if (concurrency_mode_ != ConcurrencyMode::MVCC) {
        return true;  // 非MVCC模式下不检查写入冲突
    }

    // 获取元组的最新版本信息
    auto [has_conflict, latest_txn_id] = GetLatestVersionInfo(fd, rid);

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
std::pair<bool, txn_id_t> TransactionManager::GetLatestVersionInfo(int fd, const Rid& rid) {
    auto version_link = GetVersionLink(fd, rid);
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