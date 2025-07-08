#include "storage/page_guard.h"

#include "storage/buffer_pool_manager.h"

// BasicPageGuard 实现

BasicPageGuard::BasicPageGuard(BasicPageGuard &&that) noexcept
    : bpi_(that.bpi_), page_(that.page_), is_dirty_(that.is_dirty_) {
    // 将 that 的资源转移给当前对象后，清空 that 的资源
    that.bpi_ = nullptr;
    that.page_ = nullptr;
    that.is_dirty_ = false;
}

void BasicPageGuard::Drop() {
    if (page_ != nullptr && bpi_ != nullptr) {
        // 如果页面被修改过，需要标记为脏页
        bpi_->unpin_page(page_->get_page_id(), is_dirty_);
        // 清空所有内容
        bpi_ = nullptr;
        page_ = nullptr;
        is_dirty_ = false;
    }
}

auto BasicPageGuard::operator=(BasicPageGuard &&that) noexcept -> BasicPageGuard & {
    if (this != &that) {
        // 如果当前对象已经管理着一个页面，需要先释放它
        Drop();

        // 转移资源
        bpi_ = that.bpi_;
        page_ = that.page_;
        is_dirty_ = that.is_dirty_;

        // 清空源对象
        that.bpi_ = nullptr;
        that.page_ = nullptr;
        that.is_dirty_ = false;
    }
    return *this;
}

BasicPageGuard::~BasicPageGuard() { Drop(); }

// ReadPageGuard 实现

ReadPageGuard::ReadPageGuard(ReadPageGuard &&that) noexcept : guard_(std::move(that.guard_)) {
    // 基础 guard_ 的移动构造函数已经处理了资源转移
}

auto ReadPageGuard::operator=(ReadPageGuard &&that) noexcept -> ReadPageGuard & {
    if (this != &that) {
        // 先释放当前持有的资源
        Drop();

        // 转移资源
        guard_ = std::move(that.guard_);
    }
    return *this;
}

void ReadPageGuard::Drop() {
    if (guard_.page_ != nullptr) {
        // 释放读锁
        guard_.page_->RUnlatch();
        // 释放基础guard持有的资源
        guard_.Drop();
    }
}

ReadPageGuard::~ReadPageGuard() { Drop(); }

// WritePageGuard 实现

WritePageGuard::WritePageGuard(WritePageGuard &&that) noexcept : guard_(std::move(that.guard_)) {
    // 基础 guard_ 的移动构造函数已经处理了资源转移
}

auto WritePageGuard::operator=(WritePageGuard &&that) noexcept -> WritePageGuard & {
    if (this != &that) {
        // 先释放当前持有的资源
        Drop();

        // 转移资源
        guard_ = std::move(that.guard_);
    }
    return *this;
}

void WritePageGuard::Drop() {
    if (guard_.page_ != nullptr) {
        // 释放写锁
        guard_.page_->WUnlatch();
        // 释放基础guard持有的资源
        guard_.Drop();
    }
}

WritePageGuard::~WritePageGuard() { Drop(); }
