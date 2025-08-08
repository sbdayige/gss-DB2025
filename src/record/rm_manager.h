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

#include "bitmap.h"
#include "rm_defs.h"
#include "rm_file_handle.h"

extern DiskManager disk_manager;
extern BufferPoolManager buffer_pool_manager;

/* 记录管理器，用于管理表的数据文件，进行文件的创建、打开、删除、关闭 */
class RmManager {
   public:
    RmManager() = default;

    /**
     * @description: 创建表的数据文件并初始化相关信息
     * @param {string&} filename 要创建的文件名称
     * @param {int} record_size 表中记录的大小
     */
    void create_file(const std::string &filename, int record_size) {
        if (record_size < 1 || record_size > RM_MAX_RECORD_SIZE) {
            throw InvalidRecordSizeError(record_size);
        }
        disk_manager.create_file(filename);
        int fd = disk_manager.open_file(filename);
        RmFileHdr file_hdr{};
        file_hdr.record_size = record_size;
        file_hdr.num_pages = 1;
        file_hdr.first_free_page_no = RM_NO_PAGE;
        file_hdr.num_records_per_page =
            (BITMAP_WIDTH * (PAGE_SIZE - 1 - (int)sizeof(RmFileHdr)) + 1) / (1 + record_size * BITMAP_WIDTH);
        file_hdr.bitmap_size = (file_hdr.num_records_per_page + BITMAP_WIDTH - 1) / BITMAP_WIDTH;
        disk_manager.write_page(fd, RM_FILE_HDR_PAGE, (char *)&file_hdr, sizeof(file_hdr));
        disk_manager.close_file(fd);
    }

    /**
     * @description: 删除表的数据文件
     * @param {string&} filename 要删除的文件名称
     */
    void destroy_file(const std::string &filename) { disk_manager.destroy_file(filename); }

    /**
     * @description: 打开表的数据文件，并返回文件句柄
     * @param {string&} filename 要打开的文件名称
     * @return {unique_ptr<RmFileHandle>} 文件句柄的指针
     */
    std::unique_ptr<RmFileHandle> open_file(const std::string &filename) {
        int fd = disk_manager.open_file(filename);
        return std::make_unique<RmFileHandle>(&disk_manager, &buffer_pool_manager, fd);
    }
    /**
     * @description: 关闭表的数据文件
     * @param {RmFileHandle*} file_handle 要关闭文件的句柄
     */
    void close_file(const RmFileHandle *file_handle) {
        disk_manager.write_page(file_handle->fd_, RM_FILE_HDR_PAGE, (char *)&file_handle->file_hdr_,
                                sizeof(file_handle->file_hdr_));
        buffer_pool_manager.flush_all_pages(file_handle->fd_);
        disk_manager.close_file(file_handle->fd_);
    }

    /**
     * @brief 关闭数据文件并清理缓冲池
     *
     * @param file_handle 要关闭文件的句柄
     */
    void close_file_and_delete_pages(const RmFileHandle *file_handle) {
        disk_manager.write_page(file_handle->fd_, RM_FILE_HDR_PAGE, (char *)&file_handle->file_hdr_, sizeof(file_handle->file_hdr_));
        buffer_pool_manager.flush_all_pages(file_handle->fd_);
        buffer_pool_manager.delete_all_pages(file_handle->fd_);
        disk_manager.close_file(file_handle->fd_);
    }

    void close_file_and_clear_buffer(const RmFileHandle *file_handle) {
        disk_manager.write_page(file_handle->fd_, RM_FILE_HDR_PAGE, (char *)&file_handle->file_hdr_,
                                sizeof(file_handle->file_hdr_));
        buffer_pool_manager.flush_all_pages(file_handle->fd_);
        buffer_pool_manager.delete_all_pages(file_handle->fd_);
        disk_manager.close_file(file_handle->fd_);
    }
};
