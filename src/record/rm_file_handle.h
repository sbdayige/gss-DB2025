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

#include <assert.h>

#include <map>
#include <memory>
#include <vector>

#include "bitmap.h"
#include "common/context.h"
#include "rm_defs.h"

class RmManager;

struct RmPageHandle {
    const RmFileHdr *file_hdr;  // 当前页面所在文件的文件头指针
    Page *page;                 // 页面的实际数据，包括页面存储的数据、元信息等
    RmPageHdr *page_hdr;        // page->data的第一部分，存储页面元信息，指针指向首地址，长度为sizeof(RmPageHdr)
    char *bitmap;               // page->data的第二部分，存储页面的bitmap，指针指向首地址，长度为file_hdr->bitmap_size
    char *slots;                // page->data的第三部分，存储表的记录，指针指向首地址，每个slot的长度为file_hdr->record_size

    RmPageHandle(const RmFileHdr *fhdr_, Page *page_) : file_hdr(fhdr_), page(page_) {
        page_hdr = reinterpret_cast<RmPageHdr *>(page->get_data() + page->OFFSET_PAGE_HDR);
        bitmap = page->get_data() + sizeof(RmPageHdr) + page->OFFSET_PAGE_HDR;
        slots = bitmap + file_hdr->bitmap_size;
    }

    // 按页读取优化 - 从页面数据构造
    RmPageHandle(const RmFileHdr *fhdr_, char *page_data_) : file_hdr(fhdr_), page(nullptr) {
        page_hdr = reinterpret_cast<RmPageHdr *>(page_data_ + Page::OFFSET_PAGE_HDR);
        bitmap = page_data_ + sizeof(RmPageHdr) + Page::OFFSET_PAGE_HDR;
        slots = bitmap + file_hdr->bitmap_size;
    }

    // 返回指定slot_no的slot存储收地址
    char *get_slot(int slot_no) const {
        return slots + slot_no * file_hdr->record_size;  // slots的首地址 + slot个数 * 每个slot的大小(每个record的大小)
    }

    // 按页读取优化 - 直接从页面数据获取记录
    std::unique_ptr<RmRecord> get_record_from_page(int slot_no) const {
        char *record_data = get_slot(slot_no);
        return std::make_unique<RmRecord>(file_hdr->record_size, record_data);
    }

    // 按页读取优化 - 检查slot是否被使用
    bool is_slot_used(int slot_no) const {
        return Bitmap::is_set(bitmap, slot_no);
    }
};

class RmFileHandle {
    friend class RmScan;
    friend class RmManager;
    friend class SmManager;

   public:
    DiskManager *disk_manager_;
    BufferPoolManager *buffer_pool_manager_;
    int fd_;              // 打开文件后产生的文件句柄
    RmFileHdr file_hdr_;  // 文件头，维护当前表文件的元数据

   public:
    RmFileHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
        : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
        disk_manager_->read_page(fd, RM_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));
        disk_manager_->set_fd2pageno(fd, file_hdr_.num_pages);
    }

    RmFileHdr get_file_hdr() { return file_hdr_; }
    int GetFd() { return fd_; }

    /* 判断指定位置上是否已经存在一条记录，通过Bitmap来判断 */
    bool is_record(const Rid &rid) const {
        RmPageHandle page_handle = fetch_page_handle(rid.page_no);
        return Bitmap::is_set(page_handle.bitmap, rid.slot_no);  // page的slot_no位置上是否有record
    }

    std::unique_ptr<RmRecord> get_record(const Rid &rid, Context *context) const;

    // 按页读取优化 - 获取页面读锁
    Page *AcquirePageReadLock(const Rid &rid) const;

    // 按页读取优化 - 获取页面写锁
    Page *AcquirePageWriteLock(const Rid &rid) const;

    // 按页读取优化 - 从已锁定的页面获取记录
    std::unique_ptr<RmRecord> GetRecordWithLockAcquired(const Rid &rid, const char *page_data) const;

    // 按页读取优化 - 批量获取页面中的多个记录
    std::vector<std::unique_ptr<RmRecord>> GetRecordsBatch(const std::vector<Rid> &rids, Context *context) const;

    Rid insert_record(char *buf, Context *context);

    void insert_record(const Rid &rid, char *buf);

    void insert_record_force(const Rid &rid, char *buf);

    void delete_record(const Rid &rid, Context *context);

    void update_record(const Rid &rid, char *buf, Context *context);

    RmPageHandle create_new_page_handle();

    RmPageHandle fetch_page_handle(int page_no) const;

   private:
    RmPageHandle create_page_handle();

    void release_page_handle(RmPageHandle &page_handle);
};
