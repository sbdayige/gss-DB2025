#pragma once

#include "storage/page.h"

class BufferPoolInstance;
class ReadPageGuard;
class WritePageGuard;

class BasicPageGuard {
   public:
    BasicPageGuard() = default;

    BasicPageGuard(BufferPoolInstance *bpi, Page *page) : bpi_(bpi), page_(page) {}

    BasicPageGuard(const BasicPageGuard &) = delete;
    auto operator=(const BasicPageGuard &) -> BasicPageGuard & = delete;

    BasicPageGuard(BasicPageGuard &&that) noexcept;

    void Drop();

    auto operator=(BasicPageGuard &&that) noexcept -> BasicPageGuard &;

    ~BasicPageGuard();

    auto PageId() -> PageId { return page_->get_page_id(); }

    auto GetData() -> const char * { return page_->get_data(); }

    template <class T>
    auto As() -> const T * {
        return reinterpret_cast<const T *>(GetData());
    }

    auto GetDataMut() -> char * {
        is_dirty_ = true;
        return page_->get_data();
    }

    template <class T>
    auto AsMut() -> T * {
        return reinterpret_cast<T *>(GetDataMut());
    }

   private:
    friend class ReadPageGuard;
    friend class WritePageGuard;
    friend class BufferPoolInstance;

    [[maybe_unused]] BufferPoolInstance *bpi_{nullptr};
    Page *page_{nullptr};
    bool is_dirty_{false};
};

class ReadPageGuard {
   public:
    ReadPageGuard() = default;

    ReadPageGuard(BufferPoolInstance *bpi, Page *page) : guard_(bpi, page) {}

    ReadPageGuard(const ReadPageGuard &) = delete;

    auto operator=(const ReadPageGuard &) -> ReadPageGuard & = delete;

    ReadPageGuard(ReadPageGuard &&that) noexcept;

    auto operator=(ReadPageGuard &&that) noexcept -> ReadPageGuard &;

    void Drop();

    /** TODO(P1): Add implementation
     *
     * @brief Destructor for ReadPageGuard
     *
     * Just like with BasicPageGuard, this should behave
     * as if you were dropping the guard.
     */
    ~ReadPageGuard();

    auto PageId() -> PageId { return guard_.PageId(); }

    auto GetData() -> const char * { return guard_.GetData(); }

    template <class T>
    auto As() -> const T * {
        return guard_.As<T>();
    }

   private:
    BasicPageGuard guard_;
};

class WritePageGuard {
   public:
    WritePageGuard() = default;
    WritePageGuard(BufferPoolInstance *bpi, Page *page) : guard_(bpi, page) {}
    WritePageGuard(const WritePageGuard &) = delete;
    auto operator=(const WritePageGuard &) -> WritePageGuard & = delete;

    WritePageGuard(WritePageGuard &&that) noexcept;

    auto operator=(WritePageGuard &&that) noexcept -> WritePageGuard &;

    void Drop();

    ~WritePageGuard();

    auto PageId() -> PageId { return guard_.PageId(); }

    auto GetData() -> const char * { return guard_.GetData(); }

    template <class T>
    auto As() -> const T * {
        return guard_.As<T>();
    }

    auto GetDataMut() -> char * { return guard_.GetDataMut(); }

    template <class T>
    auto AsMut() -> T * {
        return guard_.AsMut<T>();
    }

   private:
    BasicPageGuard guard_;
};
