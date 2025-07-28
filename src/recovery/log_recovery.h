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

#include <map>
#include <set>
#include <unordered_map>

#include "log_manager.h"
#include "storage/disk_manager.h"
#include "system/sm_manager.h"

extern DiskManager disk_manager;
extern BufferPoolManager buffer_pool_manager;
extern SmManager sm_manager;
extern LogManager log_manager;

class RedoLogsInPage {
   public:
    RedoLogsInPage() { table_file_ = nullptr; }
    RmFileHandle* table_file_;
    std::vector<lsn_t> redo_logs_;  // 在该page上需要redo的操作的lsn
};

class RecoveryManager {
   public:
    RecoveryManager() = default;

    void analyze();
    void redo();
    void undo();

   private:
    // 恢复相关的辅助方法
    void ensure_page_exists(RmFileHandle* fh, int target_page_no);

    void redo_insert(InsertLogRecord* log_record);
    void redo_delete(DeleteLogRecord* log_record);
    void redo_update(UpdateLogRecord* log_record);

    void undo_insert(InsertLogRecord* log_record);
    void undo_delete(DeleteLogRecord* log_record);
    void undo_update(UpdateLogRecord* log_record);

    LogBuffer buffer_;  // 读入日志

    std::set<txn_id_t> undo_list_;  // 需要回滚的事务列表
    std::set<txn_id_t> redo_list_;  // 需要重做的事务列表
};