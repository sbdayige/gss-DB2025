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
    std::cout << "========== RECOVERY ANALYZE PHASE ==========" << std::endl;

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
            std::cout << "Found checkpoint at LSN: " << checkpoint_lsn << std::endl;
            disk_manager_->close_file(restart_fd);
        } catch (const std::exception& e) {
            std::cout << "Failed to read checkpoint file: " << e.what() << std::endl;
            scan_start_lsn = 0;
        }
    } else {
        std::cout << "No checkpoint found, starting from beginning" << std::endl;
        scan_start_lsn = 0;
    }

    // 扫描日志文件
    std::map<txn_id_t, lsn_t> active_txns;  // 活跃事务表，记录每个事务的最后一个LSN

    // 读取日志文件
    if (!disk_manager_->is_file("db.log")) {
        std::cout << "No log file found, recovery complete" << std::endl;
        return;
    }

    // 获取日志文件总大小
    int total_file_size = disk_manager_->get_file_size("db.log");
    if (total_file_size <= 0) {
        std::cout << "Empty log file, recovery complete" << std::endl;
        return;
    }

    int logs_processed = 0;
    int file_offset = 0;

    std::cout << "Log file size: " << total_file_size << " bytes" << std::endl;
    std::cout << "Scanning log from LSN: " << scan_start_lsn << std::endl;

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

                std::cout << "Invalid log record at file offset " << (file_offset + current_offset) << std::endl;
                break;
            }

            // 只处理检查点之后的日志
            if (temp_record.lsn_ >= scan_start_lsn) {
                logs_processed++;

                std::cout << "Processing log: LSN=" << temp_record.lsn_
                          << ", type=" << LogTypeStr[temp_record.log_type_]
                          << ", txn_id=" << temp_record.log_tid_ << std::endl;

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
                        std::cout << "Found checkpoint record at LSN: " << temp_record.lsn_ << std::endl;
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

    std::cout << "Analyze phase completed:" << std::endl;
    std::cout << "- Total logs processed: " << logs_processed << std::endl;
    std::cout << "- Transactions to redo: " << redo_list_.size() << std::endl;
    std::cout << "- Transactions to undo: " << undo_list_.size() << std::endl;

    if (!redo_list_.empty()) {
        std::cout << "Redo list: ";
        for (auto tid : redo_list_) {
            std::cout << tid << " ";
        }
        std::cout << std::endl;
    }

    if (!undo_list_.empty()) {
        std::cout << "Undo list: ";
        for (auto tid : undo_list_) {
            std::cout << tid << " ";
        }
        std::cout << std::endl;
    }
}

/**
 * @description: 重做所有未落盘的操作
 */
void RecoveryManager::redo() {
    std::cout << "========== RECOVERY REDO PHASE ==========" << std::endl;

    if (redo_list_.empty()) {
        std::cout << "No transactions to redo" << std::endl;
        return;
    }

    // 获取日志文件总大小
    int total_file_size = disk_manager_->get_file_size("db.log");
    if (total_file_size <= 0) {
        std::cout << "No log data to redo" << std::endl;
        return;
    }

    int operations_redone = 0;
    int file_offset = 0;

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
                        std::cout << "Redoing INSERT: LSN=" << insert_log.lsn_
                                  << ", txn_id=" << insert_log.log_tid_ << std::endl;
                        redo_insert(&insert_log);
                        operations_redone++;
                        break;
                    }
                    case LogType::DELETE: {
                        DeleteLogRecord delete_log;
                        delete_log.deserialize(log_buffer + current_offset);
                        std::cout << "Redoing DELETE: LSN=" << delete_log.lsn_
                                  << ", txn_id=" << delete_log.log_tid_ << std::endl;
                        redo_delete(&delete_log);
                        operations_redone++;
                        break;
                    }
                    case LogType::UPDATE: {
                        UpdateLogRecord update_log;
                        update_log.deserialize(log_buffer + current_offset);
                        std::cout << "Redoing UPDATE: LSN=" << update_log.lsn_
                                  << ", txn_id=" << update_log.log_tid_ << std::endl;
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

    std::cout << "Redo phase completed. Operations redone: " << operations_redone << std::endl;
}

/**
 * @description: 回滚未完成的事务
 */
void RecoveryManager::undo() {
    std::cout << "========== RECOVERY UNDO PHASE ==========" << std::endl;

    if (undo_list_.empty()) {
        std::cout << "No transactions to undo" << std::endl;
        return;
    }

    // 收集需要回滚的操作（按LSN倒序）
    std::vector<std::pair<LogType, std::vector<char>>> undo_operations;

    // 获取日志文件总大小
    int total_file_size = disk_manager_->get_file_size("db.log");
    if (total_file_size <= 0) {
        std::cout << "No log data to undo" << std::endl;
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
                std::cout << "Undoing INSERT: LSN=" << insert_log.lsn_
                          << ", txn_id=" << insert_log.log_tid_ << std::endl;
                undo_insert(&insert_log);
                operations_undone++;
                break;
            }
            case LogType::DELETE: {
                DeleteLogRecord delete_log;
                delete_log.deserialize(operation.second.data());
                std::cout << "Undoing DELETE: LSN=" << delete_log.lsn_
                          << ", txn_id=" << delete_log.log_tid_ << std::endl;
                undo_delete(&delete_log);
                operations_undone++;
                break;
            }
            case LogType::UPDATE: {
                UpdateLogRecord update_log;
                update_log.deserialize(operation.second.data());
                std::cout << "Undoing UPDATE: LSN=" << update_log.lsn_
                          << ", txn_id=" << update_log.log_tid_ << std::endl;
                undo_update(&update_log);
                operations_undone++;
                break;
            }
            default:
                break;
        }
    }

    std::cout << "Undo phase completed. Operations undone: " << operations_undone << std::endl;
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

    std::cout << "Creating missing pages from " << file_hdr.num_pages
              << " to " << target_page_no << " for recovery" << std::endl;

    // 依次创建页面直到目标页面
    while (file_hdr.num_pages <= target_page_no) {
        auto page_handle = fh->create_new_page_handle();
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
        file_hdr = fh->get_file_hdr();  // 重新获取更新后的文件头
        std::cout << "Created page " << (file_hdr.num_pages - 1) << " for recovery" << std::endl;
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
                std::cout << "Record already exists at rid=(" << log_record->rid_.page_no
                          << "," << log_record->rid_.slot_no << "), skipping redo insert" << std::endl;
                return;
            }
        } catch (...) {
            // 页面或记录不存在，继续插入
        }

        // 按照原始RID插入
        fh->insert_record(log_record->rid_, log_record->insert_value_.data);
        std::cout << "Successfully redid insert at RID=(" << log_record->rid_.page_no
                  << "," << log_record->rid_.slot_no << ")" << std::endl;

    } catch (const std::exception& e) {
        std::cout << "Failed to redo insert: " << e.what() << std::endl;
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
                std::cout << "Record already deleted at rid=(" << log_record->rid_.page_no
                          << "," << log_record->rid_.slot_no << "), skipping redo delete" << std::endl;
                return;
            }
        } catch (...) {
            // 记录不存在，跳过删除
            std::cout << "Record does not exist for delete at rid=(" << log_record->rid_.page_no
                      << "," << log_record->rid_.slot_no << "), skipping redo delete" << std::endl;
            return;
        }

        fh->delete_record(log_record->rid_, nullptr);
        std::cout << "Successfully redid delete at RID=(" << log_record->rid_.page_no
                  << "," << log_record->rid_.slot_no << ")" << std::endl;

    } catch (const std::exception& e) {
        std::cout << "Failed to redo delete: " << e.what() << std::endl;
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
                std::cout << "CRITICAL ERROR: Record does not exist for update at rid=("
                          << log_record->rid_.page_no << "," << log_record->rid_.slot_no
                          << ") during redo. Database state inconsistent." << std::endl;
                throw std::runtime_error("RID consistency violation during redo_update");
            }
        } catch (...) {
            // 记录不存在，这是严重错误
            std::cout << "CRITICAL ERROR: Cannot access record for update at rid=("
                      << log_record->rid_.page_no << "," << log_record->rid_.slot_no
                      << ") during redo" << std::endl;
            throw std::runtime_error("RID consistency violation during redo_update");
        }

        fh->update_record(log_record->rid_, log_record->after_value_.data, nullptr);
        std::cout << "Successfully redid update at RID=(" << log_record->rid_.page_no
                  << "," << log_record->rid_.slot_no << ")" << std::endl;

    } catch (const std::exception& e) {
        std::cout << "Failed to redo update: " << e.what() << std::endl;
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
                std::cout << "Record already removed for undo insert at rid=(" << log_record->rid_.page_no
                          << "," << log_record->rid_.slot_no << ")" << std::endl;
                return;
            }
        } catch (...) {
            // 记录不存在，已经被删除
            std::cout << "Record does not exist for undo insert at rid=(" << log_record->rid_.page_no
                      << "," << log_record->rid_.slot_no << "), already removed" << std::endl;
            return;
        }

        fh->delete_record(log_record->rid_, nullptr);
        std::cout << "Successfully undid insert by deleting record at RID=("
                  << log_record->rid_.page_no << "," << log_record->rid_.slot_no << ")" << std::endl;

    } catch (const std::exception& e) {
        std::cout << "Failed to undo insert: " << e.what() << std::endl;
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
                std::cout << "Record already exists for undo delete at rid=(" << log_record->rid_.page_no
                          << "," << log_record->rid_.slot_no << ")" << std::endl;
                return;
            }
        } catch (...) {
            // 记录不存在，继续插入
        }

        // 按照原始RID重新插入
        fh->insert_record(log_record->rid_, log_record->delete_value_.data);
        std::cout << "Successfully undid delete by reinserting record at RID=("
                  << log_record->rid_.page_no << "," << log_record->rid_.slot_no << ")" << std::endl;

    } catch (const std::exception& e) {
        std::cout << "Failed to undo delete: " << e.what() << std::endl;
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
                std::cout << "CRITICAL ERROR: Record does not exist for undo update at rid=("
                          << log_record->rid_.page_no << "," << log_record->rid_.slot_no
                          << ") during undo. Database state inconsistent." << std::endl;
                throw std::runtime_error("RID consistency violation during undo_update");
            }
        } catch (...) {
            // 记录不存在，这是严重错误
            std::cout << "CRITICAL ERROR: Cannot access record for undo update at rid=("
                      << log_record->rid_.page_no << "," << log_record->rid_.slot_no
                      << ") during undo" << std::endl;
            throw std::runtime_error("RID consistency violation during undo_update");
        }

        fh->update_record(log_record->rid_, log_record->before_value_.data, nullptr);
        std::cout << "Successfully undid update by restoring old value at RID=("
                  << log_record->rid_.page_no << "," << log_record->rid_.slot_no << ")" << std::endl;

    } catch (const std::exception& e) {
        std::cout << "Failed to undo update: " << e.what() << std::endl;
        throw;  // 重新抛出异常，确保调用者知道恢复失败
    }
}