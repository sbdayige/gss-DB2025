/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

/**
 * @file portal.h
 * @brief 查询执行门户系统 - 执行计划到执行器的转换桥梁
 *
 * Portal系统是RMDB查询执行的核心组件，负责将优化器生成的执行计划转换为
 * 可执行的算子树，并管理整个查询执行过程。它提供了计划到执行器的统一转换
 * 接口，支持各种类型的SQL语句执行。
 *
 * 主要功能：
 * - 执行计划到执行器的转换
 * - 查询执行流程管理
 * - 不同类型SQL语句的分发处理
 * - 资源管理和清理
 */

#pragma once

#include <cerrno>
#include <cstring>
#include <string>
#include "optimizer/plan.h"
#include "execution/executor_abstract.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_projection.h"
#include "execution/executor_seq_scan.h"
#include "execution/executor_index_scan.h"
#include "execution/executor_update.h"
#include "execution/executor_insert.h"
#include "execution/executor_delete.h"
#include "execution/executor_explain.h"
#include "execution/execution_sort.h"
#include "execution/executor_aggregate.h"
#include "common/common.h"

/**
 * @brief 门户类型枚举
 *
 * 定义了不同类型的查询门户，用于区分和分发不同类型的SQL语句执行。
 * 每种门户类型对应不同的执行路径和处理方式。
 */
typedef enum portalTag
{
    PORTAL_Invalid_Query = 0,  ///< 无效查询：语法错误或不支持的查询类型
    PORTAL_ONE_SELECT,         ///< 单个SELECT查询：返回结果集的查询语句
    PORTAL_DML_WITHOUT_SELECT, ///< 非SELECT的DML语句：INSERT/UPDATE/DELETE操作
    PORTAL_MULTI_QUERY,        ///< 多语句查询：DDL语句或批量操作
    PORTAL_CMD_UTILITY         ///< 实用工具命令：SHOW、DESC、SET等管理命令
} portalTag;

/**
 * @brief 门户语句结构体
 *
 * 封装了一个完整的查询执行所需的所有信息，包括门户类型、选择列、
 * 执行器树根节点和原始执行计划。这是Portal系统处理的基本单元。
 */
struct PortalStmt
{
    portalTag tag; ///< 门户类型标识

    std::vector<TabCol> sel_cols;           ///< SELECT语句的投影列列表
    std::unique_ptr<AbstractExecutor> root; ///< 执行器树的根节点
    std::shared_ptr<Plan> plan;             ///< 原始执行计划（用于某些特殊处理）

    /**
     * @brief 构造函数
     * @param tag_ 门户类型
     * @param sel_cols_ 选择列列表
     * @param root_ 执行器根节点
     * @param plan_ 原始执行计划
     */
    PortalStmt(portalTag tag_, std::vector<TabCol> sel_cols_, std::unique_ptr<AbstractExecutor> root_, std::shared_ptr<Plan> plan_) : tag(tag_), sel_cols(std::move(sel_cols_)), root(std::move(root_)), plan(std::move(plan_)) {}
};

/**
 * @class Portal
 * @brief 查询执行门户管理器
 *
 * Portal类是RMDB查询执行系统的核心组件，负责将优化器生成的执行计划转换为
 * 具体的执行器树，并管理整个查询执行过程。它充当了查询规划器和查询执行器
 * 之间的桥梁角色。
 *
 * 主要职责：
 * 1. 执行计划类型识别和分发
 * 2. 执行计划到执行器的递归转换
 * 3. 查询执行流程的统一管理
 * 4. 资源分配和生命周期管理
 *
 * 工作流程：
 * 1. start() - 将执行计划转换为PortalStmt
 * 2. run() - 执行PortalStmt并获取结果
 * 3. drop() - 清理资源
 */
class Portal
{
private:
    SmManager *sm_manager_; ///< 系统管理器指针，用于访问表、索引等元数据

public:
    /**
     * @brief 构造函数
     * @param sm_manager 系统管理器指针
     */
    Portal(SmManager *sm_manager) : sm_manager_(sm_manager) {}
    ~Portal() {}

    /**
     * @brief 启动查询执行 - 将执行计划转换为门户语句
     *
     * 这是Portal系统的核心入口函数，负责将优化器生成的执行计划转换为
     * 可执行的门户语句。通过模式匹配识别不同类型的执行计划，并调用
     * 相应的转换逻辑。
     *
     * @param plan 优化器生成的执行计划
     * @param context 执行上下文，包含事务、锁等信息
     * @return std::shared_ptr<PortalStmt> 转换后的门户语句
     *
     * @details 支持的计划类型：
     *          - OtherPlan: 其他类型计划
     *          - SetKnobPlan: 系统参数设置计划
     *          - DDLPlan: 数据定义语言计划
     *          - DMLPlan: 数据操纵语言计划
     */
    std::shared_ptr<PortalStmt> start(std::shared_ptr<Plan> plan, Context *context)
    {
        // ===========================================
        // 计划类型识别与分发
        // ===========================================

        // 这里可以将select进行拆分，例如：一个select，带有return的select等
        if (auto x = std::dynamic_pointer_cast<OtherPlan>(plan))
        {
            // 其他类型计划：通常为系统管理命令
            return std::make_shared<PortalStmt>(PORTAL_CMD_UTILITY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(), plan);
        }
        else if (auto x = std::dynamic_pointer_cast<SetKnobPlan>(plan))
        {
            // 系统参数设置计划：SET命令
            return std::make_shared<PortalStmt>(PORTAL_CMD_UTILITY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(), plan);
        }
        else if (auto x = std::dynamic_pointer_cast<DDLPlan>(plan))
        {
            // 数据定义语言计划：CREATE/DROP TABLE/INDEX等
            return std::make_shared<PortalStmt>(PORTAL_MULTI_QUERY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(), plan);
        }
        else if (auto x = std::dynamic_pointer_cast<DMLPlan>(plan))
        {
            // ===========================================
            // DML计划处理：根据操作类型进一步分发
            // ===========================================

            switch (x->tag)
            {
            case T_Explain:
            {
                // EXPLAIN语句处理：直接使用命令工具模式，避免包装成SELECT
                return std::make_shared<PortalStmt>(PORTAL_CMD_UTILITY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(), plan);
            }

            case T_Select:
            {
                // 递归转换子计划为执行器树
                std::unique_ptr<AbstractExecutor> root = convert_plan_executor(x->subplan_, context);

                // 检查是否是投影计划，如果是则获取投影列，否则使用执行器的列信息
                std::vector<TabCol> sel_cols;
                if (auto p = std::dynamic_pointer_cast<ProjectionPlan>(x->subplan_)) {
                    sel_cols = std::move(p->sel_cols_);
                } else {
                    // 对于非投影计划（如聚合计划），从执行器中获取列信息
                    if (root) {
                        const auto& exec_cols = root->cols();
                        for (const auto& col : exec_cols) {
                            sel_cols.push_back({col.tab_name, col.name});
                        }
                    }
                }

                // 创建SELECT门户语句，包含投影列和执行器树
                return std::make_shared<PortalStmt>(PORTAL_ONE_SELECT, std::move(sel_cols), std::move(root), plan);
            }

            case T_Update:
            {
                // ===============================
                // UPDATE语句处理
                // ===============================

                // 第一步：执行扫描计划，收集需要更新的记录ID
                std::unique_ptr<AbstractExecutor> scan = convert_plan_executor(x->subplan_, context);
                std::vector<Rid> rids;

                // 遍历扫描结果，收集所有符合条件的记录ID
                for (scan->beginTuple(); !scan->is_end(); scan->nextTuple())
                {
                    rids.push_back(scan->rid());
                }

                // 第二步：创建UPDATE执行器，传入收集到的记录ID
                std::unique_ptr<AbstractExecutor> root = std::make_unique<UpdateExecutor>(sm_manager_,
                                                                                          x->tab_name_, x->set_clauses_, x->conds_, rids, context);

                return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
            }
            case T_Delete:
            {
                // ===============================
                // DELETE语句处理
                // ===============================

                // 第一步：执行扫描计划，收集需要删除的记录ID
                std::unique_ptr<AbstractExecutor> scan = convert_plan_executor(x->subplan_, context);
                std::vector<Rid> rids;

                // 遍历扫描结果，收集所有符合条件的记录ID
                for (scan->beginTuple(); !scan->is_end(); scan->nextTuple())
                {
                    rids.push_back(scan->rid());
                }

                // 第二步：创建DELETE执行器，传入收集到的记录ID
                std::unique_ptr<AbstractExecutor> root =
                    std::make_unique<DeleteExecutor>(sm_manager_, x->tab_name_, x->conds_, rids, context);

                return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
            }

            case T_Insert:
            {
                // ===============================
                // INSERT语句处理
                // ===============================

                // INSERT操作相对简单，直接创建插入执行器
                // 不需要预先扫描，直接插入指定的值
                std::unique_ptr<AbstractExecutor> root =
                    std::make_unique<InsertExecutor>(sm_manager_, x->tab_name_, x->values_, context);

                return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
            }

            default:
                throw InternalError("Unexpected field type");
                break;
            }
        }
        else
        {
            throw InternalError("Unexpected field type");
        }
        return nullptr;
    }

    /**
     * @brief 执行门户语句 - 遍历算子树并生成执行结果
     *
     * 根据门户类型分发到相应的执行路径，统一管理不同类型SQL语句的执行过程。
     * 这是Portal系统执行阶段的核心函数，负责将准备好的执行器树真正执行。
     *
     * @param portal 准备好的门户语句，包含执行器树和元数据
     * @param ql 查询语言管理器，负责实际的查询执行
     * @param txn_id 事务ID指针，用于事务管理
     * @param context 执行上下文，包含锁、缓冲区等资源
     *
     * @details 执行分发策略：
     *          - PORTAL_ONE_SELECT: 单SELECT查询，返回结果集
     *          - PORTAL_DML_WITHOUT_SELECT: DML操作，返回影响行数
     *          - PORTAL_MULTI_QUERY: DDL操作，执行结构变更
     *          - PORTAL_CMD_UTILITY: 工具命令，执行管理操作
     */
    void run(std::shared_ptr<PortalStmt> portal, QlManager *ql, txn_id_t *txn_id, Context *context)
    {
        // ===========================================
        // 门户类型分发执行
        // ===========================================

        switch (portal->tag)
        {
        case PORTAL_ONE_SELECT:
        {
            // SELECT查询执行：遍历执行器树，输出结果集
            // 使用投影列信息格式化输出
            // 获取DML计划中的limit值
            int limit = 0;
            if (auto dml_plan = std::dynamic_pointer_cast<DMLPlan>(portal->plan)) {
                limit = dml_plan->limit_;
            }
            ql->select_from(std::move(portal->root), std::move(portal->sel_cols), context, limit);
            break;
        }

        case PORTAL_DML_WITHOUT_SELECT:
        {
            // DML操作执行：INSERT/UPDATE/DELETE
            // 执行数据修改操作，返回影响的行数
            ql->run_dml(std::move(portal->root));
            break;
        }
        case PORTAL_MULTI_QUERY:
        {
            // DDL操作执行：CREATE/DROP TABLE/INDEX
            // 直接使用原始计划，因为DDL不需要复杂的执行器树
            ql->run_mutli_query(portal->plan, context);
            break;
        }
        case PORTAL_CMD_UTILITY:
        {
            // 工具命令执行：SHOW、DESC、SET等
            // 传入事务ID用于某些需要事务上下文的命令
            ql->run_cmd_utility(portal->plan, txn_id, context);
            break;
        }
        default:
        {
            throw InternalError("Unexpected field type");
        }
        }
    }

    /**
     * @brief 清理门户资源
     *
     * 负责清理Portal执行过程中分配的资源。当前实现为空，
     * 因为使用了智能指针进行自动内存管理。
     *
     * @details 资源清理策略：
     *          - 执行器树：通过unique_ptr自动释放
     *          - 门户语句：通过shared_ptr自动释放
     *          - 上下文资源：由调用者负责管理
     *
     * @note 未来可能需要添加：
     *       - 临时文件清理
     *       - 锁资源释放
     *       - 缓存清理
     */
    void drop()
    {
        // 当前使用智能指针自动管理内存，无需手动清理
        // 未来可能需要添加特定资源的清理逻辑
    }

    /**
     * @brief 递归转换执行计划为执行器 - 执行计划到执行器的核心转换函数
     *
     * 这是Portal系统最核心的转换函数，负责将优化器生成的逻辑执行计划
     * 递归转换为具体的执行器对象树。采用访问者模式，通过动态类型转换
     * 识别计划类型，并创建相应的执行器。
     *
     * @param plan 待转换的执行计划节点
     * @param context 执行上下文，传递给执行器使用
     * @return std::unique_ptr<AbstractExecutor> 转换后的执行器对象
     *
     * @details 支持的计划类型转换：
     *          - ProjectionPlan → ProjectionExecutor: 投影算子
     *          - ScanPlan → SeqScanExecutor/IndexScanExecutor: 扫描算子
     *          - JoinPlan → NestedLoopJoinExecutor: 连接算子
     *          - SortPlan → SortExecutor: 排序算子
     *
     * @note 采用递归下降策略：
     *       1. 先转换子计划为子执行器
     *       2. 再创建当前层级的执行器
     *       3. 建立执行器之间的父子关系
     */
    std::unique_ptr<AbstractExecutor> convert_plan_executor(std::shared_ptr<Plan> plan, Context *context)
    {
        // ===========================================
        // 计划类型识别与执行器创建
        // ===========================================

        if (auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan))
        {
            // ===============================
            // 投影计划转换
            // ===============================
            // 递归转换子计划，然后创建投影执行器包装子执行器
            return std::make_unique<ProjectionExecutor>(convert_plan_executor(x->subplan_, context),
                                                        x->sel_cols_);
        }
        else if (auto x = std::dynamic_pointer_cast<ScanPlan>(plan))
        {
            // ===============================
            // 扫描计划转换
            // ===============================
            if (x->tag == T_SeqScan)
            {
                // 顺序扫描：遍历表中所有记录
                return std::make_unique<SeqScanExecutor>(sm_manager_, x->tab_name_, x->conds_, context);
            }
            else
            {
                // 索引扫描：利用索引快速定位记录
                return std::make_unique<IndexScanExecutor>(sm_manager_, x->tab_name_, x->conds_, x->index_col_names_, context);
            }
        }
        else if (auto x = std::dynamic_pointer_cast<JoinPlan>(plan))
        {
            // ===============================
            // 连接计划转换
            // ===============================
            // 递归转换左右子计划，创建嵌套循环连接执行器
            std::unique_ptr<AbstractExecutor> left = convert_plan_executor(x->left_, context);
            std::unique_ptr<AbstractExecutor> right = convert_plan_executor(x->right_, context);
            std::unique_ptr<AbstractExecutor> join = std::make_unique<NestedLoopJoinExecutor>(
                std::move(left),
                std::move(right), std::move(x->conds_));
            return join;
        }
        else if (auto x = std::dynamic_pointer_cast<SortPlan>(plan))
        {
            // ===============================
            // 排序计划转换
            // ===============================
            // 递归转换子计划，创建排序执行器
            if (x->is_multi_column()) {
                // 多列排序：构建列列表和排序方向列表
                std::vector<TabCol> sort_tab_cols;
                std::vector<bool> sort_directions;
                
                for (const auto& sort_col : x->sort_cols_) {
                    sort_tab_cols.push_back(sort_col.col);
                    sort_directions.push_back(sort_col.is_desc);
                }
                
                return std::make_unique<SortExecutor>(convert_plan_executor(x->subplan_, context),
                                                      sort_tab_cols, sort_directions);
            } else {
                // 单列排序：使用向后兼容的构造函数
                return std::make_unique<SortExecutor>(convert_plan_executor(x->subplan_, context),
                                                      x->sel_col_, x->is_desc_);
            }
        }
        else if (auto x = std::dynamic_pointer_cast<FilterPlan>(plan))
        {
            // ===============================
            // 过滤计划转换
            // ===============================
            // 检查子计划是否是ScanPlan，如果是则将过滤条件合并到扫描条件中
            if (auto scan_plan = std::dynamic_pointer_cast<ScanPlan>(x->subplan_))
            {
                // 合并FilterPlan的条件和ScanPlan原有的条件
                std::vector<Condition> merged_conds = scan_plan->conds_;
                merged_conds.insert(merged_conds.end(), x->conds_.begin(), x->conds_.end());
                
                // 创建SeqScanExecutor，传入合并后的条件
                if (scan_plan->tag == T_SeqScan)
                {
                    return std::make_unique<SeqScanExecutor>(sm_manager_, scan_plan->tab_name_, merged_conds, context);
                }
                else
                {
                    // 对于索引扫描，也传入合并后的条件
                    return std::make_unique<IndexScanExecutor>(sm_manager_, scan_plan->tab_name_, merged_conds, 
                                                              scan_plan->index_col_names_, context);
                }
            }
            else
            {
                // 对于非ScanPlan的子计划，递归转换子计划
                // 这里需要创建一个FilterExecutor，但由于没有实现，暂时直接返回子计划
                return convert_plan_executor(x->subplan_, context);
            }
        }
        else if (auto x = std::dynamic_pointer_cast<AggregatePlan>(plan))
        {
            // ===============================
            // 聚合计划转换
            // ===============================
            // 递归转换子计划，创建聚合执行器
            auto subplan_executor = convert_plan_executor(x->subplan_, context);
            
            // 创建聚合执行器，传递GROUP BY列信息和HAVING条件
            return std::make_unique<AggregateExecutor>(std::move(subplan_executor), x->agg_exprs_, x->group_by_cols_, x->having_conds_);
        }

        // 未识别的计划类型，返回空指针
        return nullptr;
    }
};