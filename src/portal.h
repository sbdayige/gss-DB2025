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
#include <iostream>
#include <string>

#include "common/common.h"
#include "execution/execution_common.h"
#include "execution/execution_sort.h"
#include "execution/executor_abstract.h"
#include "execution/executor_aggregate.h"
#include "execution/executor_explain.h"
#include "execution/executor_filter.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_projection.h"

// MVCC执行器头文件
#include "execution/executor_mvcc_delete.h"
#include "execution/executor_mvcc_index_scan.h"
#include "execution/executor_mvcc_insert.h"
#include "execution/executor_mvcc_select.h"
#include "execution/executor_mvcc_update.h"

extern SmManager sm_manager;

/**
 * @brief 门户类型枚举
 *
 * 定义了不同类型的查询门户，用于区分和分发不同类型的SQL语句执行。
 * 每种门户类型对应不同的执行路径和处理方式。
 */
typedef enum portalTag {
    PORTAL_Invalid_Query = 0,   ///< 无效查询：语法错误或不支持的查询类型
    PORTAL_ONE_SELECT,          ///< 单个SELECT查询：返回结果集的查询语句
    PORTAL_DML_WITHOUT_SELECT,  ///< 非SELECT的DML语句：INSERT/UPDATE/DELETE操作
    PORTAL_MULTI_QUERY,         ///< 多语句查询：DDL语句或批量操作
    PORTAL_CMD_UTILITY          ///< 实用工具命令：SHOW、DESC、SET等管理命令
} portalTag;

/**
 * @brief 门户语句结构体
 *
 * 封装了一个完整的查询执行所需的所有信息，包括门户类型、选择列、
 * 执行器树根节点和原始执行计划。这是Portal系统处理的基本单元。
 */
struct PortalStmt {
    portalTag tag;  ///< 门户类型标识

    std::vector<TabCol> sel_cols;            ///< SELECT语句的投影列列表
    std::unique_ptr<AbstractExecutor> root;  ///< 执行器树的根节点
    std::shared_ptr<Plan> plan;              ///< 原始执行计划（用于某些特殊处理）

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
class Portal {
   public:
    Portal() = default;
    ~Portal() = default;

    std::shared_ptr<PortalStmt> start(std::shared_ptr<Plan> plan, Context* context) {
        // ===========================================
        // 计划类型识别与分发
        // ===========================================
        // 这里可以将select进行拆分，例如：一个select，带有return的select等
        if (auto x = std::dynamic_pointer_cast<OtherPlan>(plan)) {
            // 其他类型计划：通常为系统管理命令
            return std::make_shared<PortalStmt>(PORTAL_CMD_UTILITY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(), plan);
        } else if (auto x = std::dynamic_pointer_cast<SetKnobPlan>(plan)) {
            // 系统参数设置计划：SET命令
            return std::make_shared<PortalStmt>(PORTAL_CMD_UTILITY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(), plan);
        } else if (auto x = std::dynamic_pointer_cast<DDLPlan>(plan)) {
            // 数据定义语言计划：CREATE/DROP TABLE/INDEX等
            return std::make_shared<PortalStmt>(PORTAL_MULTI_QUERY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(), plan);
        } else if (auto x = std::dynamic_pointer_cast<DMLPlan>(plan)) {
            switch (x->tag) {
                case T_Explain: {
                    // EXPLAIN语句处理：直接使用命令工具模式，避免包装成SELECT
                    return std::make_shared<PortalStmt>(PORTAL_CMD_UTILITY, std::vector<TabCol>(), std::unique_ptr<AbstractExecutor>(), plan);
                }

                case T_Select: {
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

                case T_Update: {
                    // 使用 convert_plan_executor 来优化性能，避免全表扫描
                    std::unique_ptr<AbstractExecutor> scan = convert_plan_executor(x->subplan_, context);

                    std::vector<Rid> rids;

                    scan->beginTuple();
                    while (!scan->is_end()) {
                        auto rid = scan->rid();
                        rids.push_back(rid);
                        scan->nextTuple();
                    }

                    std::unique_ptr<AbstractExecutor> root = std::make_unique<MvccUpdateExecutor>(
                        x->tab_name_, x->set_clauses_, x->conds_, rids, context);
                    return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
                }
                case T_Delete: {
                    // 使用 convert_plan_executor 来优化性能，避免全表扫描
                    std::unique_ptr<AbstractExecutor> scan = convert_plan_executor(x->subplan_, context);
                    std::vector<Rid> rids;

                    // 执行扫描计划收集匹配的记录ID
                    scan->beginTuple();
                    while (!scan->is_end()) {
                        auto rid = scan->rid();
                        rids.push_back(rid);
                        scan->nextTuple();
                    }
                    std::unique_ptr<AbstractExecutor> root =
                        std::make_unique<MvccDeleteExecutor>(x->tab_name_, x->conds_, rids, context);
                    return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
                }

                case T_Insert: {
                    std::unique_ptr<AbstractExecutor> root =
                        std::make_unique<MvccInsertExecutor>(x->tab_name_, x->values_, context);

                    return std::make_shared<PortalStmt>(PORTAL_DML_WITHOUT_SELECT, std::vector<TabCol>(), std::move(root), plan);
                }

                default:
                    throw InternalError("Unexpected field type");
                    break;
            }
        } else {
            throw InternalError("Unexpected field type");
        }
        return nullptr;
    }
    void run(std::shared_ptr<PortalStmt> portal, QlManager* ql, txn_id_t* txn_id, Context* context) {
        switch (portal->tag) {
            case PORTAL_ONE_SELECT: {
                int limit = 0;
                if (auto dml_plan = std::dynamic_pointer_cast<DMLPlan>(portal->plan)) {
                    limit = dml_plan->limit_;
                }
                ql->select_from(std::move(portal->root), std::move(portal->sel_cols), context, limit);
                break;
            }

            case PORTAL_DML_WITHOUT_SELECT: {
                ql->run_dml(std::move(portal->root));
                break;
            }
            case PORTAL_MULTI_QUERY: {
                ql->run_mutli_query(portal->plan, context);
                break;
            }
            case PORTAL_CMD_UTILITY: {
                ql->run_cmd_utility(portal->plan, txn_id, context);
                break;
            }
            default: {
                throw InternalError("Unexpected field type");
            }
        }
    }
    void drop() {
    }
    std::unique_ptr<AbstractExecutor> convert_plan_executor(std::shared_ptr<Plan> plan, Context* context) {
        if (auto x = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
            return std::make_unique<ProjectionExecutor>(convert_plan_executor(x->subplan_, context),
                                                        x->sel_cols_);
        } else if (auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
            if (x->tag == T_SeqScan) {
                auto executor = std::make_unique<MvccSelectExecutor>(x->tab_name_, x->conds_, x->cols_, context);
                return executor;
            } else {
                std::string index_name = sm_manager.get_ix_manager()->get_index_name(x->tab_name_, x->index_col_names_);
                auto executor = std::make_unique<MvccIndexScanExecutor>(x->tab_name_, x->conds_, x->cols_, index_name, context);
                return executor;
            }
        } else if (auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
            std::unique_ptr<AbstractExecutor> left = convert_plan_executor(x->left_, context);
            std::unique_ptr<AbstractExecutor> right = convert_plan_executor(x->right_, context);
            std::unique_ptr<AbstractExecutor> join = std::make_unique<NestedLoopJoinExecutor>(
                std::move(left),
                std::move(right), std::move(x->conds_), x->type);  // 传递JOIN类型
            return join;
        } else if (auto x = std::dynamic_pointer_cast<FilterPlan>(plan)) {
            return std::make_unique<FilterExecutor>(convert_plan_executor(x->subplan_, context),
                                                    x->conds_);
        } else if (auto x = std::dynamic_pointer_cast<SortPlan>(plan)) {
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
        } else if (auto x = std::dynamic_pointer_cast<AggregatePlan>(plan)) {
            auto subplan_executor = convert_plan_executor(x->subplan_, context);
            return std::make_unique<AggregateExecutor>(std::move(subplan_executor), x->agg_exprs_, x->group_by_cols_, x->having_conds_);
        }
        return nullptr;
    }
};