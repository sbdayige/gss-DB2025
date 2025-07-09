/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_scan.h"

/**
 * @brief 构造函数，初始化迭代器并对第一个页面加锁
 */
IxScan::IxScan(const IxIndexHandle *ih, const Iid &lower, const Iid &upper, BufferPoolManager *bpm)
    : ih_(ih), iid_(lower), end_(upper), bpm_(bpm), current_page_(nullptr) {
    if (!is_end()) {
        lock_current_page();
    }
}

/**
 * @brief 析构函数，释放当前持有的锁
 */
IxScan::~IxScan() {
    unlock_current_page();
}

/**
 * @brief 移动到下一个元素，实现蟹行协议的并发控制
 */
void IxScan::next() {
    assert(!is_end());
    IxNodeHandle *node = ih_->fetch_node(iid_.page_no);
    assert(node->is_leaf_page());
    assert(iid_.slot_no < node->get_size());

    // increment slot no
    iid_.slot_no++;
    if (iid_.page_no != ih_->file_hdr_->last_leaf_ && iid_.slot_no == node->get_size()) {
        // 需要移动到下一页
        page_id_t next_page_no = node->get_next_leaf();

        // 先对下一页加锁
        if (next_page_no != INVALID_PAGE_ID) {
            // 通过节点获取页面ID
            IxNodeHandle *next_node = ih_->fetch_node(next_page_no);
            Page *next_page = bpm_->fetch_page(next_node->get_page_id());
            next_page->RLatch();

            // 释放当前页锁
            unlock_current_page();

            // 更新当前页
            current_page_ = next_page;
            iid_.slot_no = 0;
            iid_.page_no = next_page_no;

            // 清理临时节点
            bpm_->unpin_page(next_node->get_page_id(), false);
            delete next_node;
        } else {
            // 到达末尾
            unlock_current_page();
            iid_ = end_;
        }
    }

    bpm_->unpin_page(node->get_page_id(), false);
    delete node;
}

/**
 * @brief 获取当前位置的记录ID
 */
Rid IxScan::rid() const {
    return ih_->get_rid(iid_);
}

/**
 * @brief 对当前页面加读锁
 */
void IxScan::lock_current_page() {
    if (iid_.page_no != INVALID_PAGE_ID) {
        // 通过节点获取页面ID
        IxNodeHandle *node = ih_->fetch_node(iid_.page_no);
        current_page_ = bpm_->fetch_page(node->get_page_id());
        current_page_->RLatch();

        // 清理临时节点
        bpm_->unpin_page(node->get_page_id(), false);
        delete node;
    }
}

/**
 * @brief 释放当前页面的读锁
 */
void IxScan::unlock_current_page() {
    if (current_page_) {
        current_page_->RUnlatch();
        bpm_->unpin_page(current_page_->get_page_id(), false);
        current_page_ = nullptr;
    }
}
