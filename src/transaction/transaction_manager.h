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

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

#include "common/context.h"
#include "common/exception.h"
#include "concurrency/lock_manager.h"
#include "recovery/log_manager.h"
#include "system/sm_manager.h"
#include "transaction.h"
#include "watermark.h"

/* 系统采用的并发控制算法，当前题目中要求两阶段封锁并发控制算法 */
enum class ConcurrencyMode { TWO_PHASE_LOCKING = 0,
                             BASIC_TO,
                             MVCC };

extern SmManager sm_manager;
extern LockManager lock_manager;

/// 版本链中的第一个撤销链接，将表堆元组链接到撤销日志。
struct VersionUndoLink {
    /** 版本链中的下一个版本。 */
    UndoLink prev_;
    bool in_progress_{false};

    friend auto operator==(const VersionUndoLink &a, const VersionUndoLink &b) {
        return a.prev_ == b.prev_ && a.in_progress_ == b.in_progress_;
    }

    friend auto operator!=(const VersionUndoLink &a, const VersionUndoLink &b) { return !(a == b); }

    inline static std::optional<VersionUndoLink> FromOptionalUndoLink(std::optional<UndoLink> undo_link) {
        if (undo_link.has_value()) {
            return VersionUndoLink{*undo_link};
        }
        return std::nullopt;
    }
};

// 用于MVCC版本链管理的复合键，包含表标识符和记录标识符
struct TableRid {
    int fd;   // 表文件描述符，用于区分不同的表
    Rid rid;  // 记录标识符

    friend bool operator==(const TableRid &x, const TableRid &y) {
        return x.fd == y.fd && x.rid == y.rid;
    }

    friend bool operator!=(const TableRid &x, const TableRid &y) { return !(x == y); }
};

// 为TableRid提供哈希函数，用于unordered_map
struct TableRidHash {
    std::size_t operator()(const TableRid &key) const {
        return std::hash<int>()(key.fd) ^ (std::hash<int>()(key.rid.page_no) << 1) ^ (std::hash<int>()(key.rid.slot_no) << 2);
    }
};

class TransactionManager {
   public:
    explicit TransactionManager(ConcurrencyMode concurrency_mode = ConcurrencyMode::TWO_PHASE_LOCKING);

    ~TransactionManager() = default;

    Transaction *begin(Transaction *txn, LogManager *log_manager);

    void commit(Transaction *txn, LogManager *log_manager);

    void abort(Context *context, LogManager *log_manager);

    ConcurrencyMode get_concurrency_mode() { return concurrency_mode_; }

    void set_concurrency_mode(ConcurrencyMode concurrency_mode) { concurrency_mode_ = concurrency_mode; }

    LockManager *get_lock_manager() { return &lock_manager; }

    /**
     * @description: 获取事务ID为txn_id的事务对象
     * @return {Transaction*} 事务对象的指针
     * @param {txn_id_t} txn_id 事务ID
     */
    Transaction *get_transaction(txn_id_t txn_id) {
        if (txn_id == INVALID_TXN_ID) return nullptr;

        std::unique_lock<std::mutex> lock(latch_);
        assert(TransactionManager::txn_map.find(txn_id) != TransactionManager::txn_map.end());
        auto *res = TransactionManager::txn_map[txn_id];
        lock.unlock();
        assert(res != nullptr);
        assert(res->get_thread_id() == std::this_thread::get_id());

        return res;
    }

    /**
     * @description: 安全地查找事务ID为txn_id的事务对象（用于MVCC场景）
     * @return {Transaction*} 事务对象的指针，如果不存在则返回nullptr
     * @param {txn_id_t} txn_id 事务ID
     */
    Transaction *find_transaction(txn_id_t txn_id) {
        if (txn_id == INVALID_TXN_ID) return nullptr;

        std::unique_lock<std::mutex> lock(latch_);
        auto it = TransactionManager::txn_map.find(txn_id);
        if (it != TransactionManager::txn_map.end()) {
            return it->second;
        }
        return nullptr;
    }

    static std::unordered_map<txn_id_t, Transaction *> txn_map;  // 全局事务表，存放事务ID与事务对象的映射关系
    std::shared_mutex txn_map_mutex_;
    /**
     * @brief 更新一个撤销链接，该链接将表堆元组与第一个撤销日志连接起来。
     * 在更新之前，将调用 `check` 函数以确保有效性。
     */
    bool UpdateUndoLink(int fd, Rid rid, std::optional<UndoLink> prev_link,
                        std::function<bool(std::optional<UndoLink>)> &&check = nullptr);

    /**
     * @brief 更新一个撤销链接，该链接将表堆元组与第一个撤销日志连接起来。
     * 在更新之前，将调用 `check` 函数以确保有效性。
     */
    bool UpdateVersionLink(int fd, Rid rid, std::optional<VersionUndoLink> prev_version,
                           std::function<bool(std::optional<VersionUndoLink>)> &&check = nullptr);

    /** @brief 获取表堆元组的第一个撤销日志。 */
    std::optional<UndoLink> GetUndoLink(int fd, Rid rid);

    /** @brief 获取表堆元组的第一个撤销日志。*/
    std::optional<VersionUndoLink> GetVersionLink(int fd, Rid rid);

    /** @brief 访问事务撤销日志缓冲区并获取撤销日志。如果事务不存在，返回 nullopt。
     * 如果索引超出范围仍然会抛出异常。 */
    std::optional<UndoLog> GetUndoLogOptional(UndoLink link);

    /** @brief 访问事务撤销日志缓冲区并获取撤销日志。除非访问当前事务缓冲区，
     * 否则应该始终调用此函数以获取撤销日志，而不是手动检索事务 shared_ptr 并访问缓冲区。 */
    UndoLog GetUndoLog(UndoLink link);

    /** @brief 获取系统中的最低读时间戳。 */
    timestamp_t GetWatermark();

    /** @brief 垃圾回收。仅在所有事务都未访问时调用。 */
    void GarbageCollection();

    /** @brief MVCC: 检查写入冲突，支持快照隔离级别 */
    bool CheckWriteConflict(Transaction *txn, int fd, const Rid &rid);

    /** @brief MVCC: 为事务分配读时间戳 */
    void AssignReadTimestamp(Transaction *txn);

    /** @brief MVCC: 为事务分配提交时间戳 */
    timestamp_t AssignCommitTimestamp(Transaction *txn);

    /** @brief MVCC: 获取元组的最新版本及其事务信息 */
    std::pair<bool, txn_id_t> GetLatestVersionInfo(int fd, const Rid &rid);

    /** @brief MVCC: 检查版本可见性 */
    bool IsVersionVisible(Transaction *txn, timestamp_t version_ts, txn_id_t version_txn_id);

    // 获取全局唯一递增的时间戳（用于每次写操作）
    timestamp_t get_next_timestamp() {
        return next_timestamp_.fetch_add(1);
    }
    bool exsit_transaction(txn_id_t txn_id) {
        if (txn_id == INVALID_TXN_ID) return false;
        std::shared_lock<std::shared_mutex> lock(txn_map_mutex_);
        return TransactionManager::txn_map.find(txn_id) != TransactionManager::txn_map.end();
    }

    UndoLink DeleteUpdateVersionLink(const int &fd, Rid rid, Transaction *txn);

    /** @brief 恢复记录的索引项（用于事务回滚） */
    void RestoreIndexForRecord(const std::string &table_name, const RmRecord &rec,
                               const Rid &rid, Context *context);

    /** 保护版本信息 */
    std::shared_mutex version_info_mutex_;
    /** 存储表堆中每个元组的先前版本，使用TableRid作为键以区分不同表的记录。 */
    std::unordered_map<TableRid, VersionUndoLink, TableRidHash> version_info_;

   private:
    ConcurrencyMode concurrency_mode_;            // 事务使用的并发控制算法，目前只需要考虑2PL
    std::atomic<txn_id_t> next_txn_id_{0};        // 用于分发事务ID
    std::atomic<timestamp_t> next_timestamp_{0};  // 用于分发事务时间戳
    std::mutex latch_;                            // 用于txn_map的并发

    std::atomic<timestamp_t> last_commit_ts_{0};  // 最后提交的时间戳,仅用于MVCC
    Watermark running_txns_{0};                   // 存储所有正在运行事务的读取时间戳，以便于垃圾回收，仅用于MVCC
};