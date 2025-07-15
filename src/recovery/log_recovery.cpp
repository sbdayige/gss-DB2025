/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "log_recovery.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <vector>

/**
 * @description: analyze阶段，需要获得脏页表（DPT）和未完成的事务列表（ATT）
 */
void RecoveryManager::analyze() {
    // 初始化
    undo_list_.clear();
    redo_list_.clear();

    // 查找最后一个检查点
    lsn_t checkpoint_lsn = INVALID_LSN;
    lsn_t scan_start_lsn = 0;

    // 尝试从restart.log文件读取检查点信息
    std::string restart_file = "restart.log";
    if (disk_manager_->is_file(restart_file)) {
        try {
            int restart_fd = disk_manager_->open_file(restart_file);
            char checkpoint_data[sizeof(lsn_t)];
            disk_manager_->read_page(restart_fd, 0, checkpoint_data, sizeof(lsn_t));
            checkpoint_lsn = *reinterpret_cast<lsn_t*>(checkpoint_data);
            scan_start_lsn = checkpoint_lsn;
            disk_manager_->close_file(restart_fd);
        } catch (const std::exception& e) {
            scan_start_lsn = 0;
        }
    } else {
        scan_start_lsn = 0;
    }

    // 扫描日志文件
    std::map<txn_id_t, lsn_t> active_txns;  // 活跃事务表，记录每个事务的最后一个LSN

    // 读取日志文件
    if (!disk_manager_->is_file("db.log")) {
        return;
    }

    // 获取日志文件总大小
    int total_file_size = disk_manager_->get_file_size("db.log");
    if (total_file_size <= 0) {
        return;
    }

    int logs_processed = 0;
    int file_offset = 0;

    // 分批读取日志文件
    while (file_offset < total_file_size) {
        // 计算本次读取的数据量
        int bytes_to_read = std::min(LOG_BUFFER_SIZE, total_file_size - file_offset);

        char log_buffer[LOG_BUFFER_SIZE];
        int bytes_read = disk_manager_->read_log(log_buffer, bytes_to_read, file_offset);

        if (bytes_read <= 0) {
            break;
        }

        int current_offset = 0;

        // 扫描当前批次的日志记录
        while (current_offset < bytes_read) {
            if (current_offset + LOG_HEADER_SIZE > bytes_read) {
                // 日志记录跨越批次边界，需要特殊处理
                if (file_offset + bytes_read < total_file_size) {
                    // 还有更多数据，将未完成的记录留到下一批次处理
                    file_offset += current_offset;
                    break;
                }
                // 已经是最后一批数据，不够读取完整日志头
                break;
            }

            // 创建临时日志记录来读取头部信息
            LogRecord temp_record;
            temp_record.deserialize(log_buffer + current_offset);

            // 检查日志记录的有效性
            if (temp_record.log_tot_len_ == 0 ||
                temp_record.log_tot_len_ > LOG_BUFFER_SIZE ||
                current_offset + temp_record.log_tot_len_ > bytes_read) {
                // 日志记录跨越批次边界，需要特殊处理
                if (file_offset + bytes_read < total_file_size &&
                    temp_record.log_tot_len_ > 0 &&
                    temp_record.log_tot_len_ <= LOG_BUFFER_SIZE) {
                    // 将未完成的记录留到下一批次处理
                    file_offset += current_offset;
                    break;
                }

                break;
            }

            // 只处理检查点之后的日志
            if (temp_record.lsn_ >= scan_start_lsn) {
                logs_processed++;

                // 根据日志类型更新事务状态
                switch (temp_record.log_type_) {
                    case LogType::begin:
                        // 事务开始
                        active_txns[temp_record.log_tid_] = temp_record.lsn_;
                        break;

                    case LogType::commit:
                        // 事务提交
                        if (active_txns.find(temp_record.log_tid_) != active_txns.end()) {
                            redo_list_.insert(temp_record.log_tid_);
                            active_txns.erase(temp_record.log_tid_);
                        }
                        break;

                    case LogType::ABORT:
                        // 事务回滚
                        if (active_txns.find(temp_record.log_tid_) != active_txns.end()) {
                            undo_list_.insert(temp_record.log_tid_);
                            active_txns.erase(temp_record.log_tid_);
                        }
                        break;

                    case LogType::INSERT:
                    case LogType::DELETE:
                    case LogType::UPDATE:
                        // 数据操作，记录活跃事务
                        active_txns[temp_record.log_tid_] = temp_record.lsn_;
                        break;

                    case LogType::CHECKPOINT:
                        // 检查点记录
                        break;

                    default:
                        break;
                }
            }

            current_offset += temp_record.log_tot_len_;
        }

        // 如果处理完了整个批次，继续下一批次
        if (current_offset >= bytes_read) {
            file_offset += bytes_read;
        }
        // 否则 file_offset 已经在循环中更新了
    }

    // 分析结束，所有仍在active_txns中的事务都是未完成的，需要回滚
    for (const auto& txn : active_txns) {
        undo_list_.insert(txn.first);
    }
}

/**
 * @description: 重做所有未落盘的操作
 */
void RecoveryManager::redo() {
    if (redo_list_.empty()) {
        return;
    }

    // 获取日志文件总大小
    int total_file_size = disk_manager_->get_file_size("db.log");
    if (total_file_size <= 0) {
        return;
    }

    int operations_redone = 0;
    int file_offset = 0;
    std::cout << "Log file size: " << total_file_size << " bytes" << std::endl;
    // 分批读取日志文件进行重做
    while (file_offset < total_file_size) {
        int bytes_to_read = std::min(LOG_BUFFER_SIZE, total_file_size - file_offset);

        char log_buffer[LOG_BUFFER_SIZE];
        int bytes_read = disk_manager_->read_log(log_buffer, bytes_to_read, file_offset);

        if (bytes_read <= 0) {
            break;
        }

        int current_offset = 0;

        // 扫描当前批次的日志记录
        while (current_offset < bytes_read) {
            if (current_offset + LOG_HEADER_SIZE > bytes_read) {
                if (file_offset + bytes_read < total_file_size) {
                    file_offset += current_offset;
                    break;
                }
                break;
            }

            LogRecord temp_record;
            temp_record.deserialize(log_buffer + current_offset);

            if (temp_record.log_tot_len_ == 0 ||
                temp_record.log_tot_len_ > LOG_BUFFER_SIZE ||
                current_offset + temp_record.log_tot_len_ > bytes_read) {
                if (file_offset + bytes_read < total_file_size &&
                    temp_record.log_tot_len_ > 0 &&
                    temp_record.log_tot_len_ <= LOG_BUFFER_SIZE) {
                    file_offset += current_offset;
                    break;
                }
                break;
            }

            // 只重做已提交事务的数据操作
            if (redo_list_.find(temp_record.log_tid_) != redo_list_.end()) {
                switch (temp_record.log_type_) {
                    case LogType::INSERT: {
                        InsertLogRecord insert_log;
                        insert_log.deserialize(log_buffer + current_offset);
                        std::string table_name = insert_log.table_name_;
                        if (sm_manager_->fhs_.find(table_name) == sm_manager_->fhs_.end()) {
                            std::cout << "Table " << table_name << " not found, skipping redo" << std::endl;
                            return;
                        }
                        redo_insert(&insert_log);
                        operations_redone++;
                        break;
                    }
                    case LogType::DELETE: {
                        DeleteLogRecord delete_log;
                        delete_log.deserialize(log_buffer + current_offset);
                        std::string table_name = delete_log.table_name_;
                        if (sm_manager_->fhs_.find(table_name) == sm_manager_->fhs_.end()) {
                            std::cout << "Table " << table_name << " not found, skipping redo" << std::endl;
                            return;
                        }
                        redo_delete(&delete_log);
                        operations_redone++;
                        break;
                    }
                    case LogType::UPDATE: {
                        UpdateLogRecord update_log;
                        update_log.deserialize(log_buffer + current_offset);
                        std::string table_name = update_log.table_name_;
                        if (sm_manager_->fhs_.find(table_name) == sm_manager_->fhs_.end()) {
                            std::cout << "Table " << table_name << " not found, skipping redo" << std::endl;
                            return;
                        }
                        redo_update(&update_log);
                        operations_redone++;
                        break;
                    }
                    default:
                        // 非数据操作，跳过
                        break;
                }
            }

            current_offset += temp_record.log_tot_len_;
        }

        if (current_offset >= bytes_read) {
            file_offset += bytes_read;
        }
    }
}

/**
 * @description: 回滚未完成的事务
 */
void RecoveryManager::undo() {
    if (undo_list_.empty()) {
        return;
    }
    // 收集需要回滚的操作（按LSN倒序）
    std::vector<std::pair<LogType, std::vector<char>>> undo_operations;

    // 获取日志文件总大小
    int total_file_size = disk_manager_->get_file_size("db.log");
    if (total_file_size <= 0) {
        return;
    }
    int file_offset = 0;

    // 分批读取日志文件收集回滚操作
    while (file_offset < total_file_size) {
        int bytes_to_read = std::min(LOG_BUFFER_SIZE, total_file_size - file_offset);

        char log_buffer[LOG_BUFFER_SIZE];
        int bytes_read = disk_manager_->read_log(log_buffer, bytes_to_read, file_offset);

        if (bytes_read <= 0) {
            break;
        }

        int current_offset = 0;

        // 收集当前批次的未完成事务的所有数据操作
        while (current_offset < bytes_read) {
            if (current_offset + LOG_HEADER_SIZE > bytes_read) {
                if (file_offset + bytes_read < total_file_size) {
                    file_offset += current_offset;
                    break;
                }
                break;
            }

            LogRecord temp_record;
            temp_record.deserialize(log_buffer + current_offset);

            if (temp_record.log_tot_len_ == 0 ||
                temp_record.log_tot_len_ > LOG_BUFFER_SIZE ||
                current_offset + temp_record.log_tot_len_ > bytes_read) {
                if (file_offset + bytes_read < total_file_size &&
                    temp_record.log_tot_len_ > 0 &&
                    temp_record.log_tot_len_ <= LOG_BUFFER_SIZE) {
                    file_offset += current_offset;
                    break;
                }
                break;
            }

            // 收集需要回滚的事务的数据操作
            if (undo_list_.find(temp_record.log_tid_) != undo_list_.end()) {
                if (temp_record.log_type_ == LogType::INSERT ||
                    temp_record.log_type_ == LogType::DELETE ||
                    temp_record.log_type_ == LogType::UPDATE) {
                    // 保存整个日志记录
                    std::vector<char> log_copy(temp_record.log_tot_len_);
                    memcpy(log_copy.data(), log_buffer + current_offset, temp_record.log_tot_len_);
                    undo_operations.push_back({temp_record.log_type_, std::move(log_copy)});
                }
            }

            current_offset += temp_record.log_tot_len_;
        }

        if (current_offset >= bytes_read) {
            file_offset += bytes_read;
        }
    }
    // 按LSN倒序执行回滚（ARIES要求）
    std::reverse(undo_operations.begin(), undo_operations.end());

    int operations_undone = 0;

    for (auto& operation : undo_operations) {
        switch (operation.first) {
            case LogType::INSERT: {
                InsertLogRecord insert_log;
                insert_log.deserialize(operation.second.data());
                std::string table_name = insert_log.table_name_;
                if (sm_manager_->fhs_.find(table_name) == sm_manager_->fhs_.end()) {
                    std::cout << "Table " << table_name << " not found, skipping undo" << std::endl;
                    return;
                }
                undo_insert(&insert_log);
                operations_undone++;
                break;
            }
            case LogType::DELETE: {
                DeleteLogRecord delete_log;
                delete_log.deserialize(operation.second.data());
                std::string table_name = delete_log.table_name_;
                if (sm_manager_->fhs_.find(table_name) == sm_manager_->fhs_.end()) {
                    std::cout << "Table " << table_name << " not found, skipping undo" << std::endl;
                    return;
                }
                undo_delete(&delete_log);
                operations_undone++;
                break;
            }
            case LogType::UPDATE: {
                UpdateLogRecord update_log;
                update_log.deserialize(operation.second.data());
                std::string table_name = update_log.table_name_;
                if (sm_manager_->fhs_.find(table_name) == sm_manager_->fhs_.end()) {
                    std::cout << "Table " << table_name << " not found, skipping undo" << std::endl;
                    return;
                }
                undo_update(&update_log);
                operations_undone++;
                break;
            }
            default:
                break;
        }
    }
}

/**
 * @description: 确保指定页面存在，如果不存在则创建页面
 */
void RecoveryManager::ensure_page_exists(RmFileHandle* fh, int target_page_no) {
    RmFileHdr file_hdr = fh->get_file_hdr();

    // 如果目标页面已存在，直接返回
    if (target_page_no < file_hdr.num_pages) {
        return;
    }
    // 依次创建页面直到目标页面
    while (file_hdr.num_pages <= target_page_no) {
        auto page_handle = fh->create_new_page_handle();
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
        file_hdr = fh->get_file_hdr();  // 重新获取更新后的文件头
    }
}

/**
 * @description: 重做插入操作
 */
void RecoveryManager::redo_insert(InsertLogRecord* log_record) {
    try {
        std::string table_name(log_record->table_name_, log_record->table_name_size_);
        auto fh = sm_manager_->get_file_handle(table_name);

        // 【新增】确保目标页面存在
        ensure_page_exists(fh, log_record->rid_.page_no);

        // 检查记录是否已存在
        try {
            bool exists = fh->is_record(log_record->rid_);
            if (exists) {
                return;
            }
        } catch (...) {
            // 页面或记录不存在，继续插入
        }

        // 按照原始RID插入
        fh->insert_record(log_record->rid_, log_record->insert_value_.data);
    } catch (const std::exception& e) {
        throw;  // 重新抛出异常，确保调用者知道恢复失败
    }
}

/**
 * @description: 重做删除操作
 */
void RecoveryManager::redo_delete(DeleteLogRecord* log_record) {
    try {
        std::string table_name(log_record->table_name_, log_record->table_name_size_);
        auto fh = sm_manager_->get_file_handle(table_name);

        // 检查记录是否存在
        try {
            bool exists = fh->is_record(log_record->rid_);
            if (!exists) {
                return;
            }
        } catch (...) {
            // 记录不存在，跳过删除
            return;
        }

        fh->delete_record(log_record->rid_, nullptr);

    } catch (const std::exception& e) {
        throw;  // 重新抛出异常，确保调用者知道恢复失败
    }
}

/**
 * @description: 重做更新操作
 */
void RecoveryManager::redo_update(UpdateLogRecord* log_record) {
    try {
        std::string table_name(log_record->table_name_, log_record->table_name_size_);
        auto fh = sm_manager_->get_file_handle(table_name);

        // 检查记录是否存在
        try {
            bool exists = fh->is_record(log_record->rid_);
            if (!exists) {
                throw std::runtime_error("RID consistency violation during redo_update");
            }
        } catch (...) {
            // 记录不存在，这是严重错误
            throw std::runtime_error("RID consistency violation during redo_update");
        }

        fh->update_record(log_record->rid_, log_record->after_value_.data, nullptr);
    } catch (const std::exception& e) {
        throw;  // 重新抛出异常，确保调用者知道恢复失败
    }
}

/**
 * @description: 撤销插入操作（即删除记录）
 */
void RecoveryManager::undo_insert(InsertLogRecord* log_record) {
    try {
        std::string table_name(log_record->table_name_, log_record->table_name_size_);
        auto fh = sm_manager_->get_file_handle(table_name);

        // 检查记录是否存在
        try {
            bool exists = fh->is_record(log_record->rid_);
            if (!exists) {
                return;
            }
        } catch (...) {
            // 记录不存在，已经被删除
            return;
        }

        fh->delete_record(log_record->rid_, nullptr);
    } catch (const std::exception& e) {
        throw;  // 重新抛出异常，确保调用者知道恢复失败
    }
}

/**
 * @description: 撤销删除操作（即重新插入记录）
 */
void RecoveryManager::undo_delete(DeleteLogRecord* log_record) {
    try {
        std::string table_name(log_record->table_name_, log_record->table_name_size_);
        auto fh = sm_manager_->get_file_handle(table_name);

        // 【新增】确保目标页面存在
        ensure_page_exists(fh, log_record->rid_.page_no);

        // 检查记录是否已存在
        try {
            bool exists = fh->is_record(log_record->rid_);
            if (exists) {
                return;
            }
        } catch (...) {
            // 记录不存在，继续插入
        }

        // 按照原始RID重新插入
        fh->insert_record(log_record->rid_, log_record->delete_value_.data);
    } catch (const std::exception& e) {
        throw;  // 重新抛出异常，确保调用者知道恢复失败
    }
}

/**
 * @description: 撤销更新操作（即恢复到旧值）
 */
void RecoveryManager::undo_update(UpdateLogRecord* log_record) {
    try {
        std::string table_name(log_record->table_name_, log_record->table_name_size_);
        auto fh = sm_manager_->get_file_handle(table_name);

        // 检查记录是否存在
        try {
            bool exists = fh->is_record(log_record->rid_);
            if (!exists) {
                throw std::runtime_error("RID consistency violation during undo_update");
            }
        } catch (...) {
            // 记录不存在，这是严重错误
            throw std::runtime_error("RID consistency violation during undo_update");
        }

        fh->update_record(log_record->rid_, log_record->before_value_.data, nullptr);
    } catch (const std::exception& e) {
        throw;  // 重新抛出异常，确保调用者知道恢复失败
    }
}