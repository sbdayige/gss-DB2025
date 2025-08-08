/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL
v2. You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "buffer_pool_instance.h"

extern DiskManager disk_manager;

auto BufferPoolInstance::new_page_guarded(PageId* page_id) -> BasicPageGuard {
    std::scoped_lock lock{latch_};
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return {this, nullptr};  // 没有可用frame
    }
    Page* page = &pages_[frame_id];
    update_page(page, *page_id, frame_id);
    page->pin_count_ = 1;
    replacer_->pin(frame_id);

    return {this, page};
}

auto BufferPoolInstance::fetch_page_basic(PageId page_id) -> BasicPageGuard {
    std::scoped_lock lock{latch_};
    auto iter = page_table_.find(page_id);
    if (iter != page_table_.end()) {
        frame_id_t frame_id = iter->second;
        Page* page = &pages_[frame_id];
        if (page->pin_count_ == 0) {
            replacer_->pin(frame_id);
        }
        page->pin_count_++;
        return {this, page};
    }
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return {this, nullptr};  // 没有可用frame
    }
    Page* page = &pages_[frame_id];
    update_page(page, page_id, frame_id);
    disk_manager.read_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    page->pin_count_ = 1;  // 固定该页
    replacer_->pin(frame_id);

    return {this, page};
}

auto BufferPoolInstance::fetch_page_read(PageId page_id) -> ReadPageGuard {
    auto basic_guard = fetch_page_basic(page_id);
    if (basic_guard.page_ == nullptr) {
        return {this, nullptr};  // 没有可用页面
    }
    basic_guard.page_->RLatch();  // 获取读锁
    return {this, basic_guard.page_};
}

auto BufferPoolInstance::fetch_page_write(PageId page_id) -> WritePageGuard {
    auto basic_guard = fetch_page_basic(page_id);
    if (basic_guard.page_ == nullptr) {
        return {this, nullptr};  // 没有可用页面
    }
    basic_guard.page_->WLatch();  // 获取写锁
    return {this, basic_guard.page_};
}
bool BufferPoolInstance::find_victim_page(frame_id_t* frame_id) {
    if (!free_list_.empty()) {
        *frame_id = free_list_.front();
        free_list_.pop_front();
        return true;
    }
    return replacer_->victim(frame_id);
}

void BufferPoolInstance::update_page(Page* page, PageId new_page_id, frame_id_t new_frame_id) {
    if (page->is_dirty_) {
        disk_manager.write_page(page->id_.fd, page->id_.page_no, page->data_, PAGE_SIZE);
    }
    page_table_.erase(page->id_);
    page->id_ = new_page_id;
    page_table_.emplace(new_page_id, new_frame_id);
    page->pin_count_ = 0;     // 重置pin_count
    page->is_dirty_ = false;  // 重置脏页标志
    page->reset_memory();
}

Page* BufferPoolInstance::fetch_page(PageId page_id) {
    std::scoped_lock lock{latch_};
    auto iter = page_table_.find(page_id);
    if (iter != page_table_.end()) {
        frame_id_t frame_id = iter->second;
        Page* page = &pages_[frame_id];
        if (page->pin_count_ == 0) {
            replacer_->pin(frame_id);
        }
        page->pin_count_++;
        return page;
    }
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return nullptr;  // 没有可用frame
    }
    Page* page = &pages_[frame_id];
    update_page(page, page_id, frame_id);
    disk_manager.read_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    page->pin_count_ = 1;  // 固定该页
    replacer_->pin(frame_id);
    return page;
}

bool BufferPoolInstance::unpin_page(PageId page_id, bool is_dirty) {
    std::scoped_lock lock{latch_};
    auto iter = page_table_.find(page_id);
    if (iter == page_table_.end()) {
        return false;  // 页面不在缓冲池中
    }
    frame_id_t frame_id = iter->second;
    Page* page = &pages_[frame_id];
    if (page->pin_count_ == 0) {
        return false;  // pin_count已经为0
    }
    page->pin_count_--;
    if (page->pin_count_ == 0) {
        replacer_->unpin(frame_id);
    }
    if (is_dirty) {
        page->is_dirty_ = true;
    }
    return true;
}

bool BufferPoolInstance::flush_page(PageId page_id) {
    std::scoped_lock lock{latch_};
    auto iter = page_table_.find(page_id);
    if (iter == page_table_.end()) {
        return false;  // 页面不存在
    }
    frame_id_t frame_id = iter->second;
    Page* page = &pages_[frame_id];
    disk_manager.write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    page->is_dirty_ = false;
    return true;
}

Page* BufferPoolInstance::new_page(PageId* page_id) {
    std::scoped_lock lock{latch_};
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return nullptr;
    }
    Page* page = &pages_[frame_id];
    update_page(page, *page_id, frame_id);
    page->pin_count_ = 1;  // 固定该页
    replacer_->pin(frame_id);
    return page;
}

bool BufferPoolInstance::delete_page(PageId page_id) {
    std::scoped_lock lock{latch_};
    auto iter = page_table_.find(page_id);
    if (iter == page_table_.end()) {
        return true;  // 页面不存在,视为删除成功
    }
    frame_id_t frame_id = iter->second;
    Page* page = &pages_[frame_id];
    if (page->pin_count_ != 0) {
        return false;  // 页面正在使用,不能删除
    }
    if (page->is_dirty_) {
        disk_manager.write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    }
    page->id_.page_no = INVALID_PAGE_ID;
    page->pin_count_ = 0;
    page->is_dirty_ = false;
    page->reset_memory();
    page_table_.erase(iter);
    free_list_.push_back(frame_id);
    return true;
}

void BufferPoolInstance::flush_all_pages(int fd) {
    std::scoped_lock lock{latch_};
    for (const auto& pair : page_table_) {
        if (pair.first.fd != fd) continue;
        const PageId page_id = pair.first;
        frame_id_t frame_id = pair.second;
        Page* page = &pages_[frame_id];
        disk_manager.write_page(fd, page_id.page_no, page->data_, PAGE_SIZE);
        page->is_dirty_ = false;
    }
}

void BufferPoolInstance::delete_all_pages(int fd) {
    std::scoped_lock lock{latch_};
    for (auto it = page_table_.begin(); it != page_table_.end();) {
        if (it->first.fd == fd) {
            frame_id_t frame_id = it->second;
            Page* page = &pages_[frame_id];
            if (page->is_dirty_) {
                disk_manager.write_page(fd, page->id_.page_no, page->data_, PAGE_SIZE);
            }
            page->id_.page_no = INVALID_PAGE_ID;
            page->pin_count_ = 0;
            page->is_dirty_ = false;
            page->reset_memory();
            it = page_table_.erase(it);
            free_list_.push_back(frame_id);
        } else {
            ++it;
        }
    }
}
