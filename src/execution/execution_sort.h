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
#include "index/ix.h"
#include "system/sm.h"

class SortExecutor : public AbstractExecutor
{
private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> cols_;                           // 支持多列排序
    std::vector<bool> is_desc_;                           // 每列的排序方向
    std::vector<std::unique_ptr<RmRecord>> sorted_tuples; // 存储排序后的元组
    size_t tuple_num;                                     // 元组总数
    size_t current_index;                                 // 当前访问的元组索引
    bool is_end_;                                         // 是否已结束

public:
    // 添加与Portal.h调用匹配的构造函数 - 处理单列排序
    SortExecutor(std::unique_ptr<AbstractExecutor> prev,
                 const TabCol &sel_col,
                 bool is_desc)
    {
        prev_ = std::move(prev);
        // 将单列转换为向量
        cols_.push_back(*prev_->get_col(prev_->cols(), sel_col));
        // 存储排序方向
        is_desc_.push_back(is_desc);
        tuple_num = 0;
        current_index = 0;
        is_end_ = false;
    }

    // 保持原有构造函数 - 处理多列排序
    SortExecutor(std::unique_ptr<AbstractExecutor> prev,
                 const std::vector<TabCol> &sel_cols,
                 std::vector<bool> is_desc)
    {
        prev_ = std::move(prev);
        // 获取排序列的元数据
        for (const auto &sel_col : sel_cols)
        {
            cols_.push_back(*prev_->get_col(prev_->cols(), sel_col));
        }
        is_desc_ = std::move(is_desc);
        tuple_num = 0;
        current_index = 0;
        is_end_ = false;
    }

    void beginTuple() override
    {
        // 收集所有元组
        sorted_tuples.clear();
        prev_->beginTuple();
        while (!prev_->is_end())
        {
            sorted_tuples.push_back(prev_->Next());
            prev_->nextTuple();
        }

        tuple_num = sorted_tuples.size();
        if (tuple_num == 0)
        {
            is_end_ = true;
            return;
        }

        // 排序元组（使用高效的std::sort）
        std::sort(sorted_tuples.begin(), sorted_tuples.end(),
                  [this](const std::unique_ptr<RmRecord> &a, const std::unique_ptr<RmRecord> &b)
                  {
                      return compare_tuples(a, b);
                  });

        current_index = 0;
        is_end_ = false;
    }

    void nextTuple() override
    {
        if (is_end_)
        {
            return;
        }

        current_index++;
        if (current_index >= tuple_num)
        {
            is_end_ = true;
        }
    }

    bool is_end() const override
    {
        return is_end_;
    }

    std::unique_ptr<RmRecord> Next() override
    {
        if (is_end_)
        {
            return nullptr;
        }

        return std::make_unique<RmRecord>(*sorted_tuples[current_index]);
    }

    // 多列比较函数
    bool compare_tuples(const std::unique_ptr<RmRecord> &a, const std::unique_ptr<RmRecord> &b)
    {
        for (size_t i = 0; i < cols_.size(); i++)
        {
            const auto &col = cols_[i];
            char *a_data = a->data + col.offset;
            char *b_data = b->data + col.offset;

            int compare_result = 0;

            if (col.type == TYPE_INT)
            {
                int a_val = *(int *)a_data;
                int b_val = *(int *)b_data;
                compare_result = (a_val == b_val) ? 0 : ((a_val < b_val) ? -1 : 1);
            }
            else if (col.type == TYPE_FLOAT)
            {
                float a_val = *(float *)a_data;
                float b_val = *(float *)b_data;
                compare_result = (a_val == b_val) ? 0 : ((a_val < b_val) ? -1 : 1);
            }
            else if (col.type == TYPE_STRING)
            {
                std::string a_str = std::string((char *)a_data, col.len);
                std::string b_str = std::string((char *)b_data, col.len);
                a_str.resize(strlen(a_str.c_str())); // 移除尾部空字符
                b_str.resize(strlen(b_str.c_str()));
                compare_result = a_str.compare(b_str);
            }

            // 如果此列值不同，则根据此列决定排序结果
            if (compare_result != 0)
            {
                // 根据排序方向调整比较结果
                return is_desc_[i] ? (compare_result > 0) : (compare_result < 0);
            }
            // 如果此列值相同，则继续比较下一列
        }

        // 所有列都相等
        return false;
    }

    const std::vector<ColMeta> &cols() const override
    {
        return prev_->cols();
    }

    size_t tupleLen() const override
    {
        return prev_->tupleLen();
    }

    std::string getType() override
    {
        return "SortExecutor";
    }

    Rid &rid() override { return _abstract_rid; }
};