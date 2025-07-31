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

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <utility>

#include "errors.h"
#include "index/ix.h"
#include "record/rm.h"
#include "record/rm_file_handle.h"
#include "record_printer.h"

extern DiskManager disk_manager;
extern BufferPoolManager buffer_pool_manager;
extern RmManager rm_manager;
extern IxManager ix_manager;

/**
 * @description: 判断是否为一个文件夹
 * @return {bool} 返回是否为一个文件夹
 * @param {string&} db_name 数据库文件名称，与文件夹同名
 */
bool SmManager::is_dir(const std::string& db_name) {
    struct stat st;
    return stat(db_name.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * @description: 创建数据库，所有的数据库相关文件都放在数据库同名文件夹下
 * @param {string&} db_name 数据库名称
 */
void SmManager::create_db(const std::string& db_name) {
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
    DbMeta* new_db = new DbMeta();
    new_db->name_ = db_name;

    // 注意，此处ofstream会在当前目录创建(如果没有此文件先创建)和打开一个名为DB_META_NAME的文件
    std::ofstream ofs(DB_META_NAME);

    // 将new_db中的信息，按照定义好的operator<<操作符，写入到ofs打开的DB_META_NAME文件中
    ofs << *new_db;  // 注意：此处重载了操作符<<

    delete new_db;

    // 创建日志文件
    disk_manager.create_file(LOG_FILE_NAME);

    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 删除数据库，同时需要清空相关文件以及数据库同名文件夹
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::drop_db(const std::string& db_name) {
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
void SmManager::open_db(const std::string& db_name) {
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

    for (auto& tab : db_.tabs_) {
        const std::string& tab_name = tab.first;
        auto& tab_meta = tab.second;
        fhs_[tab_name] = rm_manager.open_file(tab_name);
        for (auto& index : tab_meta.indexes) {
            std::string ix_name = get_ix_manager()->get_index_name(tab_name, index.cols);
            ihs_[ix_name] = ix_manager.open_index(tab_name, index.cols);
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

    for (const auto& [table_name, file_handle] : fhs_) {
        rm_manager.close_file_and_clear_buffer(file_handle.get());
    }

    // 正确关闭所有索引文件，确保数据持久化
    for (auto& [index_name, ih] : ihs_) {
        ix_manager.close_index(ih.get());
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
void SmManager::show_tables(Context* context) {
    std::fstream outfile;
    extern bool output_file_enabled;
    if (output_file_enabled) {
        outfile.open("output.txt", std::ios::out | std::ios::app);
    }
    if (output_file_enabled) {
        outfile << "| Tables |\n";
    }
    RecordPrinter printer(1);
    printer.print_separator(context);
    printer.print_record({"Tables"}, context);
    printer.print_separator(context);
    for (auto& entry : db_.tabs_) {
        auto& tab = entry.second;
        printer.print_record({tab.name}, context);
        if (output_file_enabled) {
            outfile << "| " << tab.name << " |\n";
        }
    }
    printer.print_separator(context);
    outfile.close();
}

/**
 * @description: 显示表的元数据
 * @param {string&} tab_name 表名称
 * @param {Context*} context
 */
void SmManager::desc_table(const std::string& tab_name, Context* context) {
    TabMeta& tab = db_.get_table(tab_name);

    std::vector<std::string> captions = {"Field", "Type", "Index"};
    RecordPrinter printer(captions.size());
    // Print header
    printer.print_separator(context);
    printer.print_record(captions, context);
    printer.print_separator(context);
    // Print fields
    for (auto& col : tab.cols) {
        std::vector<std::string> field_info = {col.name, coltype2str(col.type), col.index ? "YES" : "NO"};
        printer.print_record(field_info, context);
    }
    // Print footer
    printer.print_separator(context);
}

/**
 * @description: 创建表
 * @param {string&} tab_name 表的名称
 * @param {vector<ColDef>&} col_defs 表的字段
 * @param {Context*} context
 */
void SmManager::create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context) {
    if (db_.is_table(tab_name)) {
        throw TableExistsError(tab_name);
    }
    // Create table meta
    int curr_offset = 0;
    TabMeta tab;
    tab.name = tab_name;
    for (auto& col_def : col_defs) {
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
    rm_manager.create_file(tab_name, record_size);
    db_.tabs_[tab_name] = tab;
    // fhs_[tab_name] = rm_manager_->open_file(tab_name);
    fhs_.emplace(tab_name, rm_manager.open_file(tab_name));

    flush_meta();
}

/**
 * @description: 删除指定的表
 * @param {string&} tab_name 表的名称
 * @param {Context*} context 执行上下文
 */
void SmManager::drop_table(const std::string& tab_name, Context* context) {
    // 检查表是否存在并获取表元数据
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }

    TabMeta& tab = db_.get_table(tab_name);

    // 删除所有索引（从后向前删除避免迭代器失效）
    while (!tab.indexes.empty()) {
        const auto& index = tab.indexes.back();
        std::vector<std::string> col_names;
        col_names.reserve(index.cols.size());
        for (const auto& col : index.cols) {
            col_names.emplace_back(col.name);
        }
        drop_index(tab_name, col_names, context);
    }

    // 关闭文件句柄并清理资源
    if (auto it = fhs_.find(tab_name); it != fhs_.end()) {
        rm_manager.close_file_and_delete_pages(it->second.get());
        fhs_.erase(it);
    }

    // 删除表文件和元数据
    rm_manager.destroy_file(tab_name);
    db_.tabs_.erase(tab_name);

    flush_meta();
}

/**
 * @description: 显示指定表的索引信息
 *
 * @param {string&} tab_name 表名称
 * @param {Context*} context 执行上下文
 */
void SmManager::show_index(const std::string& tab_name, Context* context) {
    TabMeta& tab = db_.get_table(tab_name);
    extern bool output_file_enabled;
    std::fstream outfile;
    if (output_file_enabled) {
        outfile.open("output.txt", std::ios::out | std::ios::app);
    }

    // 设置表格列标题
    const std::vector<std::string> captions = {"Table", "Unique", "KeyName"};
    RecordPrinter printer(captions.size());

    // 打印表头
    printer.print_separator(context);
    printer.print_record(captions, context);
    printer.print_separator(context);

    // 遍历表的所有索引
    for (const auto& index : tab.indexes) {
        std::string col_names = "(";
        for (size_t i = 0; i < index.cols.size(); ++i) {
            if (i != 0)
                col_names += ",";
            col_names += index.cols[i].name;
        }
        col_names += ")";

        // 打印到控制台
        const std::vector<std::string> index_info = {tab_name, "unique", col_names};
        printer.print_record(index_info, context);

        // 写入文件
        if (output_file_enabled) {
            outfile << "| " << tab_name << " | unique | " << col_names << " |\n";
        }
    }

    // 打印表尾
    printer.print_separator(context);
    outfile.close();
}

/**
 * @description: 在指定表上创建索引
 * @param {string&} tab_name 表的名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context 执行上下文
 */
void SmManager::create_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    TabMeta& tab = db_.get_table(tab_name);

    if (ix_manager.exists(tab_name, col_names)) {
        throw IndexExistsError(tab_name, col_names);
    }

    // 预分配并收集列元数据
    const size_t col_count = col_names.size();
    std::vector<ColMeta> cols;
    std::vector<std::pair<int, int>> col_info;  // {record_offset, key_offset}
    cols.reserve(col_count);
    col_info.reserve(col_count);

    int tot_col_len = 0;
    for (const auto& col_name : col_names) {
        const auto& col_meta = *tab.get_col(col_name);
        cols.emplace_back(col_meta);
        col_info.emplace_back(col_meta.offset, tot_col_len);
        tot_col_len += col_meta.len;
    }

    // 创建索引文件和句柄
    ix_manager.create_index(tab_name, cols);
    auto ih = ix_manager.open_index(tab_name, cols);
    auto* const fh = fhs_[tab_name].get();

    // 批量构建索引
    std::vector<char> key_buffer(tot_col_len);
    char* const key = key_buffer.data();

    try {
        for (RmScan scan(fh); !scan.is_end(); scan.next()) {
            const auto record = fh->get_record(scan.rid(), context);
            const char* const record_data = record->data;

            // 减少循环开销
            for (size_t i = 0; i < col_count; ++i) {
                const auto& [record_offset, key_offset] = col_info[i];
                std::memcpy(key + key_offset, record_data + record_offset, cols[i].len);
            }

            if (ih->insert_entry(key, scan.rid(), context ? context->txn_ : nullptr) == INVALID_PAGE_ID) {
                throw std::runtime_error("Index insertion failed");
            }
        }
    } catch (...) {
        // 清理失败的索引
        ix_manager.drop_index(ih.get());
        disk_manager.destroy_file(ix_manager.get_index_name(tab_name, col_names));
        throw;
    }

    // 提交更改
    const auto index_name = ix_manager.get_index_name(tab_name, col_names);
    ihs_.emplace(index_name, std::move(ih));

    tab.indexes.emplace_back(IndexMeta{
        tab_name,
        tot_col_len,
        static_cast<int>(col_count),
        std::move(cols)});

    flush_meta();
}

/**
 * @description: 删除指定表上的索引
 * @param {string&} tab_name 表名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context 执行上下文
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    if (!ix_manager.exists(tab_name, col_names)) {
        throw IndexNotFoundError(tab_name, col_names);
    }

    const auto index_name = ix_manager.get_index_name(tab_name, col_names);

    // 关闭并删除索引句柄
    if (auto it = ihs_.find(index_name); it != ihs_.end()) {
        ix_manager.drop_index(it->second.get());
        ihs_.erase(it);
    }

    // 删除索引文件，并从表元数据中移除索引
    disk_manager.destroy_file(index_name);
    TabMeta& tab = db_.get_table(tab_name);
    auto index_it = tab.get_index_meta(col_names);
    tab.indexes.erase(index_it);

    flush_meta();
}

/**
 * @description: 删除索引的重载函数
 *
 * @param {string&} tab_name 表名称
 * @param {vector<ColMeta>&} cols 索引包含的字段元数据
 * @param {Context*} context 执行上下文
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<ColMeta>& cols, Context* context) {
    std::vector<std::string> col_names;
    col_names.reserve(cols.size());
    for (const auto& col : cols) {
        col_names.emplace_back(col.name);
    }
    drop_index(tab_name, col_names, context);
}

bool SmManager::delete_index_with_rid(const std::string& tab_name, RmRecord& rec, Rid rid, Transaction* txn) {
    TabMeta& tab_ = db_.get_table(tab_name);
    // 遍历表的所有索引
    for (size_t i = 0; i < tab_.indexes.size(); ++i) {
        auto& index = tab_.indexes[i];
        auto ih = ihs_.at(get_ix_manager()->get_index_name(tab_.name, index.cols)).get();
        auto key = std::make_unique<char[]>(index.col_tot_len);
        int offset = 0;
        for (size_t j = 0; j < static_cast<size_t>(index.col_num); ++j) {
            memcpy(key.get() + offset, rec.data + index.cols[j].offset, index.cols[j].len);
            offset += index.cols[j].len;
        }
        // 删除索引项
        ih->delete_entry_with_rid(key.get(), rid, txn);
    }
    return true;
}

RmFileHandle* SmManager::get_file_handle(const std::string& tab_name) const {
    return fhs_.at(tab_name).get();
}

IxIndexHandle* SmManager::get_index_handle(
    const std::string& index_name) const {
    return ihs_.at(index_name).get();
}

bool SmManager::contains_table(const std::string& tab_name) const {
    return db_.is_table(tab_name);
}

void SmManager::load_csv_data(const std::string& csv_file_path, const std::string& tab_name, Context* context) {
    // 检查表是否存在
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }

    // 打开CSV文件并验证
    std::ifstream file(csv_file_path, std::ios::binary);
    if (!file.is_open()) {
        throw RMDBError("Cannot open CSV file: " + csv_file_path);
    }

    // 获取文件大小
    file.seekg(0, std::ios::end);
    const size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // 预分配大缓冲区，一次性读取整个文件
    std::vector<char> file_buffer(file_size + 1);
    file.read(file_buffer.data(), file_size);
    file_buffer[file_size] = '\0';  // 确保字符串结束
    file.close();

    // 使用DB2025风格的load_data函数
    load_data(file_buffer.data(), file_buffer.data() + file_size, tab_name, context);
}

void SmManager::load_data(char* start_pos, char* end_pos, const std::string& tab_name, Context* context) {
    TabMeta& tab = db_.get_table(tab_name);
    auto fh = fhs_[tab_name].get();
    const size_t record_size = fh->get_file_hdr().record_size;

    int max_record_num = fh->get_file_hdr().num_records_per_page;
    int len = max_record_num * record_size;
    std::vector<char> record_buffer(len);
    char* record_data = record_buffer.data();
    int count = 0;
    size_t offset = 0;
    memset(record_data, 0, len);  // 初始化记录缓冲区

    // 跳过第一行（表头）
    char* current_pos = std::find(start_pos, end_pos, '\n');
    if (current_pos != end_pos) {
        current_pos++;  // 跳过换行符
    }

    // 批量处理数据
    while (current_pos < end_pos) {
        size_t start_offset = offset;

        // 解析一行数据
        for (size_t i = 0; i < tab.cols.size() && current_pos < end_pos; ++i) {
            char* line_end = std::find_if(current_pos, end_pos, [](char c) { return c == ',' || c == '\n'; });
            const auto& col = tab.cols[i];
            int field_len = line_end - current_pos;

            switch (col.type) {
                case ColType::TYPE_INT: {
                    std::string field_value(current_pos, field_len);
                    int int_val = std::stoi(field_value);
                    memcpy(record_data + offset, &int_val, col.len);
                    break;
                }
                case ColType::TYPE_FLOAT: {
                    std::string field_value(current_pos, field_len);
                    float float_val = std::stof(field_value);
                    memcpy(record_data + offset, &float_val, col.len);
                    break;
                }
                case ColType::TYPE_STRING: {
                    // 将字符串复制到记录缓冲区，剩余空间填充0
                    int copy_len = std::min(field_len, col.len);
                    memcpy(record_data + offset, current_pos, copy_len);
                    break;
                }
                default:
                    throw RMDBError("Unsupported column type");
            }
            offset += col.len;
            current_pos = line_end;
            if (current_pos < end_pos && (*current_pos == ',' || *current_pos == '\n')) {
                current_pos++;
            }
        }

        // 跳到下一行
        while (current_pos < end_pos && *(current_pos - 1) != '\n') {
            current_pos++;
        }

        // 插入记录 - 使用简单的方式
        Rid rid = fh->insert_record(record_data + start_offset, context);

        // 插入索引
        if (!tab.indexes.empty()) {
            auto rec = std::make_unique<RmRecord>(record_size, record_data + start_offset);
            for (const auto& index : tab.indexes) {
                auto ih = ihs_.at(ix_manager.get_index_name(tab_name, index.cols)).get();
                auto key = std::make_unique<char[]>(index.col_tot_len);
                int key_offset = 0;
                for (size_t j = 0; j < static_cast<size_t>(index.col_num); ++j) {
                    memcpy(key.get() + key_offset, rec->data + index.cols[j].offset, index.cols[j].len);
                    key_offset += index.cols[j].len;
                }
                // 插入索引项
                ih->insert_entry(key.get(), rid, context ? context->txn_ : nullptr);
            }
        }

        count++;

        // 检查是否需要重置缓冲区
        if (count >= max_record_num || current_pos >= end_pos) {
            // 重置缓冲区
            memset(record_data, 0, len);
            offset = 0;
            count = 0;
        }
    }
}
