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

#include <limits>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

/**
 * @brief 索引扫描执行器
 *
 * IndexScanExecutor是RMDB数据库系统中负责执行基于索引的查询操作的核心组件。
 * 它继承自AbstractExecutor，遵循火山模型的执行框架。
 *
 * 主要功能：
 * 1. 利用B+树索引快速定位满足条件的记录
 * 2. 避免全表扫描，显著提升查询性能
 * 3. 支持等值查询和范围查询
 * 4. 与查询优化器协作，实现高效的索引访问路径
 *
 * 工作原理：
 * - 分析查询条件，确定可以利用索引的条件
 * - 构建索引查询的上下界范围
 * - 创建IxScan对象进行有序的索引遍历
 * - 对每个索引指向的记录进行条件过滤
 *
 * 性能优势：
 * - 时间复杂度：O(log n + k)，其中n是索引大小，k是结果集大小
 * - 相比全表扫描的O(n)有显著提升
 */
class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 目标表名称
    TabMeta tab_;                       // 表的元数据信息，包含列定义和索引信息
    std::vector<Condition> conds_;      // 查询条件列表，用于记录过滤
    RmFileHandle* fh_;                  // 表的数据文件句柄，用于读取实际记录
    std::vector<ColMeta> cols_;         // 需要输出的列元数据
    size_t len_;                        // 输出记录的总长度
    std::vector<Condition> fed_conds_;  // 下推的条件，与conds_相同

    std::vector<std::string> index_col_names_;  // 索引包含的列名列表
    IndexMeta index_meta_;                      // 使用的索引的元数据
    std::string index_name_;                    // 索引标识

    Rid rid_;                        // 当前记录的物理位置标识符
    std::unique_ptr<RecScan> scan_;  // 索引扫描器，实际类型为IxScan

    SmManager* sm_manager_;  // 系统管理器，协调各种资源

   public:
    /**
     * @brief IndexScanExecutor构造函数
     *
     * @param sm_manager 系统管理器指针，提供对数据库各种管理器的访问
     * @param tab_name 目标表名称
     * @param conds 查询条件列表
     * @param index_col_names 索引包含的列名列表
     * @param context 执行上下文，包含事务信息等
     *
     * 构造函数执行以下初始化工作：
     * 1. 获取表的元数据和索引元数据
     * 2. 处理条件中的表名推断和操作符标准化
     * 3. 为条件中的列名添加表名前缀（如果缺失）
     * 4. 标准化比较操作符的方向（确保左操作数是当前表的列）
     * 5. 计算输出记录的长度
     */
    IndexScanExecutor(SmManager* sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names, Context* context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);

        // 设置索引相关信息
        index_col_names_ = index_col_names;
        index_meta_ = *(tab_.get_index_meta(index_col_names_));

        // 获取数据文件句柄
        fh_ = sm_manager_->fhs_.at(tab_name_).get();

        // 设置输出列信息
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;

        // 获取索引名称并确保索引文件已打开
        index_name_ = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_col_names_);
        if (!sm_manager->ihs_.count(index_name_)) {
            // 如果没有打开则打开文件
            sm_manager->ihs_.emplace(index_name_, sm_manager_->get_ix_manager()->open_index(tab_name_, index_col_names_));
        }

        // 标准化查询条件：确保左操作数总是当前表的列
        for (auto& cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // 如果左操作数不是当前表的列，需要交换左右操作数
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op(cond.op);  // 调整比较操作符
            }
        }
        fed_conds_ = conds_;
    }

    /**
     * @brief 值比较辅助函数
     *
     * @param val1 第一个值
     * @param val2 第二个值
     * @param op 比较操作符
     * @return bool 比较结果
     */
    static inline bool compare_values(const Value& val1, const Value& val2, CompOp op) {
        // 确保两个值的类型相同
        if (val1.type != val2.type) {
            return false;
        }

        int cmp = 0;
        switch (val1.type) {
            case TYPE_INT:
                cmp = (val1.int_val < val2.int_val) ? -1 : (val1.int_val > val2.int_val) ? 1
                                                                                         : 0;
                break;
            case TYPE_FLOAT:
                cmp = (val1.float_val < val2.float_val) ? -1 : (val1.float_val > val2.float_val) ? 1
                                                                                                 : 0;
                break;
            case TYPE_STRING:
                cmp = val1.str_val.compare(val2.str_val);
                break;
            default:
                return false;
        }

        switch (op) {
            case OP_EQ:
                return cmp == 0;
            case OP_NE:
                return cmp != 0;
            case OP_LT:
                return cmp < 0;
            case OP_GT:
                return cmp > 0;
            case OP_LE:
                return cmp <= 0;
            case OP_GE:
                return cmp >= 0;
            default:
                return false;
        }
    }

    /**
     * @brief 操作符交换函数
     *
     * @param op 原操作符
     * @return CompOp 交换后的操作符
     */
    static inline CompOp swap_op(CompOp op) {
        switch (op) {
            case OP_EQ:
                return OP_EQ;
            case OP_NE:
                return OP_NE;
            case OP_LT:
                return OP_GT;
            case OP_GT:
                return OP_LT;
            case OP_LE:
                return OP_GE;
            case OP_GE:
                return OP_LE;
            default:
                return op;
        }
    }

    /**
     * @brief 初始化索引扫描
     *
     * 根据查询条件确定索引扫描的范围，并创建相应的索引扫描器。
     *
     * 处理流程：
     * 1. 分析查询条件，提取索引相关的条件
     * 2. 构建索引查询的上下界
     * 3. 创建IxScan对象进行索引扫描
     * 4. 定位到第一个满足条件的记录
     */
    void beginTuple() override {
        // 缓存索引句柄，避免重复查找
        auto ih = sm_manager_->ihs_.at(index_name_).get();

        // 使用局部缓冲区，避免线程本地变量的问题
        const size_t record_size = index_meta_.col_tot_len;
        std::vector<char> lower_buffer(record_size);
        std::vector<char> upper_buffer(record_size);

        RmRecord lower_record(lower_buffer.data(), record_size);
        RmRecord upper_record(upper_buffer.data(), record_size);

        off_t offset = 0;

        for (const auto& col : index_meta_.cols) {
            Value max_val, min_val;
            switch (col.type) {
                case ColType::TYPE_INT: {
                    max_val.set_int(std::numeric_limits<int>::max());
                    min_val.set_int(std::numeric_limits<int>::min());
                    break;
                }
                case ColType::TYPE_FLOAT: {
                    max_val.set_float(std::numeric_limits<float>::max());
                    min_val.set_float(std::numeric_limits<float>::lowest());
                    break;
                }
                case ColType::TYPE_STRING: {
                    max_val.set_str(std::string(col.len, '\xFF'));  // 使用0xFF而不是255
                    min_val.set_str(std::string(col.len, '\0'));    // 使用'\0'而不是0
                    break;
                }
                default:
                    throw InternalError("Unsupported column type in index scan");
            }
            for (const auto& cond : fed_conds_) {
                if (cond.lhs_col.col_name == col.name && cond.is_rhs_val) {
                    switch (cond.op) {
                        case CompOp::OP_EQ: {
                            if (compare_values(cond.rhs_val, min_val, CompOp::OP_GT)) {
                                min_val = cond.rhs_val;
                            }
                            if (compare_values(cond.rhs_val, max_val, CompOp::OP_LT)) {
                                max_val = cond.rhs_val;
                            }
                            break;
                        }
                        case CompOp::OP_LT:
                        case CompOp::OP_LE: {
                            if (compare_values(cond.rhs_val, max_val, CompOp::OP_LT)) {
                                max_val = cond.rhs_val;
                            }
                            break;
                        }
                        case CompOp::OP_GT:
                        case CompOp::OP_GE: {
                            if (compare_values(cond.rhs_val, min_val, CompOp::OP_GT)) {
                                min_val = cond.rhs_val;
                            }
                            break;
                        }
                        case CompOp::OP_NE: {
                            // 对于不等于的情况，忽略（索引扫描不支持不等于条件）
                            break;
                        }
                        default: {
                            throw InternalError("Unexpected comparison operator in index scan condition at " + getType());
                        }
                    }
                }
            }

            // 安全地复制值到记录缓冲区
            switch (col.type) {
                case ColType::TYPE_INT: {
                    memcpy(lower_record.data + offset, &min_val.int_val, sizeof(int));
                    memcpy(upper_record.data + offset, &max_val.int_val, sizeof(int));
                    break;
                }
                case ColType::TYPE_FLOAT: {
                    memcpy(lower_record.data + offset, &min_val.float_val, sizeof(float));
                    memcpy(upper_record.data + offset, &max_val.float_val, sizeof(float));
                    break;
                }
                case ColType::TYPE_STRING: {
                    // 确保字符串长度不超过列长度
                    size_t copy_len = std::min(static_cast<size_t>(col.len), min_val.str_val.length());
                    memcpy(lower_record.data + offset, min_val.str_val.c_str(), copy_len);
                    if (copy_len < static_cast<size_t>(col.len)) {
                        memset(lower_record.data + offset + copy_len, 0, col.len - copy_len);
                    }

                    copy_len = std::min(static_cast<size_t>(col.len), max_val.str_val.length());
                    memcpy(upper_record.data + offset, max_val.str_val.c_str(), copy_len);
                    if (copy_len < static_cast<size_t>(col.len)) {
                        memset(upper_record.data + offset + copy_len, 0, col.len - copy_len);
                    }
                    break;
                }
                default:
                    throw InternalError("Unsupported column type in index scan");
            }
            offset += col.len;
        }

        // 创建索引扫描器
        auto lower_iid = ih->lower_bound(lower_record.data);
        auto upper_iid = ih->upper_bound(upper_record.data);
        scan_ = std::make_unique<IxScan>(ih, lower_iid, upper_iid, sm_manager_->get_bpm());

        std::cerr << "debug2 - scan created" << std::endl;

        // 检查扫描器是否有效
        if (scan_ == nullptr) {
            std::cerr << "Error: Failed to create scan" << std::endl;
            return;
        }

        std::cerr << "debug2.5 - about to call find_next_valid_record" << std::endl;

        // 定位到第一个满足条件的记录
        find_next_valid_record();
        std::cerr << "debug1" << std::endl;
    }

    /**
     * @brief 查找下一个有效记录
     */
    inline void find_next_valid_record() {
        // 安全检查：确保扫描器已初始化
        if (scan_ == nullptr) {
            std::cerr << "Error: scan_ is null in find_next_valid_record" << std::endl;
            return;
        }

        while (!is_end()) {
            try {
                // 安全获取当前记录的RID
                rid_ = scan_->rid();
                std::cerr << "Got rid: page_no=" << rid_.page_no << ", slot_no=" << rid_.slot_no << std::endl;

                // 安全获取记录
                if (fh_ == nullptr) {
                    std::cerr << "Error: fh_ is null" << std::endl;
                    return;
                }

                std::cerr << "About to call get_record with rid: page_no=" << rid_.page_no << ", slot_no=" << rid_.slot_no << std::endl;

                auto rec = fh_->get_record(rid_, context_);

                std::cerr << "get_record completed successfully" << std::endl;

                if (rec == nullptr) {
                    std::cerr << "Error: get_record returned null" << std::endl;
                    scan_->next();
                    continue;
                }

                std::cerr << "About to check record pointer" << std::endl;
                if (rec.get() == nullptr) {
                    std::cerr << "Error: rec.get() returned null" << std::endl;
                    scan_->next();
                    continue;
                }

                std::cerr << "About to check cols_ size: " << cols_.size() << std::endl;
                std::cerr << "About to check fed_conds_ size: " << fed_conds_.size() << std::endl;

                // 评估条件
                std::cerr << "About to call eval_conds" << std::endl;
                bool conds_result = eval_conds(cols_, fed_conds_, rec.get());
                std::cerr << "eval_conds completed with result: " << conds_result << std::endl;

                if (conds_result) {
                    std::cerr << "Found valid record" << std::endl;
                    return;
                }

                scan_->next();
            } catch (const std::exception& e) {
                std::cerr << "Exception in find_next_valid_record: " << e.what() << std::endl;
                scan_->next();
            } catch (...) {
                std::cerr << "Unknown exception in find_next_valid_record" << std::endl;
                scan_->next();
            }
        }
        std::cerr << "Reached end of scan" << std::endl;
    }

    /**
     * @brief 移动到下一个满足条件的记录
     *
     * 在索引扫描过程中，继续向前移动扫描器，
     * 直到找到下一个满足所有条件的记录。
     */
    void nextTuple() override {
        if (scan_ == nullptr) {
            throw InternalError("Scan not initialized at " + getType());
        }
        if (!scan_->is_end()) {
            scan_->next();
        }
        // 查找下一个有效记录
        find_next_valid_record();
    }

    /**
     * @brief 获取下一个满足条件的记录
     *
     * 这是火山模型执行器的核心接口方法。
     *
     * @return std::unique_ptr<RmRecord> 下一个满足条件的记录，如果没有更多记录则返回nullptr
     *
     * 执行流程：
     * 1. 检查扫描器是否已初始化，如果没有则自动初始化
     * 2. 检查是否到达扫描末尾
     * 3. 获取当前记录
     * 4. 移动到下一个记录位置
     */
    std::unique_ptr<RmRecord> Next() override {
        if (scan_ == nullptr) {
            beginTuple();
        }

        // 检查是否已经到达扫描末尾
        if (is_end()) {
            return nullptr;
        }
        return fh_->get_record(rid_, context_);
    }

    /**
     * @brief 检查索引扫描是否已经结束
     *
     * @return bool 如果扫描结束返回true，否则返回false
     */
    bool is_end() const override {
        return scan_ == nullptr || scan_->is_end();
    }

    /**
     * @brief 获取选择列的总长度
     *
     * @return size_t 记录长度
     */
    size_t tupleLen() const override {
        return len_;
    }

    /**
     * @brief 获取选择的列元数据
     *
     * @return const std::vector<ColMeta>& 列元数据的常量引用
     */
    const std::vector<ColMeta>& cols() const override {
        return cols_;
    }

    /**
     * @brief 获取指定列的元数据
     * @param target 目标列的表列引用
     * @return 目标列的元数据
     */
    ColMeta get_col_offset(const TabCol& target) override {
        auto pos = get_col(cols_, target);
        return *pos;
    }

    /**
     * @brief 获取当前记录的位置标识符
     *
     * @return Rid& 当前记录在数据文件中的位置引用
     */
    Rid& rid() override { return rid_; }

    /**
     * @brief 获取执行器类型名称
     *
     * @return std::string 执行器类型标识
     */
    std::string getType() override { return "IndexScanExecutor"; }
};
