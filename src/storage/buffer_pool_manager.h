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

#include <cassert>
#include <list>
#include <unordered_map>
#include <vector>

#include "buffer_pool_instance.h"
#include "disk_manager.h"
#include "errors.h"
#include "page.h"
#include "page_guard.h"

class BufferPoolManager {
   private:
    size_t pool_size_;  // buffer_pool中可容纳页面的个数，即帧的个数
    // Page *pages_;                                                    // buffer_pool中的Page对象数组，在构造空间中申请内存空间，在析构函数中释放，大小为BUFFER_POOL_SIZE
    // std::unordered_map<PageId, frame_id_t, PageIdHash> page_table_;  // 帧号和页面号的映射哈希表，用于根据页面的PageId定位该页面的帧编号
    // std::list<frame_id_t> free_list_;                                // 空闲帧编号的链表
    DiskManager *disk_manager_;
    // Replacer *replacer_;  // buffer_pool的置换策略，当前赛题中为LRU置换策略
    // std::mutex latch_;    // 用于共享数据结构的并发控制
    std::hash<PageId> hasher_;  // 用于计算PageId的哈希值
    BufferPoolInstance *buffer_pool_instances_[BUFFER_POOL_INSTANCE_SIZE];

   public:
    BufferPoolManager(size_t pool_size, DiskManager *disk_manager)
        : pool_size_(pool_size), disk_manager_(disk_manager) {
        // 为buffer pool分配一块连续的内存空间
        for (size_t i = 0; i < BUFFER_POOL_INSTANCE_SIZE; ++i) {
            buffer_pool_instances_[i] = new BufferPoolInstance(pool_size / BUFFER_POOL_INSTANCE_SIZE, disk_manager);
        }
    }

    ~BufferPoolManager() {
        for (size_t i = 0; i < BUFFER_POOL_INSTANCE_SIZE; ++i) {
            delete buffer_pool_instances_[i];
        }
    }

   public:
    Page *fetch_page(PageId page_id);

    bool unpin_page(PageId page_id, bool is_dirty);

    bool flush_page(PageId page_id);

    Page *new_page(PageId *page_id);

    bool delete_page(PageId page_id);

    void flush_all_pages(int fd);

    void delete_all_pages(int fd);

    size_t get_instance_no(const PageId &page_id) const { return hasher_(page_id) % BUFFER_POOL_INSTANCE_SIZE; }

    auto new_page_guarded(PageId *page_id) -> BasicPageGuard;

    auto fetch_page_basic(PageId page_id) -> BasicPageGuard;

    auto fetch_page_read(PageId page_id) -> ReadPageGuard;

    auto fetch_page_write(PageId page_id) -> WritePageGuard;
};