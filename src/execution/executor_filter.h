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

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"

/**
 * @brief 过滤执行器类
 *
 * FilterExecutor是RMDB数据库系统中负责执行WHERE条件过滤的核心组件。
 * 它继承自AbstractExecutor，遵循火山模型的执行框架。
 *
 * 主要功能：
 * 1. 从子执行器获取记录
 * 2. 根据WHERE条件过滤记录
 * 3. 只返回满足所有条件的记录
 */
class FilterExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> child_;  // 子执行器，提供数据源
    std::vector<Condition> conds_;             // 过滤条件列表

   public:
    /**
     * @brief FilterExecutor构造函数
     *
     * @param child 子执行器，提供原始数据源
     * @param conds 过滤条件列表
     */
    FilterExecutor(std::unique_ptr<AbstractExecutor> child, std::vector<Condition> conds) {
        child_ = std::move(child);
        conds_ = std::move(conds);
    }

    /**
     * @brief 开始元组遍历
     */
    void beginTuple() override {
        child_->beginTuple();
        // 初始化条件值的raw数据
        init_condition_values();
        // 移动到第一个满足条件的记录
        while (!child_->is_end()) {
            auto rec = child_->Next();
            if (rec && eval_conds(child_->cols(), conds_, rec.get())) {
                return;  // 找到第一个满足条件的记录
            }
            child_->nextTuple();
        }
    }

    /**
     * @brief 移动到下一个满足条件的元组
     */
    void nextTuple() override {
        if (!child_->is_end()) {
            child_->nextTuple();
        }
        // 继续寻找下一个满足条件的记录
        while (!child_->is_end()) {
            auto rec = child_->Next();
            if (rec && eval_conds(child_->cols(), conds_, rec.get())) {
                return;  // 找到满足条件的记录
            }
            child_->nextTuple();
        }
    }

    /**
     * @brief 检查是否到达结束位置
     */
    bool is_end() const override {
        return child_->is_end();
    }

    /**
     * @brief 获取下一个满足条件的记录
     */
    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) {
            return nullptr;
        }

        return child_->Next();
    }

    /**
     * @brief 获取元组长度
     */
    size_t tupleLen() const override {
        return child_->tupleLen();
    }

    /**
     * @brief 获取列元数据
     */
    const std::vector<ColMeta>& cols() const override {
        return child_->cols();
    }

    /**
     * @brief 获取当前记录的位置标识符
     */
    Rid& rid() override {
        return child_->rid();
    }

   private:
    /**
     * @brief 初始化条件值的raw数据
     * 处理类型不匹配的情况，确保条件值能够正确比较
     */
    void init_condition_values() {
        const auto& rec_cols = child_->cols();
        for (auto& cond : conds_) {
            if (cond.is_rhs_val) {
                // 找到左侧列的元数据
                auto lhs_col = get_col(rec_cols, cond.lhs_col);

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

   public:
    /**
     * @brief 获取执行器类型名称
     */
    std::string getType() override {
        return "FilterExecutor";
    }
};
