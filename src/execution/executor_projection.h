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

class ProjectionExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;  // 投影节点的儿子节点
    std::vector<ColMeta> cols_;               // 需要投影的字段
    size_t len_;                              // 字段总长度
    std::vector<size_t> sel_idxs_;            // 选中列在原始记录中的索引位置

   public:
    /**
     * @brief ProjectionExecutor构造函数
     *
     * @param prev 子执行器，提供原始数据源
     * @param sel_cols 需要投影的列信息
     *
     * 构造函数执行以下工作：
     * 1. 接收子执行器和选择的列信息
     * 2. 建立列索引映射关系
     * 3. 重新计算投影后的列偏移量和总长度
     */
    ProjectionExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<TabCol> &sel_cols) {
        prev_ = std::move(prev);

        size_t curr_offset = 0;
        auto &prev_cols = prev_->cols();
        for (auto &sel_col : sel_cols) {
            auto pos = get_col(prev_cols, sel_col);
            sel_idxs_.push_back(pos - prev_cols.begin());
            auto col = *pos;
            col.offset = curr_offset;
            curr_offset += col.len;
            cols_.push_back(col);
        }
        len_ = curr_offset;
    }

    /**
     * @brief 开始元组遍历
     * 委托给子执行器处理
     */
    void beginTuple() override {
        prev_->beginTuple();
    }

    /**
     * @brief 移动到下一个元组
     * 委托给子执行器处理
     */
    void nextTuple() override {
        prev_->nextTuple();
    }

    /**
     * @brief 检查是否到达结束位置
     * @return bool 如果子执行器结束则返回true
     */
    bool is_end() const override {
        return prev_->is_end();
    }

    /**
     * @brief 获取投影后的元组长度
     * @return size_t 投影后记录的总长度
     */
    size_t tupleLen() const override {
        return len_;
    }

    /**
     * @brief 获取投影后的列元数据
     * @return const std::vector<ColMeta>& 投影后的列信息
     */
    const std::vector<ColMeta> &cols() const override {
        return cols_;
    }

    /**
     * @brief 获取下一条投影后的记录
     *
     * @return std::unique_ptr<RmRecord> 投影后的记录，如果没有更多记录则返回nullptr
     *
     * 执行流程：
     * 1. 从子执行器获取下一条原始记录
     * 2. 根据选择的列索引提取对应数据
     * 3. 重新组织数据生成投影后的记录
     */
    std::unique_ptr<RmRecord> Next() override {
        // 从子执行器获取下一条记录
        auto prev_record = prev_->Next();
        if (prev_record == nullptr) {
            return nullptr;  // 子执行器已经结束
        }

        // 创建投影后的记录缓冲区
        auto projected_record = std::make_unique<RmRecord>(len_);

        // 根据选择的列索引提取数据并重新组织
        auto &prev_cols = prev_->cols();
        for (size_t i = 0; i < sel_idxs_.size(); ++i) {
            size_t sel_idx = sel_idxs_[i];              // 原始记录中的列索引
            const auto &prev_col = prev_cols[sel_idx];  // 原始列的元数据
            const auto &proj_col = cols_[i];            // 投影后列的元数据

            // 从原始记录中复制对应列的数据到投影记录中
            memcpy(projected_record->data + proj_col.offset,  // 目标位置
                   prev_record->data + prev_col.offset,       // 源位置
                   prev_col.len);                             // 复制长度
        }

        return projected_record;
    }

    /**
     * @brief 获取当前记录的位置标识符
     * @return Rid& 委托给子执行器的rid
     */
    Rid &rid() override {
        return prev_->rid();
    }

    /**
     * @brief 获取执行器类型名称
     * @return std::string 返回执行器类型标识
     */
    std::string getType() override {
        return "ProjectionExecutor";
    }
};