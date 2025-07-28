/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "log_manager.h"

#include <cstring>

extern DiskManager disk_manager;

/**
 * @description: 添加日志记录到日志缓冲区中，并返回日志记录号
 * @param {LogRecord*} log_record 要写入缓冲区的日志记录
 * @return {lsn_t} 返回该日志的日志记录号
 */
lsn_t LogManager::add_log_to_buffer(LogRecord* log_record) {
    std::unique_lock<std::mutex> lock(latch_);

    // 分配LSN
    lsn_t lsn = global_lsn_.fetch_add(1);
    log_record->lsn_ = lsn;

    // 检查缓冲区是否有足够空间
    if (log_buffer_.is_full(log_record->log_tot_len_)) {
        // 缓冲区满，需要先刷盘
        flush_log_to_disk_internal();
    }

    // 将日志记录序列化到缓冲区
    log_record->serialize(log_buffer_.buffer_ + log_buffer_.offset_);
    log_buffer_.offset_ += log_record->log_tot_len_;

    return lsn;
}

/**
 * @description: 把日志缓冲区的内容刷到磁盘中，由于目前只设置了一个缓冲区，因此需要阻塞其他日志操作
 */
void LogManager::flush_log_to_disk() {
    std::unique_lock<std::mutex> lock(latch_);
    flush_log_to_disk_internal();
}

/**
 * @description: 内部刷盘方法，调用时已持有锁
 */
void LogManager::flush_log_to_disk_internal() {
    if (log_buffer_.offset_ > 0) {
        // 写入磁盘
        disk_manager.write_log(log_buffer_.buffer_, log_buffer_.offset_);

        // 更新持久化LSN
        persist_lsn_ = global_lsn_.load() - 1;

        // 清空缓冲区
        log_buffer_.offset_ = 0;
        memset(log_buffer_.buffer_, 0, sizeof(log_buffer_.buffer_));
    }
}

/**
 * @description: 创建静态检查点
 * 静态检查点的创建步骤：
 * 1. 将日志缓冲区内容写到日志文件
 * 2. 在日志文件中写入检查点记录
 * 3. 将检查点记录的地址写到重新启动文件
 */
void LogManager::create_static_checkpoint() {
    std::unique_lock<std::mutex> lock(latch_);

    // 1. 将日志缓冲区内容写到日志文件
    flush_log_to_disk_internal();

    // 2. 写入检查点记录
    CheckpointLogRecord checkpoint_record;
    lsn_t checkpoint_lsn = global_lsn_.fetch_add(1);
    checkpoint_record.lsn_ = checkpoint_lsn;

    // 将检查点记录写入缓冲区并立即刷盘
    checkpoint_record.serialize(log_buffer_.buffer_ + log_buffer_.offset_);
    log_buffer_.offset_ += checkpoint_record.log_tot_len_;
    flush_log_to_disk_internal();

    // 3. 将检查点LSN写入重启文件
    const std::string restart_file = "restart.log";

    // 检查文件是否存在，如果不存在则创建
    if (!disk_manager.is_file(restart_file)) {
        disk_manager.create_file(restart_file);
    }

    int restart_fd = disk_manager.open_file(restart_file);
    disk_manager.write_page(restart_fd, 0, (char*)&checkpoint_lsn, sizeof(lsn_t));
    disk_manager.close_file(restart_fd);
}
