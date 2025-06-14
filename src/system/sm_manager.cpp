/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sm_manager.h"

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>

#include "index/ix.h"
#include "record/rm.h"
#include "record_printer.h"

/**
 * @description: 判断是否为一个文件夹
 * @return {bool} 返回是否为一个文件夹
 * @param {string&} db_name 数据库文件名称，与文件夹同名
 */
bool SmManager::is_dir(const std::string &db_name) {
    struct stat st;
    return stat(db_name.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * @description: 创建数据库，所有的数据库相关文件都放在数据库同名文件夹下
 * @param {string&} db_name 数据库名称
 */
void SmManager::create_db(const std::string &db_name) {
    if (is_dir(db_name)) {
        throw DatabaseExistsError(db_name);
    }
    // 为数据库创建一个子目录
    std::string cmd = "mkdir " + db_name;
    if (system(cmd.c_str()) < 0) {  // 创建一个名为db_name的目录
        throw UnixError();
    }
    if (chdir(db_name.c_str()) < 0) {  // 进入名为db_name的目录
        throw UnixError();
    }
    // 创建系统目录
    DbMeta *new_db = new DbMeta();
    new_db->name_ = db_name;

    // 注意，此处ofstream会在当前目录创建(如果没有此文件先创建)和打开一个名为DB_META_NAME的文件
    std::ofstream ofs(DB_META_NAME);

    // 将new_db中的信息，按照定义好的operator<<操作符，写入到ofs打开的DB_META_NAME文件中
    ofs << *new_db;  // 注意：此处重载了操作符<<

    delete new_db;

    // 创建日志文件
    disk_manager_->create_file(LOG_FILE_NAME);

    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 删除数据库，同时需要清空相关文件以及数据库同名文件夹
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::drop_db(const std::string &db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    std::string cmd = "rm -r " + db_name;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 打开数据库，找到数据库对应的文件夹，并加载数据库元数据和相关文件
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::open_db(const std::string &db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }

    if (!db_.name_.empty()) {
        throw DatabaseExistsError(db_name);
    }

    if (chdir(db_name.c_str()) < 0) {
        throw UnixError();
    }

    std::ifstream ifs(DB_META_NAME);
    if (!ifs) {
        throw UnixError();
    }

    ifs >> db_;

    for (auto &tab : db_.tabs_) {
        const std::string &tab_name = tab.first;
        auto &tab_meta = tab.second;
        fhs_[tab_name] = rm_manager_->open_file(tab_name);
        for (auto &index : tab_meta.indexes) {
            std::string ix_name = get_ix_manager()->get_index_name(tab_name, index.cols);
            ihs_[ix_name] = ix_manager_->open_index(tab_name, index.cols);
        }
    }
}

/**
 * @description: 把数据库相关的元数据刷入磁盘中
 */
void SmManager::flush_meta() {
    // 默认清空文件
    std::ofstream ofs(DB_META_NAME);
    ofs << db_;
}

/**
 * @description: 关闭数据库并把数据落盘
 */
void SmManager::close_db() {
    if (db_.name_.empty()) {
        throw DatabaseNotFoundError(db_.name_);
    }

    flush_meta();

    ihs_.clear();
    fhs_.clear();

    db_.tabs_.clear();
    db_.name_.clear();

    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 显示所有的表,通过测试需要将其结果写入到output.txt,详情看题目文档
 * @param {Context*} context
 */
void SmManager::show_tables(Context *context) {
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);
    outfile << "| Tables |\n";
    RecordPrinter printer(1);
    printer.print_separator(context);
    printer.print_record({"Tables"}, context);
    printer.print_separator(context);
    for (auto &entry : db_.tabs_) {
        auto &tab = entry.second;
        printer.print_record({tab.name}, context);
        outfile << "| " << tab.name << " |\n";
    }
    printer.print_separator(context);
    outfile.close();
}

/**
 * @description: 显示表的元数据
 * @param {string&} tab_name 表名称
 * @param {Context*} context
 */
void SmManager::desc_table(const std::string &tab_name, Context *context) {
    TabMeta &tab = db_.get_table(tab_name);

    std::vector<std::string> captions = {"Field", "Type", "Index"};
    RecordPrinter printer(captions.size());
    // Print header
    printer.print_separator(context);
    printer.print_record(captions, context);
    printer.print_separator(context);
    // Print fields
    for (auto &col : tab.cols) {
        std::vector<std::string> field_info = {col.name, coltype2str(col.type), col.index ? "YES" : "NO"};
        printer.print_record(field_info, context);
    }
    // Print footer
    printer.print_separator(context);
}

/**
 * @description: 显示表的索引信息
 * @param {string&} tab_name 表名称
 * @param {Context*} context
 */
void SmManager::show_index(const std::string &tab_name, Context *context) {
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }

    TabMeta &tab = db_.get_table(tab_name);

    std::vector<std::string> captions = {"Table", "Unique", "Key_name"};
    RecordPrinter printer(captions.size());

    // 打印表头 - 控制台输出使用标准表格格式
    printer.print_separator(context);
    printer.print_record(captions, context);
    printer.print_separator(context);

    // 写入output.txt文件
    std::fstream outfile;
    outfile.open("output.txt", std::ios::out | std::ios::app);

    // 遍历表的所有索引
    for (auto &index : tab.indexes) {
        // 构造列名列表字符串，格式为 (col1,col2,...)
        std::string col_list = "(";
        for (size_t i = 0; i < index.cols.size(); ++i) {
            if (i > 0) col_list += ",";
            col_list += index.cols[i].name;
        }
        col_list += ")";

        // 构造记录数据
        std::vector<std::string> record = {tab_name, "unique", col_list};

        // 打印到控制台缓冲区（使用标准表格格式）
        printer.print_record(record, context);

        // 写入到文件（保持原有格式）
        outfile << "| " << tab_name << " | unique | " << col_list << " |\n";
    }

    // 打印表尾分隔符
    printer.print_separator(context);

    outfile.close();
}

/**
 * @description: 创建表
 * @param {string&} tab_name 表的名称
 * @param {vector<ColDef>&} col_defs 表的字段
 * @param {Context*} context
 */
void SmManager::create_table(const std::string &tab_name, const std::vector<ColDef> &col_defs, Context *context) {
    if (db_.is_table(tab_name)) {
        throw TableExistsError(tab_name);
    }

    // Create table meta
    int curr_offset = 0;
    TabMeta tab;
    tab.name = tab_name;
    for (auto &col_def : col_defs) {
        ColMeta col = {.tab_name = tab_name,
                       .name = col_def.name,
                       .type = col_def.type,
                       .len = col_def.len,
                       .offset = curr_offset,
                       .index = false};
        curr_offset += col_def.len;
        tab.cols.push_back(col);
    }
    // Create & open record file
    int record_size = curr_offset;  // record_size就是col meta所占的大小（表的元数据也是以记录的形式进行存储的）
    rm_manager_->create_file(tab_name, record_size);
    db_.tabs_[tab_name] = tab;
    // fhs_[tab_name] = rm_manager_->open_file(tab_name);
    fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));

    flush_meta();
}

/**
 * @description: 删除表
 * @param {string&} tab_name 表的名称
 * @param {Context*} context
 */
void SmManager::drop_table(const std::string &tab_name, Context *context) {
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    TabMeta &tab = db_.get_table(tab_name);

    // 首先关闭并清理所有索引
    for (auto &index : tab.indexes) {
        std::string ix_name = get_ix_manager()->get_index_name(tab_name, index.cols);
        if (ihs_.count(ix_name) > 0) {
            auto &ih_ = ihs_.at(ix_name);
            ix_manager_->close_index(ih_.get());
            ihs_.erase(ix_name);
        }
        // 确保索引文件被完全删除
        ix_manager_->destroy_index(tab_name, index.cols);
    }
    tab.indexes.clear();

    // 然后关闭并删除表文件
    if (fhs_.count(tab_name) > 0) {
        rm_manager_->close_file(fhs_[tab_name].get());
        fhs_.erase(tab_name);
    }
    rm_manager_->destroy_file(tab_name);

    // 最后从元数据中删除表信息
    db_.tabs_.erase(tab_name);
    flush_meta();
}

/**
 * @description: 创建索引
 * @param {string&} tab_name 表的名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::create_index(const std::string &tab_name, const std::vector<std::string> &col_names, Context *context) {
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }

    TabMeta &tab = db_.get_table(tab_name);

    // 检查索引是否已存在
    if (tab.is_index(col_names)) {
        throw IndexExistsError(tab_name, col_names);
    }

    std::vector<ColMeta> idx_cols;
    int tot_len = 0;
    for (auto &col_name : col_names) {
        // 检查列是否存在
        if (!tab.is_col(col_name)) {
            throw ColumnNotFoundError(col_name);
        }
        // 获取列元数据并添加到索引列集合
        idx_cols.push_back(*tab.get_col(col_name));
        tot_len += idx_cols.back().len;
    }

    // 检查索引文件是否已经存在，如果存在则先删除
    if (ix_manager_->exists(tab_name, idx_cols)) {
        ix_manager_->destroy_index(tab_name, idx_cols);
    }

    ix_manager_->create_index(tab_name, idx_cols);

    // 打开索引文件获取索引句柄
    auto ih = ix_manager_->open_index(tab_name, idx_cols);

    // 从表中获取所有现有记录并添加到索引
    auto fh = fhs_[tab_name].get();
    for (RmScan scan(fh); !scan.is_end(); scan.next()) {
        // 获取记录
        auto rid = scan.rid();
        auto record = fh->get_record(rid, context);

        // 构建索引键
        char *key = new char[tot_len];
        int offset = 0;
        for (auto &col : idx_cols) {
            memcpy(key + offset, record->data + col.offset, col.len);
            offset += col.len;
        }

        // 将记录插入索引，现在不会因为重复键而抛出异常
        ih->insert_entry(key, rid, context->txn_);

        delete[] key;
    }

    // 创建索引元数据
    IndexMeta index_meta;
    index_meta.tab_name = tab_name;
    index_meta.col_tot_len = tot_len;
    index_meta.col_num = idx_cols.size();
    index_meta.cols = idx_cols;

    // 更新表元数据
    tab.indexes.push_back(index_meta);

    // 保存索引句柄
    std::string index_name = ix_manager_->get_index_name(tab_name, idx_cols);
    if (ihs_.count(index_name)) {
        throw IndexExistsError(tab_name, col_names);
    }
    ihs_[index_name] = std::move(ih);

    flush_meta();
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string &tab_name, const std::vector<std::string> &col_names, Context *context) {
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
    TabMeta &tab = db_.get_table(tab_name);
    if (!tab.is_index(col_names)) {
        return;
    }
    auto index_name = ix_manager_->get_index_name(tab_name, col_names);
    if (ihs_.count(index_name) == 0) {
        return;
    }
    auto ih = std::move(ihs_.at(index_name));
    ix_manager_->close_index(ih.get());
    ix_manager_->destroy_index(tab_name, col_names);
    ihs_.erase(index_name);
    auto index_meta = tab.get_index_meta(col_names);
    tab.indexes.erase(index_meta);
    flush_meta();
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<ColMeta>&} 索引包含的字段元数据
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string &tab_name, const std::vector<ColMeta> &cols, Context *context) {
    std::vector<std::string> col_names;

    for (auto &col : cols) {
        col_names.push_back(col.name);
    }

    drop_index(tab_name, col_names, context);
}