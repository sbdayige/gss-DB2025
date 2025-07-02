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
#include <set>
#include <map>
#include <vector>
#include <algorithm>
#include <cstring>

/**
 * @description: analyze阶段，需要获得脏页表（DPT）和未完成的事务列表（ATT）
 */
void RecoveryManager::analyze() {
    std::cout << "Starting Recovery Analyze Phase..." << std::endl;
    
    // 清空状态
    undo_list_.clear();
    redo_list_.clear();
    
    // 确定扫描起始位置
    lsn_t start_lsn = 0;
    std::string restart_file = "restart.log";
    
    // 尝试读取最后一个检查点
    if (disk_manager_->is_file(restart_file)) {
        int restart_fd = disk_manager_->open_file(restart_file);
        char checkpoint_data[sizeof(lsn_t)];
        try {
            disk_manager_->read_page(restart_fd, 0, checkpoint_data, sizeof(lsn_t));
            start_lsn = *reinterpret_cast<lsn_t*>(checkpoint_data);
            std::cout << "Found checkpoint at LSN: " << start_lsn << std::endl;
        } catch (...) {
            start_lsn = 0; // 检查点文件损坏，从头开始
        }
        disk_manager_->close_file(restart_fd);
    }
    
    // 扫描日志
    char log_data[LOG_BUFFER_SIZE];
    int offset = 0; // 从头开始扫描
    int bytes_read = disk_manager_->read_log(log_data, LOG_BUFFER_SIZE, offset);
    
    int current_offset = 0;
    while (current_offset < bytes_read) {
        if (current_offset + LOG_HEADER_SIZE > bytes_read) break; // 防止读取越界
        
        // 首先读取日志头部
        LogRecord temp_record;
        temp_record.deserialize(log_data + current_offset);
        
        if (temp_record.log_tot_len_ == 0 || temp_record.log_tot_len_ > LOG_BUFFER_SIZE) break; // 无效记录
        if (current_offset + temp_record.log_tot_len_ > bytes_read) break; // 防止读取越界
        
        switch (temp_record.log_type_) {
            case LogType::begin:
                undo_list_.insert(temp_record.log_tid_);
                break;
                
            case LogType::commit:
                if (undo_list_.count(temp_record.log_tid_)) {
                    undo_list_.erase(temp_record.log_tid_);
                    redo_list_.insert(temp_record.log_tid_);
                }
                break;
                
            case LogType::ABORT:
                undo_list_.erase(temp_record.log_tid_);
                break;
                
            case LogType::CHECKPOINT:
                // 检查点记录，继续扫描
                break;
                
            default:
                // INSERT/DELETE/UPDATE记录，不影响事务状态
                break;
        }
        
        current_offset += temp_record.log_tot_len_;
    }
    
    std::cout << "Analyze phase completed. Undo list size: " << undo_list_.size() 
              << ", Redo list size: " << redo_list_.size() << std::endl;
}

/**
 * @description: 重做所有未落盘的操作
 */
void RecoveryManager::redo() {
    std::cout << "Starting Recovery Redo Phase..." << std::endl;
    
    if (redo_list_.empty()) {
        std::cout << "No transactions to redo." << std::endl;
        return;
    }
    
    // 重新扫描日志，重做已提交事务的操作
    char log_data[LOG_BUFFER_SIZE];
    int bytes_read = disk_manager_->read_log(log_data, LOG_BUFFER_SIZE, 0);
    
    int current_offset = 0;
    while (current_offset < bytes_read) {
        if (current_offset + LOG_HEADER_SIZE > bytes_read) break;
        
        LogRecord temp_record;
        temp_record.deserialize(log_data + current_offset);
        
        if (temp_record.log_tot_len_ == 0 || temp_record.log_tot_len_ > LOG_BUFFER_SIZE) break;
        if (current_offset + temp_record.log_tot_len_ > bytes_read) break;
        
        // 只重做已提交事务的操作
        if (redo_list_.count(temp_record.log_tid_)) {
            std::cout << "Redoing operation: type=" << LogTypeStr[temp_record.log_type_] << ", txn_id=" << temp_record.log_tid_ << std::endl;
            switch (temp_record.log_type_) {
                case LogType::INSERT: {
                    InsertLogRecord insert_log;
                    insert_log.deserialize(log_data + current_offset);
                    redo_insert(&insert_log);
                    break;
                }
                case LogType::DELETE: {
                    DeleteLogRecord delete_log;
                    delete_log.deserialize(log_data + current_offset);
                    redo_delete(&delete_log);
                    break;
                }
                case LogType::UPDATE: {
                    UpdateLogRecord update_log;
                    update_log.deserialize(log_data + current_offset);
                    redo_update(&update_log);
                    break;
                }
                default:
                    std::cout << "Skipping redo for log type: " << LogTypeStr[temp_record.log_type_] << std::endl;
                    break;
            }
        }
        
        current_offset += temp_record.log_tot_len_;
    }
    
    std::cout << "Redo phase completed." << std::endl;
}

/**
 * @description: 回滚未完成的事务
 */
void RecoveryManager::undo() {
    std::cout << "Starting Recovery Undo Phase..." << std::endl;
    
    if (undo_list_.empty()) {
        std::cout << "No transactions to undo." << std::endl;
        return;
    }
    
    // 收集需要回滚的操作（按LSN倒序）
    std::vector<std::pair<LogType, std::vector<char>>> undo_operations;
    
    char log_data[LOG_BUFFER_SIZE];
    int bytes_read = disk_manager_->read_log(log_data, LOG_BUFFER_SIZE, 0);
    
    int current_offset = 0;
    while (current_offset < bytes_read) {
        if (current_offset + LOG_HEADER_SIZE > bytes_read) break;
        
        LogRecord temp_record;
        temp_record.deserialize(log_data + current_offset);
        
        if (temp_record.log_tot_len_ == 0 || temp_record.log_tot_len_ > LOG_BUFFER_SIZE) break;
        if (current_offset + temp_record.log_tot_len_ > bytes_read) break;
        
        // 收集未完成事务的操作
        if (undo_list_.count(temp_record.log_tid_)) {
            if (temp_record.log_type_ == LogType::INSERT || 
                temp_record.log_type_ == LogType::DELETE || 
                temp_record.log_type_ == LogType::UPDATE) {
                // 保存整个日志记录的拷贝
                std::vector<char> log_copy(temp_record.log_tot_len_);
                memcpy(log_copy.data(), log_data + current_offset, temp_record.log_tot_len_);
                undo_operations.push_back({temp_record.log_type_, std::move(log_copy)});
            }
        }
        
        current_offset += temp_record.log_tot_len_;
    }
    
    // 按LSN倒序执行回滚
    std::reverse(undo_operations.begin(), undo_operations.end());
    
    for (auto& operation : undo_operations) {
        switch (operation.first) {
            case LogType::INSERT: {
                InsertLogRecord insert_log;
                insert_log.deserialize(operation.second.data());
                undo_insert(&insert_log);
                break;
            }
            case LogType::DELETE: {
                DeleteLogRecord delete_log;
                delete_log.deserialize(operation.second.data());
                undo_delete(&delete_log);
                break;
            }
            case LogType::UPDATE: {
                UpdateLogRecord update_log;
                update_log.deserialize(operation.second.data());
                undo_update(&update_log);
                break;
            }
            default:
                break;
        }
    }
    
    std::cout << "Undo phase completed." << std::endl;
}

/**
 * @description: 重做插入操作
 */
void RecoveryManager::redo_insert(InsertLogRecord* log_record) {
    std::string table_name(log_record->table_name_, log_record->table_name_size_);
    std::cout << "Redo insert: table=" << table_name << ", rid=(" << log_record->rid_.page_no << "," << log_record->rid_.slot_no << ")" << std::endl;
    auto fh = sm_manager_->get_file_handle(table_name);
    
    try {
        // 先检查页面是否存在，如果不存在则说明需要重新插入
        bool record_exists = false;
        try {
            record_exists = fh->is_record(log_record->rid_);
        } catch (const std::exception& e) {
            // 页面不存在或其他错误，说明记录不存在
            record_exists = false;
        }
        
        if (record_exists) {
            std::cout << "Record already exists at rid=(" << log_record->rid_.page_no << "," << log_record->rid_.slot_no << "), skipping redo insert" << std::endl;
            return;
        }
        
        // 记录不存在，尝试使用原始位置插入
        try {
            fh->insert_record(log_record->rid_, log_record->insert_value_.data);
            std::cout << "Successfully redid insert for table=" << table_name << ", rid=(" << log_record->rid_.page_no << "," << log_record->rid_.slot_no << ")" << std::endl;
        } catch (const std::exception& e) {
            // 如果指定位置插入失败，使用普通插入方法（让系统自动分配RID）
            std::cout << "Failed to insert at specific RID, trying auto-allocation: " << e.what() << std::endl;
            Context dummy_context(nullptr, nullptr, nullptr);
            Rid new_rid = fh->insert_record(log_record->insert_value_.data, &dummy_context);
            std::cout << "Successfully redid insert with auto-allocated rid=(" << new_rid.page_no << "," << new_rid.slot_no << ")" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "Failed to redo insert completely: " << e.what() << std::endl;
    }
}

/**
 * @description: 重做删除操作
 */
void RecoveryManager::redo_delete(DeleteLogRecord* log_record) {
    try {
        std::string table_name(log_record->table_name_, log_record->table_name_size_);
        auto fh = sm_manager_->get_file_handle(table_name);
        fh->delete_record(log_record->rid_, nullptr);
    } catch (...) {
        // 记录可能已被删除，忽略错误
    }
}

/**
 * @description: 重做更新操作
 */
void RecoveryManager::redo_update(UpdateLogRecord* log_record) {
    try {
        std::string table_name(log_record->table_name_, log_record->table_name_size_);
        auto fh = sm_manager_->get_file_handle(table_name);
        fh->update_record(log_record->rid_, log_record->after_value_.data, nullptr);
    } catch (...) {
        // 更新可能失败，忽略错误
    }
}

/**
 * @description: 撤销插入操作（即删除记录）
 */
void RecoveryManager::undo_insert(InsertLogRecord* log_record) {
    try {
        std::string table_name(log_record->table_name_, log_record->table_name_size_);
        auto fh = sm_manager_->get_file_handle(table_name);
        fh->delete_record(log_record->rid_, nullptr);
    } catch (...) {
        // 记录可能已被删除，忽略错误
    }
}

/**
 * @description: 撤销删除操作（即重新插入记录）
 */
void RecoveryManager::undo_delete(DeleteLogRecord* log_record) {
    try {
        std::string table_name(log_record->table_name_, log_record->table_name_size_);
        auto fh = sm_manager_->get_file_handle(table_name);
        fh->insert_record(log_record->rid_, log_record->delete_value_.data);
    } catch (...) {
        // 插入可能失败，忽略错误
    }
}

/**
 * @description: 撤销更新操作（即恢复到旧值）
 */
void RecoveryManager::undo_update(UpdateLogRecord* log_record) {
    try {
        std::string table_name(log_record->table_name_, log_record->table_name_size_);
        auto fh = sm_manager_->get_file_handle(table_name);
        fh->update_record(log_record->rid_, log_record->before_value_.data, nullptr);
    } catch (...) {
        // 更新可能失败，忽略错误
    }
}