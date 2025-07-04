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
        fhs_[tab_name] = rm_manager_->open_file(tab_name);
        for (auto& index : tab_meta.indexes) {
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
    rm_manager_->create_file(tab_name, record_size);
    db_.tabs_[tab_name] = tab;
    // fhs_[tab_name] = rm_manager_->open_file(tab_name);
    fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));

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
        rm_manager_->close_file_and_delete_pages(it->second.get());
        fhs_.erase(it);
    }

    // 删除表文件和元数据
    rm_manager_->destroy_file(tab_name);
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

    if (ix_manager_->exists(tab_name, col_names)) {
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
    ix_manager_->create_index(tab_name, cols);
    auto ih = ix_manager_->open_index(tab_name, cols);
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
        ix_manager_->drop_index(ih.get());
        disk_manager_->destroy_file(ix_manager_->get_index_name(tab_name, col_names));
        throw;
    }

    // 提交更改
    const auto index_name = ix_manager_->get_index_name(tab_name, col_names);
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
    if (!ix_manager_->exists(tab_name, col_names)) {
        throw IndexNotFoundError(tab_name, col_names);
    }

    const auto index_name = ix_manager_->get_index_name(tab_name, col_names);

    // 关闭并删除索引句柄
    if (auto it = ihs_.find(index_name); it != ihs_.end()) {
        ix_manager_->drop_index(it->second.get());
        ihs_.erase(it);
    }

    // 删除索引文件，并从表元数据中移除索引
    disk_manager_->destroy_file(index_name);
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

void SmManager::load_csv_data(const std::string& csv_file_path, const std::string& tab_name) {
    // 路径解析：支持绝对路径和两种相对路径（当前目录和上级目录）
    std::string resolved_path;
    bool found = false;
    char cwd[4096];
    getcwd(cwd, sizeof(cwd));

    // 绝对路径
    if (!csv_file_path.empty() && (csv_file_path[0] == '/' || (csv_file_path.size() > 1 && csv_file_path[1] == ':'))) {
        std::ifstream test(csv_file_path);
        if (test.is_open()) {
            resolved_path = csv_file_path;
            found = true;
        }
    }
    // 相对路径：当前目录
    if (!found) {
        std::string try1 = std::string(cwd) + "/" + csv_file_path;
        std::ifstream test(try1);
        if (test.is_open()) {
            resolved_path = try1;
            found = true;
        }
    }
    // 相对路径：上级目录
    if (!found) {
        std::string parent = std::string(cwd);
        size_t pos = parent.find_last_of('/');
        if (pos != std::string::npos) parent = parent.substr(0, pos);
        std::string try2 = parent + "/" + csv_file_path;
        std::ifstream test(try2);
        if (test.is_open()) {
            resolved_path = try2;
            found = true;
        }
    }
    // 最后兜底
    if (!found) {
        resolved_path = csv_file_path;
    }

    std::ifstream file(resolved_path);
    if (!file.is_open()) {
        throw std::runtime_error("无法打开CSV文件: " + resolved_path + "（原始路径: " + csv_file_path + ")");
    }

    auto tab_ = db_.get_table(tab_name);
    auto fh_ = fhs_.at(tab_name).get();
    size_t record_size = fh_->get_file_hdr().record_size;
    char* record = new char[record_size];

    std::string line;
    bool first_line = true;
    std::vector<std::string> headers;

    // 健壮CSV分割，支持引号、逗号、空字段
    auto parse_csv_line = [](const std::string& line) -> std::vector<std::string> {
        std::vector<std::string> result;
        std::string field;
        bool in_quotes = false;
        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (c == '"') {
                if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else {
                    in_quotes = !in_quotes;
                }
            } else if (c == ',' && !in_quotes) {
                result.push_back(field);
                field.clear();
            } else {
                field += c;
            }
        }
        result.push_back(field);
        return result;
    };

    size_t line_num = 0;
    while (std::getline(file, line)) {
        ++line_num;
        if (line.empty()) continue;
        auto fields = parse_csv_line(line);

        // 处理第一行(表头)
        if (first_line) {
            headers = fields;
            if (headers.size() != tab_.cols.size()) {
                delete[] record;
                throw std::runtime_error("CSV字段数与表结构不一致，表字段数: " + std::to_string(tab_.cols.size()) +
                                         "，CSV字段数: " + std::to_string(headers.size()));
            }
            for (size_t i = 0; i < headers.size(); ++i) {
                std::string csv_col = headers[i];
                std::string tab_col = tab_.cols[i].name;
                // 去除空格
                while (!csv_col.empty() && (csv_col.front() == ' ' || csv_col.front() == '"')) csv_col.erase(0, 1);
                while (!csv_col.empty() && (csv_col.back() == ' ' || csv_col.back() == '"')) csv_col.pop_back();
                if (csv_col != tab_col) {
                    delete[] record;
                    throw std::runtime_error("CSV表头与表结构不一致，第" + std::to_string(i + 1) + "列: " + csv_col + " != " + tab_col);
                }
            }
            first_line = false;
            continue;
        }

        if (fields.size() != tab_.cols.size()) {
            delete[] record;
            throw std::runtime_error("第" + std::to_string(line_num) + "行字段数与表结构不一致，期望" +
                                     std::to_string(tab_.cols.size()) + "，实际" + std::to_string(fields.size()));
        }

        std::memset(record, 0, record_size);

        for (size_t i = 0; i < tab_.cols.size(); ++i) {
            const auto& col = tab_.cols[i];
            std::string value = fields[i];
            // 去除字段值前后空格和引号
            while (!value.empty() && (value.front() == ' ' || value.front() == '"')) value.erase(0, 1);
            while (!value.empty() && (value.back() == ' ' || value.back() == '"')) value.pop_back();

            try {
                switch (col.type) {
                    case TYPE_INT: {
                        int int_value = value.empty() ? 0 : std::stoi(value);
                        memcpy(record + col.offset, &int_value, col.len);
                        break;
                    }
                    case TYPE_FLOAT: {
                        float float_value = value.empty() ? 0.0f : std::stof(value);
                        memcpy(record + col.offset, &float_value, col.len);
                        break;
                    }
                    case TYPE_STRING: {
                        memset(record + col.offset, 0, col.len);
                        size_t copy_len = std::min(value.size(), static_cast<size_t>(col.len));
                        memcpy(record + col.offset, value.c_str(), copy_len);
                        break;
                    }
                    default:
                        delete[] record;
                        throw std::runtime_error("未知字段类型: " + std::to_string(static_cast<int>(col.type)));
                }
            } catch (const std::exception& e) {
                delete[] record;
                throw std::runtime_error("第" + std::to_string(line_num) + "行，第" + std::to_string(i + 1) +
                                         "列（" + col.name + "）类型转换失败，值: " + value + "，错误: " + e.what());
            }
        }

        // 插入记录
        auto rid_ = fh_->insert_record(record, nullptr);

        // 更新索引
        for (const auto& index : tab_.indexes) {
            auto idx_name = ix_manager_->get_index_name(tab_name, index.cols);
            auto ih = ihs_.at(idx_name).get();
            auto key = std::make_unique<char[]>(index.col_tot_len);
            int offset_ = 0;
            for (size_t i = 0; i < static_cast<unsigned long>(index.col_num); ++i) {
                std::memcpy(key.get() + offset_, record + index.cols[i].offset, index.cols[i].len);
                offset_ += index.cols[i].len;
            }
            ih->insert_entry(key.get(), rid_, nullptr);
        }
    }

    delete[] record;
    file.close();
}
