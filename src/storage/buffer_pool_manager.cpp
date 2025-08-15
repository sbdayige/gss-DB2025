/**
 * @brief 高性能缓冲池管理器实现
 *
 * 合并了BufferPoolInstance功能，提供优化的页面管理和缓存统计功能
 */

#include "buffer_pool_manager.h"

extern DiskManager disk_manager;

// 内部辅助方法实现
bool BufferPoolManager::find_victim_page(BufferPoolShard* shard, frame_id_t* frame_id) {
    // 优先使用空闲帧
    if (!shard->free_list_.empty()) {
        *frame_id = shard->free_list_.front();
        shard->free_list_.pop_front();
        return true;
    }

    // 没有空闲帧时，使用替换策略
    return shard->replacer_->victim(frame_id);
}

void BufferPoolManager::update_page(BufferPoolShard* shard, Page* page, PageId new_page_id, frame_id_t new_frame_id) {
    // 如果是脏页，先写回磁盘
    if (page->is_dirty_) {
        disk_manager.write_page(page->id_.fd, page->id_.page_no, page->data_, PAGE_SIZE);
    }

    // 更新页表映射
    shard->page_table_.erase(page->id_);
    page->id_ = new_page_id;
    shard->page_table_.emplace(new_page_id, new_frame_id);

    // 重置页面状态
    page->pin_count_ = 0;     // 重置pin_count
    page->is_dirty_ = false;  // 重置脏页标志
    page->reset_memory();
}

/**
 * @brief 获取指定页面（带统计）
 */
Page* BufferPoolManager::fetch_page(PageId page_id) {
    total_requests_.fetch_add(1, std::memory_order_relaxed);

    size_t instance_no = get_instance_no(page_id);
    BufferPoolShard* shard = buffer_pool_shards_[instance_no];

    std::scoped_lock lock{shard->latch_};

    // 检查页面是否已在缓冲池中
    const auto page_iter = shard->page_table_.find(page_id);
    if (page_iter != shard->page_table_.end()) {
        const frame_id_t frame_id = page_iter->second;
        Page* existing_page = &shard->pages_[frame_id];

        if (existing_page->pin_count_ == 0) {
            shard->replacer_->pin(frame_id);
        }
        existing_page->pin_count_++;
        cache_hits_.fetch_add(1, std::memory_order_relaxed);
        return existing_page;
    }

    // 页面不在缓冲池中，需要从磁盘加载
    frame_id_t frame_id;
    const bool found_victim = find_victim_page(shard, &frame_id);
    if (!found_victim) {
        cache_misses_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;  // 没有可用frame
    }

    Page* target_page = &shard->pages_[frame_id];
    update_page(shard, target_page, page_id, frame_id);
    disk_manager.read_page(page_id.fd, page_id.page_no, target_page->data_, PAGE_SIZE);
    target_page->pin_count_ = 1;  // 固定该页
    shard->replacer_->pin(frame_id);

    cache_hits_.fetch_add(1, std::memory_order_relaxed);
    return target_page;
}

/**
 * @brief 解除页面固定状态
 */
bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    size_t instance_no = get_instance_no(page_id);
    BufferPoolShard* shard = buffer_pool_shards_[instance_no];

    std::scoped_lock lock{shard->latch_};

    const auto page_iter = shard->page_table_.find(page_id);
    if (page_iter == shard->page_table_.end()) {
        return false;  // 页面不在缓冲池中
    }

    const frame_id_t frame_id = page_iter->second;
    Page* target_page = &shard->pages_[frame_id];

    if (target_page->pin_count_ == 0) {
        return false;  // pin_count已经为0
    }

    target_page->pin_count_--;
    if (target_page->pin_count_ == 0) {
        shard->replacer_->unpin(frame_id);
    }

    if (is_dirty) {
        target_page->is_dirty_ = true;
    }

    return true;
}

/**
 * @brief 刷新指定页面到磁盘
 */
bool BufferPoolManager::flush_page(PageId page_id) {
    size_t instance_no = get_instance_no(page_id);
    BufferPoolShard* shard = buffer_pool_shards_[instance_no];

    std::scoped_lock lock{shard->latch_};

    const auto page_iter = shard->page_table_.find(page_id);
    if (page_iter == shard->page_table_.end()) {
        return false;  // 页面不存在
    }

    const frame_id_t frame_id = page_iter->second;
    Page* target_page = &shard->pages_[frame_id];

    disk_manager.write_page(page_id.fd, page_id.page_no, target_page->data_, PAGE_SIZE);
    target_page->is_dirty_ = false;

    return true;
}

/**
 * @brief 分配新页面
 */
Page* BufferPoolManager::new_page(PageId* page_id) {
    page_id->page_no = disk_manager.allocate_page(page_id->fd);

    size_t instance_no = get_instance_no(*page_id);
    BufferPoolShard* shard = buffer_pool_shards_[instance_no];

    std::scoped_lock lock{shard->latch_};

    frame_id_t frame_id;
    const bool found_victim = find_victim_page(shard, &frame_id);
    if (!found_victim) {
        return nullptr;
    }

    Page* target_page = &shard->pages_[frame_id];
    update_page(shard, target_page, *page_id, frame_id);
    target_page->pin_count_ = 1;  // 固定该页
    shard->replacer_->pin(frame_id);

    return target_page;
}

/**
 * @brief 删除指定页面
 */
bool BufferPoolManager::delete_page(PageId page_id) {
    size_t instance_no = get_instance_no(page_id);
    BufferPoolShard* shard = buffer_pool_shards_[instance_no];

    std::scoped_lock lock{shard->latch_};

    const auto page_iter = shard->page_table_.find(page_id);
    if (page_iter == shard->page_table_.end()) {
        return true;  // 页面不存在,视为删除成功
    }

    const frame_id_t frame_id = page_iter->second;
    Page* target_page = &shard->pages_[frame_id];

    if (target_page->pin_count_ != 0) {
        return false;  // 页面正在使用,不能删除
    }

    // 如果是脏页，先写回磁盘
    if (target_page->is_dirty_) {
        disk_manager.write_page(page_id.fd, page_id.page_no, target_page->data_, PAGE_SIZE);
    }

    // 重置页面状态并回收frame
    target_page->id_.page_no = INVALID_PAGE_ID;
    target_page->pin_count_ = 0;
    target_page->is_dirty_ = false;
    target_page->reset_memory();
    shard->page_table_.erase(page_iter);
    shard->free_list_.push_back(frame_id);

    return true;
}

/**
 * @brief 批量刷新指定文件的所有页面
 */
void BufferPoolManager::flush_all_pages(int fd) {
    // 并行刷新所有分片的页面
    for (size_t i = 0; i < total_instances_; ++i) {
        BufferPoolShard* shard = buffer_pool_shards_[i];
        std::scoped_lock lock{shard->latch_};

        for (const auto& [page_id, frame_id] : shard->page_table_) {
            if (page_id.fd != fd) {
                continue;
            }

            Page* target_page = &shard->pages_[frame_id];
            disk_manager.write_page(fd, page_id.page_no, target_page->data_, PAGE_SIZE);
            target_page->is_dirty_ = false;
        }
    }
}

/**
 * @brief 批量删除指定文件的所有页面
 */
void BufferPoolManager::delete_all_pages(int fd) {
    // 并行删除所有分片的页面
    for (size_t i = 0; i < total_instances_; ++i) {
        BufferPoolShard* shard = buffer_pool_shards_[i];
        std::scoped_lock lock{shard->latch_};

        for (auto page_iter = shard->page_table_.begin(); page_iter != shard->page_table_.end();) {
            if (page_iter->first.fd == fd) {
                const frame_id_t frame_id = page_iter->second;
                Page* target_page = &shard->pages_[frame_id];

                // 如果是脏页，先写回磁盘
                if (target_page->is_dirty_) {
                    disk_manager.write_page(fd, target_page->id_.page_no, target_page->data_, PAGE_SIZE);
                }

                // 重置页面状态并回收frame
                target_page->id_.page_no = INVALID_PAGE_ID;
                target_page->pin_count_ = 0;
                target_page->is_dirty_ = false;
                target_page->reset_memory();
                page_iter = shard->page_table_.erase(page_iter);
                shard->free_list_.push_back(frame_id);
            } else {
                ++page_iter;
            }
        }
    }
}

/**
 * @brief 创建带守护的新页面
 */
auto BufferPoolManager::new_page_guarded(PageId* page_id) -> BasicPageGuard {
    page_id->page_no = disk_manager.allocate_page(page_id->fd);

    size_t instance_no = get_instance_no(*page_id);
    BufferPoolShard* shard = buffer_pool_shards_[instance_no];

    std::scoped_lock lock{shard->latch_};

    frame_id_t frame_id;
    const bool found_victim = find_victim_page(shard, &frame_id);
    if (!found_victim) {
        return BasicPageGuard{this, nullptr};  // 没有可用frame
    }

    Page* target_page = &shard->pages_[frame_id];
    update_page(shard, target_page, *page_id, frame_id);
    target_page->pin_count_ = 1;
    shard->replacer_->pin(frame_id);

    return BasicPageGuard{this, target_page};
}

/**
 * @brief 获取带基础守护的页面
 */
auto BufferPoolManager::fetch_page_basic(PageId page_id) -> BasicPageGuard {
    total_requests_.fetch_add(1, std::memory_order_relaxed);

    size_t instance_no = get_instance_no(page_id);
    BufferPoolShard* shard = buffer_pool_shards_[instance_no];

    std::scoped_lock lock{shard->latch_};

    // 首先检查页面是否已在缓冲池中
    const auto page_iter = shard->page_table_.find(page_id);
    if (page_iter != shard->page_table_.end()) {
        const frame_id_t frame_id = page_iter->second;
        Page* existing_page = &shard->pages_[frame_id];

        if (existing_page->pin_count_ == 0) {
            shard->replacer_->pin(frame_id);
        }
        existing_page->pin_count_++;
        return BasicPageGuard{this, existing_page};
    }

    // 页面不在缓冲池中，需要从磁盘加载
    frame_id_t frame_id;
    const bool found_victim = find_victim_page(shard, &frame_id);
    if (!found_victim) {
        return BasicPageGuard{this, nullptr};  // 没有可用frame
    }

    Page* target_page = &shard->pages_[frame_id];
    update_page(shard, target_page, page_id, frame_id);
    disk_manager.read_page(page_id.fd, page_id.page_no, target_page->data_, PAGE_SIZE);
    target_page->pin_count_ = 1;  // 固定该页
    shard->replacer_->pin(frame_id);

    return BasicPageGuard{this, target_page};
}

/**
 * @brief 获取带读守护的页面
 */
auto BufferPoolManager::fetch_page_read(PageId page_id) -> ReadPageGuard {
    total_requests_.fetch_add(1, std::memory_order_relaxed);

    auto basic_guard = fetch_page_basic(page_id);
    if (basic_guard.page_ == nullptr) {
        return ReadPageGuard{this, nullptr};  // 没有可用页面
    }

    basic_guard.page_->RLatch();  // 获取读锁
    return ReadPageGuard{this, basic_guard.page_};
}

/**
 * @brief 获取带写守护的页面
 */
auto BufferPoolManager::fetch_page_write(PageId page_id) -> WritePageGuard {
    total_requests_.fetch_add(1, std::memory_order_relaxed);

    auto basic_guard = fetch_page_basic(page_id);
    if (basic_guard.page_ == nullptr) {
        return WritePageGuard{this, nullptr};  // 没有可用页面
    }

    basic_guard.page_->WLatch();  // 获取写锁
    return WritePageGuard{this, basic_guard.page_};
}