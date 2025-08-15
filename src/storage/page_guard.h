/**
 * @brief 智能页面守护系统
 *
 * 提供RAII风格的页面管理，自动处理页面的生命周期，
 * 支持读写分离和移动语义优化
 */

#pragma once

#include "storage/page.h"

class BufferPoolManager;
class ReadPageGuard;
class WritePageGuard;

/**
 * @brief 基础页面守护类
 *
 * 提供页面的基本生命周期管理和数据访问功能
 */
class BasicPageGuard {
   public:
    BasicPageGuard() = default;
    BasicPageGuard(BufferPoolManager *bpm, Page *page) : bpm_(bpm), page_(page) {}

    // 禁用拷贝构造和拷贝赋值
    BasicPageGuard(const BasicPageGuard &) = delete;
    auto operator=(const BasicPageGuard &) -> BasicPageGuard & = delete;

    // 支持移动语义
    BasicPageGuard(BasicPageGuard &&that) noexcept;
    auto operator=(BasicPageGuard &&that) noexcept -> BasicPageGuard &;

    // 生命周期管理
    void Drop();
    ~BasicPageGuard();

    // 页面信息访问
    auto PageId() -> PageId { return page_->get_page_id(); }
    auto GetData() -> const char * { return page_->get_data(); }
    auto GetDataMut() -> char * {
        is_dirty_ = true;
        return page_->get_data();
    }

    // 类型转换模板
    template <class T>
    auto As() -> const T * {
        return reinterpret_cast<const T *>(GetData());
    }

    template <class T>
    auto AsMut() -> T * {
        return reinterpret_cast<T *>(GetDataMut());
    }

    // 状态查询
    bool IsValid() const { return page_ != nullptr; }
    bool IsDirty() const { return is_dirty_; }

   private:
    friend class ReadPageGuard;
    friend class WritePageGuard;
    friend class BufferPoolManager;

    [[maybe_unused]] BufferPoolManager *bpm_{nullptr};
    Page *page_{nullptr};
    bool is_dirty_{false};
};

class ReadPageGuard {
   public:
    ReadPageGuard() = default;
    ReadPageGuard(BufferPoolManager *bpm, Page *page) : guard_(bpm, page) {}

    // 禁用拷贝构造和拷贝赋值
    ReadPageGuard(const ReadPageGuard &) = delete;
    auto operator=(const ReadPageGuard &) -> ReadPageGuard & = delete;

    // 支持移动语义
    ReadPageGuard(ReadPageGuard &&that) noexcept;
    auto operator=(ReadPageGuard &&that) noexcept -> ReadPageGuard &;

    // 生命周期管理
    void Drop();
    ~ReadPageGuard();

    // 只读访问接口
    auto PageId() -> PageId { return guard_.PageId(); }
    auto GetData() -> const char * { return guard_.GetData(); }

    template <class T>
    auto As() -> const T * {
        return guard_.As<T>();
    }

    // 状态查询
    bool IsValid() const { return guard_.IsValid(); }

   private:
    BasicPageGuard guard_;
};

/**
 * @brief 可写页面守护类
 *
 * 提供读写访问权限的页面守护，支持数据修改
 */
class WritePageGuard {
   public:
    WritePageGuard() = default;
    WritePageGuard(BufferPoolManager *bpm, Page *page) : guard_(bpm, page) {}

    // 禁用拷贝构造和拷贝赋值
    WritePageGuard(const WritePageGuard &) = delete;
    auto operator=(const WritePageGuard &) -> WritePageGuard & = delete;

    // 支持移动语义
    WritePageGuard(WritePageGuard &&that) noexcept;
    auto operator=(WritePageGuard &&that) noexcept -> WritePageGuard &;

    // 生命周期管理
    void Drop();
    ~WritePageGuard();

    // 读写访问接口
    auto PageId() -> PageId { return guard_.PageId(); }
    auto GetData() -> const char * { return guard_.GetData(); }
    auto GetDataMut() -> char * { return guard_.GetDataMut(); }

    template <class T>
    auto As() -> const T * {
        return guard_.As<T>();
    }

    template <class T>
    auto AsMut() -> T * {
        return guard_.AsMut<T>();
    }

    // 状态查询
    bool IsValid() const { return guard_.IsValid(); }
    bool IsDirty() const { return guard_.IsDirty(); }

    // 升级接口（从ReadPageGuard升级而来）
    auto UpgradeWrite() -> WritePageGuard;

   private:
    BasicPageGuard guard_;
};
