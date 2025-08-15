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
#include <mutex>
#include <unordered_map>
#include <vector>

#include "disk_manager.h"
#include "errors.h"
#include "page.h"
#include "page_guard.h"
#include "replacer/lru_replacer.h"
#include "replacer/replacer.h"

extern DiskManager disk_manager;

class BufferPoolManager {
   private:
    // 缓冲池实例结构
    struct BufferPoolShard {
        size_t pool_size_;                                   // 缓冲池大小（帧数）
        Page* pages_;                                        // 缓冲池中的页面数组，连续分配
        std::unordered_map<PageId, frame_id_t> page_table_;  // 页面到帧的映射表
        std::list<frame_id_t> free_list_;                    // 空闲帧链表
        Replacer* replacer_;                                 // 页面替换策略实现
        std::mutex latch_;                                   // 并发控制锁

        BufferPoolShard(size_t pool_size) : pool_size_(pool_size) {
            pages_ = new Page[pool_size_];
            replacer_ = new LRUReplacer(pool_size_);
            for (size_t i = 0; i < pool_size_; ++i) {
                free_list_.emplace_back(static_cast<frame_id_t>(i));
            }
            page_table_.reserve(pool_size_);
        }

        ~BufferPoolShard() {
            delete[] pages_;
            delete replacer_;
        }
    };

    size_t pool_size_;          // buffer_pool中可容纳页面的个数，即帧的个数
    size_t total_instances_;    // 缓冲池实例总数
    std::hash<PageId> hasher_;  // 用于计算PageId的哈希值
    BufferPoolShard* buffer_pool_shards_[BUFFER_POOL_INSTANCE_SIZE];

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
            buffer_pool_shards_[i] = new BufferPoolShard(instance_pool_size);
        }
    }

    ~BufferPoolManager() {
        for (size_t i = 0; i < BUFFER_POOL_INSTANCE_SIZE; ++i) {
            delete buffer_pool_shards_[i];
        }
    }

    static void mark_dirty(Page* page) { page->is_dirty_ = true; }

   public:
    // 基础页面操作接口
    Page* fetch_page(PageId page_id);
    bool unpin_page(PageId page_id, bool is_dirty);
    bool flush_page(PageId page_id);
    Page* new_page(PageId* page_id);
    bool delete_page(PageId page_id);

    // 批量操作接口
    void flush_all_pages(int fd);
    void delete_all_pages(int fd);

    // 高级页面守护接口
    auto new_page_guarded(PageId* page_id) -> BasicPageGuard;
    auto fetch_page_basic(PageId page_id) -> BasicPageGuard;
    auto fetch_page_read(PageId page_id) -> ReadPageGuard;
    auto fetch_page_write(PageId page_id) -> WritePageGuard;

   private:
    // 内部辅助方法
    size_t get_instance_no(const PageId& page_id) const {
        return hasher_(page_id) % BUFFER_POOL_INSTANCE_SIZE;
    }

    bool find_victim_page(BufferPoolShard* shard, frame_id_t* frame_id);
    void update_page(BufferPoolShard* shard, Page* page, PageId new_page_id, frame_id_t new_frame_id);

   public:
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