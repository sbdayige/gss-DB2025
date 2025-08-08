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

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>

#include "common/config.h"
#include "errors.h"

/**
 * @brief 高级磁盘管理器
 *
 * 负责管理所有磁盘I/O操作，包括页面分配、文件操作、
 * WAL日志管理等核心功能。采用原子操作保证线程安全。
 */
class DiskManager {
   public:
    explicit DiskManager();
    ~DiskManager() = default;

    // 核心页面I/O操作
    void write_page(int fd, page_id_t page_no, const char *offset, int num_bytes);
    void read_page(int fd, page_id_t page_no, char *offset, int num_bytes);

    // 页面分配管理
    page_id_t allocate_page(int fd);
    void deallocate_page(page_id_t page_id);

    // 高级文件系统操作
    bool is_dir(const std::string &path);
    void create_dir(const std::string &path);
    void destroy_dir(const std::string &path);

    // 文件操作接口
    bool is_file(const std::string &path);
    void create_file(const std::string &path);
    void destroy_file(const std::string &path);
    int open_file(const std::string &path);
    void close_file(int fd);
    int get_file_size(const std::string &file_name);
    std::string get_file_name(int fd);
    int get_file_fd(const std::string &file_name);

    // WAL日志系统操作
    int read_log(char *log_data, int size, int offset);
    void write_log(char *log_data, int size);
    void SetLogFd(int log_fd) { log_fd_ = log_fd; }
    int GetLogFd() { return log_fd_; }

    // 页面号管理（线程安全）
    void set_fd2pageno(int fd, int start_page_no) {
        fd2pageno_[fd].store(start_page_no, std::memory_order_release);
    }

    page_id_t get_fd2pageno(int fd) {
        return fd2pageno_[fd].load(std::memory_order_acquire);
    }

    // 系统限制常量
    static constexpr int MAX_FD = 8192;
    static constexpr int DISK_IO_BUFFER_SIZE = 4096;

   private:
    // 文件管理映射表
    std::unordered_map<std::string, int> path2fd_;  // 路径到文件描述符的映射
    std::unordered_map<int, std::string> fd2path_;  // 文件描述符到路径的映射

    // WAL日志文件管理
    int log_fd_ = -1;  // WAL日志文件句柄，-1表示未打开

    // 原子页面分配计数器（线程安全）
    std::atomic<page_id_t> fd2pageno_[MAX_FD]{};  // 每个文件的已分配页面数

    // 性能统计（可选）
    mutable std::atomic<size_t> total_reads_{0};
    mutable std::atomic<size_t> total_writes_{0};
};