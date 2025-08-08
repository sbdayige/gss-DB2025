/**
 * @brief 高性能缓冲池管理器实现
 *
 * 提供优化的页面管理和缓存统计功能
 */

#include "buffer_pool_manager.h"

extern DiskManager disk_manager;

/**
 * @brief 获取指定页面（带统计）
 */
Page* BufferPoolManager::fetch_page(PageId page_id) {
    total_requests_.fetch_add(1, std::memory_order_relaxed);

    size_t instance_no = get_instance_no(page_id);
    auto* page = buffer_pool_instances_[instance_no]->fetch_page(page_id);

    if (page != nullptr) {
        cache_hits_.fetch_add(1, std::memory_order_relaxed);
    } else {
        cache_misses_.fetch_add(1, std::memory_order_relaxed);
    }

    return page;
}

/**
 * @brief 解除页面固定状态
 */
bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    return buffer_pool_instances_[get_instance_no(page_id)]->unpin_page(page_id, is_dirty);
}

/**
 * @brief 刷新指定页面到磁盘
 */
bool BufferPoolManager::flush_page(PageId page_id) {
    return buffer_pool_instances_[get_instance_no(page_id)]->flush_page(page_id);
}

/**
 * @brief 分配新页面
 */
Page* BufferPoolManager::new_page(PageId* page_id) {
    page_id->page_no = disk_manager.allocate_page(page_id->fd);
    return buffer_pool_instances_[get_instance_no(*page_id)]->new_page(page_id);
}

/**
 * @brief 删除指定页面
 */
bool BufferPoolManager::delete_page(PageId page_id) {
    return buffer_pool_instances_[get_instance_no(page_id)]->delete_page(page_id);
}

/**
 * @brief 批量刷新指定文件的所有页面
 */
void BufferPoolManager::flush_all_pages(int fd) {
    // 并行刷新所有实例的页面
    for (size_t i = 0; i < total_instances_; ++i) {
        buffer_pool_instances_[i]->flush_all_pages(fd);
    }
}

/**
 * @brief 批量删除指定文件的所有页面
 */
void BufferPoolManager::delete_all_pages(int fd) {
    // 并行删除所有实例的页面
    for (size_t i = 0; i < total_instances_; ++i) {
        buffer_pool_instances_[i]->delete_all_pages(fd);
    }
}

/**
 * @brief 创建带守护的新页面
 */
auto BufferPoolManager::new_page_guarded(PageId* page_id) -> BasicPageGuard {
    page_id->page_no = disk_manager.allocate_page(page_id->fd);
    return buffer_pool_instances_[get_instance_no(*page_id)]->new_page_guarded(page_id);
}

/**
 * @brief 获取带基础守护的页面
 */
auto BufferPoolManager::fetch_page_basic(PageId page_id) -> BasicPageGuard {
    total_requests_.fetch_add(1, std::memory_order_relaxed);
    return buffer_pool_instances_[get_instance_no(page_id)]->fetch_page_basic(page_id);
}

/**
 * @brief 获取带读守护的页面
 */
auto BufferPoolManager::fetch_page_read(PageId page_id) -> ReadPageGuard {
    total_requests_.fetch_add(1, std::memory_order_relaxed);
    return buffer_pool_instances_[get_instance_no(page_id)]->fetch_page_read(page_id);
}

/**
 * @brief 获取带写守护的页面
 */
auto BufferPoolManager::fetch_page_write(PageId page_id) -> WritePageGuard {
    total_requests_.fetch_add(1, std::memory_order_relaxed);
    return buffer_pool_instances_[get_instance_no(page_id)]->fetch_page_write(page_id);
}