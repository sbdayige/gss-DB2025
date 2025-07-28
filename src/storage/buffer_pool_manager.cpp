/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL
v2. You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "buffer_pool_manager.h"

extern DiskManager disk_manager;

Page* BufferPoolManager::fetch_page(PageId page_id) {
    return buffer_pool_instances_[get_instance_no(page_id)]->fetch_page(page_id);
}

bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    return buffer_pool_instances_[get_instance_no(page_id)]->unpin_page(page_id, is_dirty);
}

bool BufferPoolManager::flush_page(PageId page_id) {
    return buffer_pool_instances_[get_instance_no(page_id)]->flush_page(page_id);
}

Page* BufferPoolManager::new_page(PageId* page_id) {
    page_id->page_no = disk_manager.allocate_page(page_id->fd);
    return buffer_pool_instances_[get_instance_no(*page_id)]->new_page(page_id);
}

bool BufferPoolManager::delete_page(PageId page_id) {
    return buffer_pool_instances_[get_instance_no(page_id)]->delete_page(page_id);
}

void BufferPoolManager::flush_all_pages(int fd) {
    for (size_t i = 0; i < BUFFER_POOL_INSTANCE_SIZE; ++i) {
        buffer_pool_instances_[i]->flush_all_pages(fd);
    }
}

void BufferPoolManager::delete_all_pages(int fd) {
    for (size_t i = 0; i < BUFFER_POOL_INSTANCE_SIZE; ++i) {
        buffer_pool_instances_[i]->delete_all_pages(fd);
    }
}

auto BufferPoolManager::new_page_guarded(PageId* page_id) -> BasicPageGuard {
    page_id->page_no = disk_manager.allocate_page(page_id->fd);
    return buffer_pool_instances_[get_instance_no(*page_id)]->new_page_guarded(page_id);
}

auto BufferPoolManager::fetch_page_basic(PageId page_id) -> BasicPageGuard {
    return buffer_pool_instances_[get_instance_no(page_id)]->fetch_page_basic(page_id);
}

auto BufferPoolManager::fetch_page_read(PageId page_id) -> ReadPageGuard {
    return buffer_pool_instances_[get_instance_no(page_id)]->fetch_page_read(page_id);
}

auto BufferPoolManager::fetch_page_write(PageId page_id) -> WritePageGuard {
    return buffer_pool_instances_[get_instance_no(page_id)]->fetch_page_write(page_id);
}