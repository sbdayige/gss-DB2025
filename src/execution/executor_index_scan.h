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
    IndexScanExecutor(SmManager* sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names,
                      Context* context) {
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

        // 操作符映射表，用于交换左右操作数时调整操作符
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ},
            {OP_NE, OP_NE},
            {OP_LT, OP_GT},
            {OP_GT, OP_LT},
            {OP_LE, OP_GE},
            {OP_GE, OP_LE},
        };

        // 标准化查询条件：确保左操作数总是当前表的列
        for (auto& cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // 如果左操作数不是当前表的列，需要交换左右操作数
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);  // 调整比较操作符
            }
        }
        fed_conds_ = conds_;
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
        // 初始化条件值的raw数据
        init_condition_values();

        // 获取索引句柄
        auto index_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta_.cols);
        auto ih = sm_manager_->ihs_.at(index_name).get();

        // 分析条件，构建索引查询范围
        Iid lower_iid, upper_iid;
        bool has_range = false;

        // 为单列索引构建范围查询
        if (index_col_names_.size() == 1) {
            const std::string& index_col = index_col_names_[0];
            const auto& col_meta = *tab_.get_col(index_col);

            // 收集该列的所有条件
            Value lower_val, upper_val;
            bool has_lower = false, has_upper = false;
            bool lower_inclusive = true, upper_inclusive = true;

            for (const auto& cond : conds_) {
                if (!cond.is_rhs_val || cond.lhs_col.col_name != index_col) continue;

                switch (cond.op) {
                    case OP_EQ:
                        // 等值查询：设置相同的上下界
                        lower_val = upper_val = cond.rhs_val;
                        has_lower = has_upper = true;
                        lower_inclusive = upper_inclusive = true;
                        break;
                    case OP_GT:
                        // 大于：设置下界，不包含
                        if (!has_lower || compare_values(cond.rhs_val, lower_val, col_meta.type) > 0) {
                            lower_val = cond.rhs_val;
                            has_lower = true;
                            lower_inclusive = false;
                        }
                        break;
                    case OP_GE:
                        // 大于等于：设置下界，包含
                        if (!has_lower || compare_values(cond.rhs_val, lower_val, col_meta.type) > 0) {
                            lower_val = cond.rhs_val;
                            has_lower = true;
                            lower_inclusive = true;
                        }
                        break;
                    case OP_LT:
                        // 小于：设置上界，不包含
                        if (!has_upper || compare_values(cond.rhs_val, upper_val, col_meta.type) < 0) {
                            upper_val = cond.rhs_val;
                            has_upper = true;
                            upper_inclusive = false;
                        }
                        break;
                    case OP_LE:
                        // 小于等于：设置上界，包含
                        if (!has_upper || compare_values(cond.rhs_val, upper_val, col_meta.type) < 0) {
                            upper_val = cond.rhs_val;
                            has_upper = true;
                            upper_inclusive = true;
                        }
                        break;
                }
            }

            // 构建索引键并获取扫描范围
            if (has_lower || has_upper) {
                auto lower_key = std::make_unique<char[]>(index_meta_.col_tot_len);
                auto upper_key = std::make_unique<char[]>(index_meta_.col_tot_len);

                // 初始化键值
                memset(lower_key.get(), 0, index_meta_.col_tot_len);
                memset(upper_key.get(), 0, index_meta_.col_tot_len);

                if (has_lower) {
                    memcpy(lower_key.get(), lower_val.raw->data, col_meta.len);
                    if (lower_inclusive) {
                        lower_iid = ih->lower_bound(lower_key.get());
                    } else {
                        lower_iid = ih->upper_bound(lower_key.get());
                    }
                } else {
                    lower_iid = ih->leaf_begin();
                }

                if (has_upper) {
                    memcpy(upper_key.get(), upper_val.raw->data, col_meta.len);
                    if (upper_inclusive) {
                        upper_iid = ih->upper_bound(upper_key.get());
                    } else {
                        upper_iid = ih->lower_bound(upper_key.get());
                    }
                } else {
                    upper_iid = ih->leaf_end();
                }

                has_range = true;
            }
        }

        // 如果没有可用的索引条件，回退到全索引扫描
        if (!has_range) {
            lower_iid = ih->leaf_begin();
            upper_iid = ih->leaf_end();
        }

        // 创建索引扫描器
        scan_ = std::make_unique<IxScan>(ih, lower_iid, upper_iid, sm_manager_->get_bpm());

        // 移动到第一个满足所有条件的记录
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conds(cols_, fed_conds_, rec.get())) {
                return;  // 找到第一个满足条件的记录
            }
            scan_->next();
        }
    }

   private:
    /**
     * @brief 比较两个值的大小
     * @param val1 第一个值
     * @param val2 第二个值
     * @param type 值的类型
     * @return 负数表示val1<val2，0表示相等，正数表示val1>val2
     */
    int compare_values(const Value& val1, const Value& val2, ColType type) {
        switch (type) {
            case TYPE_INT:
                return *(int*)val1.raw->data - *(int*)val2.raw->data;
            case TYPE_FLOAT: {
                float f1 = *(float*)val1.raw->data;
                float f2 = *(float*)val2.raw->data;
                if (f1 < f2) return -1;
                if (f1 > f2) return 1;
                return 0;
            }
            case TYPE_STRING: {
                // 使用更安全的字符串比较
                size_t len1 = strnlen((char*)val1.raw->data, val1.raw->size);
                size_t len2 = strnlen((char*)val2.raw->data, val2.raw->size);
                size_t min_len = std::min(len1, len2);

                int result = memcmp(val1.raw->data, val2.raw->data, min_len);
                if (result != 0) return result;

                // 如果前面的字符都相同，比较长度
                if (len1 < len2) return -1;
                if (len1 > len2) return 1;
                return 0;
            }
            default:
                throw InternalError("Unsupported column type for comparison");
        }
    }

   public:
    /**
     * @brief 移动到下一个满足条件的记录
     *
     * 在索引扫描过程中，继续向前移动扫描器，
     * 直到找到下一个满足所有条件的记录。
     */
    void nextTuple() override {
        if (scan_ == nullptr) {
            throw InternalError("Index scan not initialized at " + getType());
        }

        if (!scan_->is_end()) {
            scan_->next();
        }

        // 继续寻找下一个满足条件的记录
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conds(cols_, fed_conds_, rec.get())) {
                return;  // 找到满足条件的记录
            }
            scan_->next();
        }
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
        // 自动初始化扫描器（如果还未初始化）
        if (scan_ == nullptr) {
            beginTuple();
        }

        // 检查是否已经到达扫描末尾
        if (is_end()) {
            return nullptr;
        }

        // 获取当前记录
        auto record = fh_->get_record(rid_, context_);

        return record;
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

   private:
    /**
     * @brief 初始化条件值的raw数据
     * 处理类型不匹配的情况，确保条件值能够正确比较
     */
    void init_condition_values() {
        for (auto& cond : conds_) {
            if (cond.is_rhs_val) {
                // 找到左侧列的元数据
                auto lhs_col = get_col(cols_, cond.lhs_col);

                // 如果类型不匹配但可以转换，进行类型转换
                if (cond.rhs_val.type != lhs_col->type) {
                    if (can_cast_type(cond.rhs_val.type, lhs_col->type)) {
                        cast_condition_value(cond.rhs_val, lhs_col->type);
                    }
                }

                // 初始化raw数据
                if (cond.rhs_val.raw == nullptr) {
                    cond.rhs_val.init_raw(lhs_col->len);
                }
            }
        }
    }

    /**
     * @brief 检查是否可以进行类型转换
     */
    bool can_cast_type(ColType from, ColType to) {
        if (from == to) return true;
        if (from == TYPE_INT && to == TYPE_FLOAT) return true;
        if (from == TYPE_FLOAT && to == TYPE_INT) return true;
        return false;
    }

    /**
     * @brief 转换条件值的类型
     */
    void cast_condition_value(Value& val, ColType to) {
        if (val.type == TYPE_INT && to == TYPE_FLOAT) {
            int int_val = val.int_val;
            val.type = TYPE_FLOAT;
            val.float_val = static_cast<float>(int_val);
        } else if (val.type == TYPE_FLOAT && to == TYPE_INT) {
            float float_val = val.float_val;
            val.type = TYPE_INT;
            val.int_val = static_cast<int>(float_val);
        }
        // 如果类型相同或无法转换，保持原样
    }
};