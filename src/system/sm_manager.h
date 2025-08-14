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

#include "common/context.h"
#include "index/ix.h"
#include "record/rm_file_handle.h"
#include "sm_defs.h"
#include "sm_meta.h"

extern DiskManager disk_manager;
extern BufferPoolManager buffer_pool_manager;
extern RmManager rm_manager;
extern IxManager ix_manager;

class Context;

struct ColDef {
    std::string name;  // Column name
    ColType type;      // Type of column
    int len;           // Length of column
};

/* 系统管理器，负责元数据管理和DDL语句的执行 */
class SmManager {
   public:
    DbMeta db_;                                                            // 当前打开的数据库的元数据
    std::unordered_map<std::string, std::unique_ptr<RmFileHandle>> fhs_;   // file name -> record file handle, 当前数据库中每张表的数据文件
    std::unordered_map<std::string, std::unique_ptr<IxIndexHandle>> ihs_;  // file name -> index file handle, 当前数据库中每个索引的文件

    SmManager() = default;
    ~SmManager() = default;

    BufferPoolManager* get_bpm() { return &buffer_pool_manager; }

    RmManager* get_rm_manager() { return &rm_manager; }

    IxManager* get_ix_manager() { return &ix_manager; }

    bool is_dir(const std::string& db_name);

    void create_db(const std::string& db_name);

    void drop_db(const std::string& db_name);

    void open_db(const std::string& db_name);

    void close_db();

    void flush_meta();

    void show_tables(Context* context);

    void desc_table(const std::string& tab_name, Context* context);

    void create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context);

    void drop_table(const std::string& tab_name, Context* context);

    void show_index(const std::string& tab_name, Context* context);

    void create_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context);

    void drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context);

    void drop_index(const std::string& tab_name, const std::vector<ColMeta>& col_names, Context* context);

    bool delete_index_with_rid(const std::string& tab_name, RmRecord& rec, Rid rid, Transaction* txn);

    RmFileHandle* get_file_handle(const std::string& tab_name) const;

    IxIndexHandle* get_index_handle(const std::string& index_name) const;

    bool contains_table(const std::string& tab_name) const;

    void load_csv_data(const std::string& csv_file_path, const std::string& tab_name, Context* context = nullptr);

    void load_csv_data_mmap(const std::string& csv_file_path, const std::string& tab_name, Context* context = nullptr);

    void load_csv_data_auto(const std::string& csv_file_path, const std::string& tab_name, Context* context = nullptr);

   private:
    void load_data(char* start_pos, char* end_pos, const std::string& tab_name, Context* context);
};
