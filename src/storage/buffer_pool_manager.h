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
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <list>
#include <unordered_map>
#include <vector>

#include "buffer_pool_instance.h"
#include "disk_manager.h"
#include "errors.h"
#include "page.h"
#include "page_guard.h"

extern DiskManager disk_manager;

/**
 * @brief 高性能缓冲池管理器
 *
 * 采用分片设计，支持多线程并发访问，
 * 优化的哈希策略和LRU替换算法
 */
class BufferPoolManager {
   private:
    size_t pool_size_;          // buffer_pool中可容纳页面的个数，即帧的个数
    size_t total_instances_;    // 缓冲池实例总数
    std::hash<PageId> hasher_;  // 用于计算PageId的哈希值
    BufferPoolInstance *buffer_pool_instances_[BUFFER_POOL_INSTANCE_SIZE];

    // 性能监控相关
    mutable std::atomic<size_t> cache_hits_{0};
    mutable std::atomic<size_t> cache_misses_{0};
    mutable std::atomic<size_t> total_requests_{0};

   public:
    BufferPoolManager(size_t pool_size) : pool_size_(pool_size), total_instances_(BUFFER_POOL_INSTANCE_SIZE) {
        // 为buffer pool分配一块连续的内存空间
        size_t instance_pool_size = pool_size / BUFFER_POOL_INSTANCE_SIZE;
        if (instance_pool_size == 0) {
            instance_pool_size = 1;  // 确保每个实例至少有一个页面
        }

        for (size_t i = 0; i < BUFFER_POOL_INSTANCE_SIZE; ++i) {
            buffer_pool_instances_[i] = new BufferPoolInstance(instance_pool_size);
        }
    }

    ~BufferPoolManager() {
        for (size_t i = 0; i < BUFFER_POOL_INSTANCE_SIZE; ++i) {
            delete buffer_pool_instances_[i];
        }
    }

   public:
    // 基础页面操作接口
    Page *fetch_page(PageId page_id);
    bool unpin_page(PageId page_id, bool is_dirty);
    bool flush_page(PageId page_id);
    Page *new_page(PageId *page_id);
    bool delete_page(PageId page_id);

    // 批量操作接口
    void flush_all_pages(int fd);
    void delete_all_pages(int fd);

    // 高级页面守护接口
    auto new_page_guarded(PageId *page_id) -> BasicPageGuard;
    auto fetch_page_basic(PageId page_id) -> BasicPageGuard;
    auto fetch_page_read(PageId page_id) -> ReadPageGuard;
    auto fetch_page_write(PageId page_id) -> WritePageGuard;

    // 工具方法
    size_t get_instance_no(const PageId &page_id) const {
        return hasher_(page_id) % BUFFER_POOL_INSTANCE_SIZE;
    }

    // 性能统计接口
    double get_cache_hit_ratio() const {
        size_t total = total_requests_.load();
        return total > 0 ? static_cast<double>(cache_hits_.load()) / total : 0.0;
    }

    void reset_statistics() {
        cache_hits_.store(0);
        cache_misses_.store(0);
        total_requests_.store(0);
    }
};