/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"

/**
 * @description: 申请行级共享锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID 记录所在的表的fd
 * @param {int} tab_fd
 */
bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    std::unique_lock<std::mutex> lock(latch_);  // 1. Acquire global latch

    // 创建锁数据标识符( 行级锁
    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);

    auto queue_it = lock_table_.find(lock_data_id);
    if (queue_it == lock_table_.end()) {
        // 如果锁表中没有该锁数据标识符，则创建一个新的锁请求队列
        queue_it = lock_table_
                       .emplace(std::piecewise_construct,
                                std::forward_as_tuple(lock_data_id),  // 构造 key
                                std::forward_as_tuple()               // 默认构造 value (LockRequestQueue)
                                )
                       .first;
    }
    LockRequestQueue& request_queue = queue_it->second;

    // 检查是否已经获得锁
    for (const auto& req : request_queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) {
            if (req.lock_mode_ == LockManager::LockMode::EXCLUSIVE || req.lock_mode_ == LockManager::LockMode::SHARED) {
                return true;
            }
        }
    }

    // 检查是否冲突
    bool conflict = false;
    for (const auto& req : request_queue.request_queue_) {
        if (req.granted_ && req.lock_mode_ == LockManager::LockMode::EXCLUSIVE) {
            conflict = true;
            break;
        }
    }

    LockRequest current_request(txn->get_transaction_id(), LockManager::LockMode::SHARED);

    if (!conflict) {
        current_request.granted_ = true;
        request_queue.request_queue_.push_back(current_request);
        txn->get_lock_set()->insert(lock_data_id);
        return true;
    } else {
        // no-wait策略下，直接返回false
        return false;
    }
}

/**
 * @description: 申请行级排他锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID
 * @param {int} tab_fd 记录所在的表的fd
 */
bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    std::unique_lock<std::mutex> lock(latch_);  // 1. Acquire global latch

    // 创建锁数据标识符( 行级锁
    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);

    auto queue_it = lock_table_.find(lock_data_id);
    if (queue_it == lock_table_.end()) {
        // 如果锁表中没有该锁数据标识符，则创建一个新的锁请求队列
        queue_it = lock_table_
                       .emplace(std::piecewise_construct,
                                std::forward_as_tuple(lock_data_id),  // 构造 key
                                std::forward_as_tuple()               // 默认构造 value (LockRequestQueue)
                                )
                       .first;
    }
    LockRequestQueue& request_queue = queue_it->second;

    // 检查是否已经获得锁
    for (const auto& req : request_queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) {
            if (req.lock_mode_ == LockManager::LockMode::EXCLUSIVE) {
                return true;
            }
        }
    }

    bool conflict = false;
    for (const auto& req : request_queue.request_queue_) {
        if (req.granted_) {
            if (req.txn_id_ != txn->get_transaction_id()) {
                conflict = true;
                break;
            }

            if (req.txn_id_ == txn->get_transaction_id() && req.lock_mode_ == LockMode::SHARED) {
                conflict = true;
                break;
            }
        }
    }

    LockRequest current_request(txn->get_transaction_id(), LockManager::LockMode::EXCLUSIVE);

    if (!conflict) {
        current_request.granted_ = true;
        request_queue.request_queue_.push_back(current_request);
        txn->get_lock_set()->insert(lock_data_id);
        return true;
    } else {
        // no-wait策略
        return false;
    }
}

/**
 * @description: 申请表级读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    return true;
}

/**
 * @description: 申请表级写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    return true;
}

/**
 * @description: 申请表级意向读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    return true;
}

/**
 * @description: 申请表级意向写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    return true;
}

/**
 * @description: 释放锁
 * @return {bool} 返回解锁是否成功
 * @param {Transaction*} txn 要释放锁的事务对象指针
 * @param {LockDataId} lock_data_id 要释放的锁ID
 */
bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    return true;
}

/**
 * @description: 申请间隙锁，记录事务的WHERE条件
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请间隙锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 * @param {std::vector<Condition>} conds WHERE条件列表
 */
bool LockManager::lock_gap(Transaction* txn, int tab_fd, std::vector<Condition> conds) {
    std::unique_lock<std::mutex> lock(latch_);

    auto queue_it = gap_lock_table_.find(tab_fd);
    if (queue_it == gap_lock_table_.end()) {
        queue_it =
            gap_lock_table_.emplace(std::piecewise_construct, std::forward_as_tuple(tab_fd), std::forward_as_tuple())
                .first;
    }
    GapLockRequestQueue& request_queue = queue_it->second;
    request_queue.request_queue_.emplace_back(txn->get_transaction_id(), std::move(conds));
    txn->get_lock_gap_set()->insert(tab_fd);
    return true;
}

/**
 * @description: 获取指定表和事务的间隙锁条件
 * @return {std::vector<Condition>} 返回该事务在该表上的所有间隙锁条件
 * @param {int} tab_fd 目标表的fd
 * @param {Transaction*} txn 事务对象指针
 */
std::vector<Condition> LockManager::get_gap_condition(int tab_fd, Transaction* txn) {
    std::unique_lock<std::mutex> lock(latch_);

    std::vector<Condition> all_conditions;

    // 查找该表的间隙锁队列
    auto it = gap_lock_table_.find(tab_fd);
    if (it != gap_lock_table_.end()) {
        // 遍历队列中的所有间隙锁请求
        for (const auto& request : it->second.request_queue_) {
            // 只返回其他事务的条件（不包括当前事务自己的）
            if (request.txn_id_ != txn->get_transaction_id()) {
                all_conditions.insert(all_conditions.end(), request.conds.begin(), request.conds.end());
            }
        }
    }

    return all_conditions;
}

/**
 * @description: 释放指定事务在指定表上的所有间隙锁
 * @return {bool} 返回解锁是否成功
 * @param {Transaction*} txn 要释放间隙锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::unlock_gap(Transaction* txn, int tab_fd) {
    std::unique_lock<std::mutex> lock(latch_);

    // 查找该表的间隙锁队列
    auto it = gap_lock_table_.find(tab_fd);
    if (it != gap_lock_table_.end()) {
        // 移除该事务的所有间隙锁请求
        auto& queue = it->second.request_queue_;
        queue.remove_if([txn](const GapLockRequest& request) {
            return request.txn_id_ == txn->get_transaction_id();
        });

        // 如果队列为空，删除该表的条目
        if (queue.empty()) {
            gap_lock_table_.erase(it);
        }
    }

    // 从事务的间隙锁集合中移除
    txn->get_lock_gap_set()->erase(tab_fd);

    return true;
}

/**
 * @description: 释放事务的所有间隙锁
 * @param {Transaction*} txn 要释放间隙锁的事务对象指针
 */
void LockManager::unlock_all_gap_locks(Transaction* txn) {
    std::unique_lock<std::mutex> lock(latch_);

    // 获取事务持有的所有间隙锁
    auto gap_set = txn->get_lock_gap_set();
    for (int tab_fd : *gap_set) {
        // 查找该表的间隙锁队列
        auto it = gap_lock_table_.find(tab_fd);
        if (it != gap_lock_table_.end()) {
            // 移除该事务的所有间隙锁请求
            auto& queue = it->second.request_queue_;
            queue.remove_if([txn](const GapLockRequest& request) {
                return request.txn_id_ == txn->get_transaction_id();
            });

            // 如果队列为空，删除该表的条目
            if (queue.empty()) {
                gap_lock_table_.erase(it);
            }
        }
    }

    // 清空事务的间隙锁集合
    gap_set->clear();
}