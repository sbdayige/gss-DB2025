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

/**
 * @description: 查找可以被替换的缓冲池页面
 *
 * 该函数按以下顺序查找可替换页面：
 * 1. 首先检查空闲链表(free_list_)是否有空闲帧
 * 2. 如果没有空闲帧，则使用替换策略(LRU)选择牺牲页面
 *
 * @param {frame_id_t*} frame_id 输出参数，返回找到的可替换帧的ID
 * @return {bool} true表示找到可替换帧，false表示没有可替换的帧
 * @note 该函数在调用时需要持有缓冲池的互斥锁
 */
bool BufferPoolInstance::find_victim_page(frame_id_t* frame_id) {
    // Todo:
    // !1 使用BufferPoolInstance::free_list_判断缓冲池是否已满需要淘汰页面
    // !1.1 未满获得frame
    // !1.2 已满使用lru_replacer中的方法选择淘汰页面

    // 如果有空闲帧,直接使用
    // Caller must hold the latch_ if free_list_ access needs protection.
    if (!free_list_.empty()) {
        *frame_id = free_list_.front();
        free_list_.pop_front();
        return true;
    }

    // 没有空闲帧,使用LRU策略淘汰一个页面
    return replacer_->victim(frame_id);
}

/**
 * @description: 更新页面的元数据和内容
 *
 * 该函数执行以下操作：
 * 1. 如果当前页面是脏页：
 *    - 将页面内容写回磁盘
 *    - 重置脏页标志
 * 2. 更新页表映射：
 *    - 删除旧的页面ID到帧的映射
 *    - 添加新的页面ID到帧的映射
 * 3. 重置页面内容：
 *    - 更新页面ID
 *    - 清空页面数据
 *
 * @param {Page*} page 要更新的页面指针
 * @param {PageId} new_page_id 新的页面ID
 * @param {frame_id_t} new_frame_id 新的帧ID
 * @note 调用者必须确保传入参数的有效性
 */
void BufferPoolInstance::update_page(Page* page, PageId new_page_id, frame_id_t new_frame_id) {
    // Todo:
    // !1 如果是脏页，写回磁盘，并且把dirty置为false
    // !2 更新page table
    // !3 重置page的data，更新page id

    // 如果是脏页,写回磁盘
    if (page->is_dirty_) {
        disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->data_, PAGE_SIZE);
    }

    // 从页表中删除旧映射
    page_table_.erase(page->id_);

    // 更新页表,添加新映射
    page->id_ = new_page_id;
    page_table_.emplace(new_page_id, new_frame_id);

    // 重置页面数据
    page->pin_count_ = 0;     // 重置pin_count
    page->is_dirty_ = false;  // 重置脏页标志
    page->reset_memory();
}

/**
 * @description: 获取指定的页面
 *
 * @param {PageId} page_id 需要获取的页面ID
 * @return {Page*} 获取的页面指针，如果无法获取则返回nullptr
 * @note 该函数使用互斥锁保护并发访问
 */
Page* BufferPoolInstance::fetch_page(PageId page_id) {
    // Todo:
    //  !1.     从page_table_中搜寻目标页
    //  !1.1
    //  !若目标页有被page_table_记录，则将其所在frame固定(pin)，并返回目标页。
    //  !1.2
    //  !否则，尝试调用find_victim_page获得一个可用的frame，若失败则返回nullptr
    //  !2.     若获得的可用frame存储的为dirty
    //  !page，则须调用updata_page将page写回到磁盘
    //  !3.     调用disk_manager_的read_page读取目标页到frame
    //  !4.     固定目标页，更新pin_count_
    //  !5.     返回目标页

    std::scoped_lock lock{latch_};

    // 在页表中查找目标页
    auto iter = page_table_.find(page_id);
    if (iter != page_table_.end()) {
        // 页面在缓冲池中,增加pin_count并返回
        frame_id_t frame_id = iter->second;
        Page* page = &pages_[frame_id];
        if (page->pin_count_ == 0) {
            // 如果pin_count为0说明是新取出的页
            // 需要在replacer中固定该页
            replacer_->pin(frame_id);
        }
        // replacer_->pin(frame_id);  // 固定该页
        page->pin_count_++;
        // INFO("fetch page: {}, pin_count: {}", page->get_page_id().page_no, page->pin_count_);
        return page;
    }

    // 页面不在缓冲池中,寻找可用frame
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return nullptr;  // 没有可用frame
    }

    // 获取victim frame对应的页面
    Page* page = &pages_[frame_id];
    update_page(page, page_id, frame_id);
    disk_manager_->read_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    page->pin_count_ = 1;  // 固定该页
    //! 本来就是新页不在缓存中，test中可以调用replacer_->unpin(frame_id)来固定该页，不保证
    replacer_->pin(frame_id);
    // INFO("fetch page: {}, pin_count: {}", page->get_page_id().page_no, page->pin_count_);
    return page;
}

/**
 * @description: 取消固定一个页面
 *
 * 该函数完成以下操作：
 * 1. 在页表中查找目标页面
 * 2. 如果页面不存在或已经完全解除固定(pin_count=0)，返回false
 * 3. 减少页面的引用计数：
 *    - 如果引用计数降为0，在替换器中标记为可替换
 * 4. 根据is_dirty参数更新页面的脏页标记
 *
 * @param {PageId} page_id 目标页面的ID
 * @param {bool} is_dirty 是否将页面标记为脏页
 * @return {bool} true表示成功解除固定，false表示操作失败
 * @note 该函数使用互斥锁保护并发访问
 */
bool BufferPoolInstance::unpin_page(PageId page_id, bool is_dirty) {
    // Todo:
    // !0. lock latch
    // !1. 尝试在page_table_中搜寻page_id对应的页P
    // !1.1 P在页表中不存在 return false
    // !1.2 P在页表中存在，获取其pin_count_
    // !2.1 若pin_count_已经等于0，则返回false
    // !2.2 若pin_count_大于0，则pin_count_自减一
    // !2.2.1 若自减后等于0，则调用replacer_的Unpin
    // !3 根据参数is_dirty，更改P的is_dirty_
    std::scoped_lock lock{latch_};

    // 在页表中查找目标页
    auto iter = page_table_.find(page_id);
    if (iter == page_table_.end()) {
        return false;  // 页面不在缓冲池中
    }

    // 获取frame和页面
    frame_id_t frame_id = iter->second;
    Page* page = &pages_[frame_id];

    // 检查pin_count
    if (page->pin_count_ == 0) {
        return false;  // pin_count已经为0
    }

    // 减少pin_count
    page->pin_count_--;
    // INFO("Unpinning page: {}, pin_count: {}", page->get_page_id().page_no, page->pin_count_);
    // 如果pin_count降为0,在replacer中取消固定
    if (page->pin_count_ == 0) {
        replacer_->unpin(frame_id);
    }

    // 更新dirty标记
    if (is_dirty) {
        page->is_dirty_ = true;
    }

    return true;
}

/**
 * @description: 将目标页写回磁盘，不考虑当前页面是否正在被使用
 * @return {bool} 成功则返回true，否则返回false(只有page_table_中没有目标页时)
 * @param {PageId} page_id 目标页的page_id，不能为INVALID_PAGE_ID
 */
bool BufferPoolInstance::flush_page(PageId page_id) {
    // Todo:
    // !0. lock latch
    // !1. 查找页表,尝试获取目标页P
    // !1.1 目标页P没有被page_table_记录 ，返回false
    // !2. 无论P是否为脏都将其写回磁盘。
    // !3. 更新P的is_dirty_

    std::scoped_lock lock{latch_};

    // 在页表中查找目标页
    auto iter = page_table_.find(page_id);
    if (iter == page_table_.end()) {
        return false;  // 页面不存在
    }

    // 获取目标页
    frame_id_t frame_id = iter->second;
    Page* page = &pages_[frame_id];

    // 写回磁盘
    disk_manager_->write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);

    // 更新dirty标记
    page->is_dirty_ = false;

    return true;
}

/**
 * @description: 创建新页面

 * @param {PageId*} page_id 输出参数，存储新创建页面的ID
 * @return {Page*} 新创建的页面指针，如果创建失败则返回nullptr
 * @note 该函数使用互斥锁保护并发访问
 */
Page* BufferPoolInstance::new_page(PageId* page_id) {
    // Todo:
    // !1.   获得一个可用的frame，若无法获得则返回nullptr
    // !2.   在fd对应的文件分配一个新的page_id
    // !3.   将frame的数据写回磁盘
    // !4.   固定frame，更新pin_count_
    // !5.   返回获得的page

    std::scoped_lock lock{latch_};

    // 找一个可用frame
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return nullptr;
    }

    // 获取frame对应的页面
    Page* page = &pages_[frame_id];
    update_page(page, *page_id, frame_id);
    page->pin_count_ = 1;  // 固定该页
    // INFO("new page: {}, pin_count: {}", page->get_page_id().page_no, page->pin_count_);
    //! 本来就是新页不在缓存中，test中可以调用replacer_->unpin(frame_id)来固定该页，不保证
    replacer_->pin(frame_id);

    return page;
}

/**
 * @description: 从buffer_pool删除目标页
 * @return {bool}
 * 如果目标页不存在于buffer_pool或者成功被删除则返回true，若其存在于buffer_pool但无法删除则返回false
 * @param {PageId} page_id 目标页
 */
bool BufferPoolInstance::delete_page(PageId page_id) {
    // 1.   在page_table_中查找目标页，若不存在返回true
    // 2.   若目标页的pin_count不为0，则返回false
    // 3.
    // 将目标页数据写回磁盘，从页表中删除目标页，重置其元数据，将其加入free_list_，返回true

    std::scoped_lock lock{latch_};

    // 在页表中查找目标页
    auto iter = page_table_.find(page_id);
    if (iter == page_table_.end()) {
        return true;  // 页面不存在,视为删除成功
    }

    // 获取frame和页面
    frame_id_t frame_id = iter->second;
    Page* page = &pages_[frame_id];

    // 检查pin_count
    if (page->pin_count_ != 0) {
        return false;  // 页面正在使用,不能删除
    }

    // 如果是脏页写回磁盘
    if (page->is_dirty_) {
        disk_manager_->write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    }

    // 重置页面元数据
    page->id_.page_no = INVALID_PAGE_ID;
    page->pin_count_ = 0;
    page->is_dirty_ = false;
    page->reset_memory();

    // 从页表中删除
    page_table_.erase(iter);

    // 将frame加入空闲列表
    free_list_.push_back(frame_id);

    return true;
}

/**
 * @description: 将指定文件的所有缓冲页刷新到磁盘
 *
 * 该函数执行以下操作：
 * 1. 遍历页表查找属于指定文件的所有页面
 * 2. 将找到的页面写回磁盘，不论是否为脏页
 * 3. 重置页面的脏页标记
 *
 * @param {int} fd 要刷新的文件描述符
 * @note 该函数使用互斥锁保护并发访问
 */
void BufferPoolInstance::flush_all_pages(int fd) {
    std::scoped_lock lock{latch_};

    // 遍历页表
    for (const auto& pair : page_table_) {
        if (pair.first.fd != fd) continue;
        const PageId page_id = pair.first;
        frame_id_t frame_id = pair.second;

        // 获取页面并写回磁盘
        Page* page = &pages_[frame_id];
        disk_manager_->write_page(fd, page_id.page_no, page->data_, PAGE_SIZE);
        page->is_dirty_ = false;
    }
}

/**
 * @description: 从缓冲池中删除指定文件的所有页面
 * @param {int} fd 要删除的文件描述符
 * @note 该函数使用互斥锁保护并发访问
 */
void BufferPoolInstance::delete_all_pages(int fd) {
    std::scoped_lock lock{latch_};

    for (auto it = page_table_.begin(); it != page_table_.end();) {
        if (it->first.fd == fd) {
            // 获取frame和页面
            frame_id_t frame_id = it->second;
            Page* page = &pages_[frame_id];

            // 如果是脏页写回磁盘
            if (page->is_dirty_) {
                disk_manager_->write_page(fd, page->id_.page_no, page->data_, PAGE_SIZE);
            }

            // 重置页面元数据
            page->id_.page_no = INVALID_PAGE_ID;
            page->pin_count_ = 0;
            page->is_dirty_ = false;
            page->reset_memory();

            // 从页表中删除
            it = page_table_.erase(it);

            // 将frame加入空闲列表
            free_list_.push_back(frame_id);
        } else {
            ++it;
        }
    }
}

auto BufferPoolInstance::new_page_guarded(PageId* page_id) -> BasicPageGuard {
    std::scoped_lock lock{latch_};
    // 找一个可用frame
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return {this, nullptr};  // 没有可用frame
    }

    // 获取frame对应的页面
    Page* page = &pages_[frame_id];
    update_page(page, *page_id, frame_id);
    page->pin_count_ = 1;
    replacer_->pin(frame_id);

    return {this, page};
}

auto BufferPoolInstance::fetch_page_basic(PageId page_id) -> BasicPageGuard {
    std::scoped_lock lock{latch_};

    // 在页表中查找目标页
    auto iter = page_table_.find(page_id);
    if (iter != page_table_.end()) {
        // 页面在缓冲池中,增加pin_count并返回
        frame_id_t frame_id = iter->second;
        Page* page = &pages_[frame_id];
        if (page->pin_count_ == 0) {
            replacer_->pin(frame_id);
        }
        page->pin_count_++;
        return {this, page};
    }

    // 页面不在缓冲池中,寻找可用frame
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return {this, nullptr};  // 没有可用frame
    }

    // 获取victim frame对应的页面
    Page* page = &pages_[frame_id];
    update_page(page, page_id, frame_id);
    disk_manager_->read_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
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
