/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "planner.h"

#include <algorithm>
#include <climits>
#include <memory>
#include <set>
#include <unordered_map>

#include "execution/executor_delete.h"
#include "execution/executor_explain.h"
#include "execution/executor_index_scan.h"
#include "execution/executor_insert.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_projection.h"
#include "execution/executor_seq_scan.h"
#include "execution/executor_update.h"
#include "index/ix.h"
#include "record/rm.h"
#include "record_printer.h"

// 索引匹配规则：支持等值查询和范围查询，匹配索引字段
bool Planner::get_index_cols(std::string tab_name, std::vector<Condition> curr_conds, std::vector<std::string> &index_col_names) {
    index_col_names.clear();
    std::set<std::string> indexed_columns;

    // 收集所有可以使用索引的列（等值查询和范围查询）
    for (auto &cond : curr_conds) {
        if (cond.is_rhs_val && cond.lhs_col.tab_name.compare(tab_name) == 0) {
            // 支持等值查询和范围查询操作符
            if (cond.op == OP_EQ || cond.op == OP_LT || cond.op == OP_GT ||
                cond.op == OP_LE || cond.op == OP_GE || cond.op == OP_NE) {
                indexed_columns.insert(cond.lhs_col.col_name);
            }
        }
    }

    // 将集合转换为向量
    for (const auto &col : indexed_columns) {
        index_col_names.push_back(col);
    }

    TabMeta &tab = sm_manager_->db_.get_table(tab_name);

    // 检查是否存在匹配的索引（单列索引或复合索引的前缀）
    if (!index_col_names.empty()) {
        // 优先查找单列索引
        for (const auto &col : index_col_names) {
            std::vector<std::string> single_col = {col};
            if (tab.is_index(single_col)) {
                index_col_names = single_col;
                return true;
            }
        }

        // 检查是否有复合索引可以使用
        if (tab.is_index(index_col_names)) {
            return true;
        }
    }

    return false;
}

/**
 * @brief 表算子条件谓词生成
 *
 * @param conds 条件
 * @param tab_names 表名
 * @return std::vector<Condition>
 */
std::vector<Condition> pop_conds(std::vector<Condition> &conds, std::string tab_names) {
    // auto has_tab = [&](const std::string &tab_name) {
    //     return std::find(tab_names.begin(), tab_names.end(), tab_name) != tab_names.end();
    // };
    std::vector<Condition> solved_conds;
    auto it = conds.begin();
    while (it != conds.end()) {
        if ((tab_names.compare(it->lhs_col.tab_name) == 0 && it->is_rhs_val) || (it->lhs_col.tab_name.compare(it->rhs_col.tab_name) == 0)) {
            solved_conds.emplace_back(std::move(*it));
            it = conds.erase(it);
        } else {
            it++;
        }
    }
    return solved_conds;
}

int push_conds(Condition *cond, std::shared_ptr<Plan> plan) {
    if (auto x = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        if (x->tab_name_.compare(cond->lhs_col.tab_name) == 0) {
            return 1;
        } else if (x->tab_name_.compare(cond->rhs_col.tab_name) == 0) {
            return 2;
        } else {
            return 0;
        }
    } else if (auto x = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        int left_res = push_conds(cond, x->left_);
        // 条件已经下推到左子节点
        if (left_res == 3) {
            return 3;
        }
        int right_res = push_conds(cond, x->right_);
        // 条件已经下推到右子节点
        if (right_res == 3) {
            return 3;
        }
        // 左子节点或右子节点有一个没有匹配到条件的列
        if (left_res == 0 || right_res == 0) {
            return left_res + right_res;
        }
        // 左子节点匹配到条件的右边
        if (left_res == 2) {
            // 需要将左右两边的条件变换位置
            std::map<CompOp, CompOp> swap_op = {
                {OP_EQ, OP_EQ},
                {OP_NE, OP_NE},
                {OP_LT, OP_GT},
                {OP_GT, OP_LT},
                {OP_LE, OP_GE},
                {OP_GE, OP_LE},
            };
            std::swap(cond->lhs_col, cond->rhs_col);
            cond->op = swap_op.at(cond->op);
        }
        x->conds_.emplace_back(std::move(*cond));
        return 3;
    }
    return false;
}

std::shared_ptr<Plan> pop_scan(int *scantbl, std::string table, std::vector<std::string> &joined_tables,
                               std::vector<std::shared_ptr<Plan>> plans) {
    for (size_t i = 0; i < plans.size(); i++) {
        auto x = std::dynamic_pointer_cast<ScanPlan>(plans[i]);
        if (x->tab_name_.compare(table) == 0) {
            scantbl[i] = 1;
            joined_tables.emplace_back(x->tab_name_);
            return plans[i];
        }
    }
    return nullptr;
}

std::shared_ptr<Query> Planner::logical_optimization(std::shared_ptr<Query> query, Context *context) {
    // 只对SELECT语句执行逻辑优化
    if (!std::dynamic_pointer_cast<ast::SelectStmt>(query->parse)) {
        return query;
    }

    // 1. 谓词下推 (Predicate Pushdown)
    query = predicate_pushdown(query);

    // 2. 投影下推 (Projection Pushdown)
    query = projection_pushdown(query);

    // 3. 连接顺序优化 (Join Order Optimization)
    query = join_order_optimization(query);

    return query;
}

std::shared_ptr<Plan> Planner::physical_optimization(std::shared_ptr<Query> query, Context *context) {
    // 第一步：构建基本的扫描和连接计划
    std::shared_ptr<Plan> plan = make_one_rel(query);

    // 第二步：应用投影下推 - 在计划树中插入Project节点
    plan = apply_projection_pushdown(plan, query);

    // 第三步：处理ORDER BY
    plan = generate_sort_plan(query, std::move(plan));
    return plan;
}

/**
 * @brief 多表连接查询计划生成函数
 *
 * 该函数是查询优化器的核心，负责将涉及多个表的SELECT查询转换为一个统一的执行计划树。
 * 实现了自底向上的连接枚举算法，包括表扫描计划生成、连接顺序优化和连接算法选择。
 *
 * @param query 包含查询信息的Query对象，包括表列表、条件列表等
 * @return std::shared_ptr<Plan> 返回完整的查询执行计划树
 */
std::shared_ptr<Plan> Planner::make_one_rel(std::shared_ptr<Query> query) {
    // 将抽象语法树转换为SelectStmt类型，获取查询的具体信息
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    std::vector<std::string> tables = query->tables;

    // ===========================================
    // 第一阶段：为每个表生成最优的扫描计划
    // ===========================================

    // 创建表扫描执行器数组，每个表对应一个扫描计划
    std::vector<std::shared_ptr<Plan>> table_scan_executors(tables.size());

    // 为每个表单独生成扫描计划，包括条件下推和访问路径选择
    for (size_t i = 0; i < tables.size(); i++) {
        // 从全局条件中提取当前表的单表条件用于索引选择
        std::vector<Condition> curr_conds;
        for (const auto &cond : query->conds) {
            if (cond.lhs_col.tab_name == tables[i] && cond.is_rhs_val) {
                curr_conds.push_back(cond);
            }
        }

        // 检查当前表是否有可用的索引来优化查询
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(tables[i], curr_conds, index_col_names);

        if (index_exist == false) {
            // 没有可用索引：使用顺序扫描（Sequential Scan）
            // 将WHERE条件传递给ScanPlan，让执行器处理Filter节点生成
            index_col_names.clear();
            table_scan_executors[i] =
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, tables[i], curr_conds, index_col_names);
        } else {
            // 有可用索引：使用索引扫描（Index Scan）
            // 将WHERE条件传递给ScanPlan，让执行器处理Filter节点生成
            table_scan_executors[i] =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, tables[i], curr_conds, index_col_names);
        }
    }

    // ===========================================
    // 特殊情况处理：单表查询优化
    // ===========================================

    // 如果只有一个表，无需进行连接操作，直接返回该表的扫描计划
    if (tables.size() == 1) {
        return table_scan_executors[0];
    }

    // ===========================================
    // 第二阶段：多表连接计划构建
    // ===========================================

    // 获取剩余的连接条件（单表条件已在第一阶段处理）
    // 将连接条件和WHERE条件分开处理
    std::vector<Condition> join_conds;
    for (const auto &cond : query->conds) {
        if (!cond.is_rhs_val) {  // 连接条件：左右都是列
            join_conds.push_back(cond);
        }
        // WHERE条件（is_rhs_val = true）留给apply_predicate_pushdown处理
    }

    auto conds = std::move(join_conds);  // 只处理连接条件
    std::shared_ptr<Plan> table_join_executors;

    // 创建表使用状态跟踪数组：-1表示未使用，1表示已使用
    // 用于避免重复使用同一个表的扫描计划
    int scantbl[tables.size()];
    for (size_t i = 0; i < tables.size(); i++) {
        scantbl[i] = -1;
    }

    if (conds.size() >= 1) {
        // ===========================================
        // 第2.1阶段：处理第一个连接条件，建立初始连接树
        // ===========================================

        // 已连接表名列表，用于跟踪哪些表已经参与了连接操作
        std::vector<std::string> joined_tables(tables.size());
        auto it = conds.begin();

        // 处理第一个连接条件：选择两个表进行连接，建立连接树的根节点
        while (it != conds.end()) {
            std::shared_ptr<Plan> left, right;

            // 从扫描计划列表中获取连接条件涉及的左表和右表
            // pop_scan会标记表为已使用，并更新joined_tables列表
            left = pop_scan(scantbl, it->lhs_col.tab_name, joined_tables, table_scan_executors);
            right = pop_scan(scantbl, it->rhs_col.tab_name, joined_tables, table_scan_executors);

            // 将当前条件封装为连接条件
            std::vector<Condition> join_conds{*it};

            // ===========================================
            // 连接算法选择策略
            // ===========================================
            // 根据系统配置选择最适合的连接算法

            if (enable_nestedloop_join && enable_sortmerge_join) {
                // 两种算法都启用时，默认选择嵌套循环连接
                // Nested Loop Join适用于小表连接或没有排序要求的场景
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left), std::move(right), join_conds);
            } else if (enable_nestedloop_join) {
                // 仅启用嵌套循环连接
                // 算法简单，适用于小数据集或内存受限的环境
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left), std::move(right), join_conds);
            } else if (enable_sortmerge_join) {
                // 仅启用排序合并连接
                // 适用于大数据集，特别是连接列已排序的情况
                table_join_executors = std::make_shared<JoinPlan>(T_SortMerge, std::move(left), std::move(right), join_conds);
            } else {
                // 配置错误：没有启用任何连接算法
                throw RMDBError("No join executor selected!");
            }

            // 移除已处理的连接条件，避免重复处理
            it = conds.erase(it);
            break;  // 只处理第一个条件，建立初始连接树
        }

        // ===========================================
        // 第2.2阶段：处理剩余连接条件，扩展连接树
        // ===========================================

        // 继续处理剩余的连接条件，将更多表加入到连接树中
        it = conds.begin();
        while (it != conds.end()) {
            // 初始化新表连接计划指针
            std::shared_ptr<Plan> left_need_to_join_executors = nullptr;
            std::shared_ptr<Plan> right_need_to_join_executors = nullptr;
            bool isneedreverse = false;  // 标记是否需要交换操作数

            // 检查连接条件的左表是否已在连接树中
            if (std::find(joined_tables.begin(), joined_tables.end(), it->lhs_col.tab_name) == joined_tables.end()) {
                // 左表未参与连接，从扫描计划中获取
                left_need_to_join_executors = pop_scan(scantbl, it->lhs_col.tab_name, joined_tables, table_scan_executors);
            }

            // 检查连接条件的右表是否已在连接树中
            if (std::find(joined_tables.begin(), joined_tables.end(), it->rhs_col.tab_name) == joined_tables.end()) {
                // 右表未参与连接，从扫描计划中获取
                right_need_to_join_executors = pop_scan(scantbl, it->rhs_col.tab_name, joined_tables, table_scan_executors);
                isneedreverse = true;  // 标记可能需要交换操作数以保持左深树结构
            }

            // ===========================================
            // 连接情况分类处理
            // ===========================================

            if (left_need_to_join_executors != nullptr && right_need_to_join_executors != nullptr) {
                // 情况1：两个新表之间的连接
                // 先将两个新表连接，然后将结果与已有连接树合并

                std::vector<Condition> join_conds{*it};

                // 创建新表之间的连接节点
                std::shared_ptr<Plan> temp_join_executors = std::make_shared<JoinPlan>(T_NestLoop,
                                                                                       std::move(left_need_to_join_executors),
                                                                                       std::move(right_need_to_join_executors),
                                                                                       join_conds);

                // 将新的连接节点与已有连接树合并（无额外连接条件，相当于笛卡尔积）
                // 修改：将已有连接树作为左子树，新连接节点作为右子树
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(table_join_executors),
                                                                  std::move(temp_join_executors),
                                                                  std::vector<Condition>());
            } else if (left_need_to_join_executors != nullptr || right_need_to_join_executors != nullptr) {
                // 情况2：一个新表与已有连接树的连接

                if (isneedreverse) {
                    // 需要交换操作数以维持左深连接树的结构
                    // 同时需要相应地调整比较操作符
                    std::map<CompOp, CompOp> swap_op = {
                        {OP_EQ, OP_EQ},  // 等于操作符交换后不变
                        {OP_NE, OP_NE},  // 不等于操作符交换后不变
                        {OP_LT, OP_GT},  // 小于 <-> 大于
                        {OP_GT, OP_LT},  // 大于 <-> 小于
                        {OP_LE, OP_GE},  // 小于等于 <-> 大于等于
                        {OP_GE, OP_LE},  // 大于等于 <-> 小于等于
                    };

                    // 交换连接条件的左右操作数
                    std::swap(it->lhs_col, it->rhs_col);
                    it->op = swap_op.at(it->op);

                    // 将右表计划移动到左表计划
                    left_need_to_join_executors = std::move(right_need_to_join_executors);
                }

                std::vector<Condition> join_conds{*it};

                // 将新表与已有连接树连接，构建左深连接树
                // 修改：将已有连接树作为左子树，新表作为右子树
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(table_join_executors),
                                                                  std::move(left_need_to_join_executors), join_conds);
            } else {
                // 情况3：连接条件涉及的两个表都已在连接树中
                // 执行条件下推优化，将条件推送到连接树的适当位置
                push_conds(std::move(&(*it)), table_join_executors);
            }

            // 移除已处理的连接条件
            it = conds.erase(it);
        }
    } else {
        // ===========================================
        // 无连接条件的情况处理
        // ===========================================

        // 如果没有连接条件，直接使用第一个表的扫描计划
        // 这种情况下通常是单表查询或需要笛卡尔积的多表查询
        table_join_executors = table_scan_executors[0];
        scantbl[0] = 1;  // 标记第一个表为已使用
    }

    // ===========================================
    // 第三阶段：处理剩余未连接的表
    // ===========================================

    // 检查是否还有表未参与连接，如果有则通过笛卡尔积方式加入
    // 这种情况通常出现在：
    // 1. 查询中缺少某些表的连接条件
    // 2. 故意的笛卡尔积查询
    // 3. 连接条件不完整的复杂查询
    for (size_t i = 0; i < tables.size(); i++) {
        if (scantbl[i] == -1)  // 表尚未被使用
        {
            // 通过笛卡尔积将剩余表加入连接树
            // 注意：这可能导致大量的中间结果，影响查询性能
            // 修改：将已有连接树作为左子树，新表作为右子树，以符合期望的输出顺序
            table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(table_join_executors),
                                                              std::move(table_scan_executors[i]), std::vector<Condition>());
        }
    }

    // 返回完整的查询执行计划树
    // 此时所有表都已被整合到一个统一的执行计划中
    return table_join_executors;
}

/**
 * @brief 排序计划生成函数
 *
 * 该函数负责处理SQL查询中的ORDER BY子句，在现有查询计划的基础上添加排序操作。
 * 它分析查询中的排序需求，定位排序列的元数据信息，然后生成相应的排序执行计划。
 *
 * @param query 包含查询信息的Query对象，包括解析后的AST和排序要求
 * @param plan 已经生成的基础查询计划（如连接计划、扫描计划等）
 * @return std::shared_ptr<Plan> 返回包含排序操作的完整执行计划
 *
 * @details 函数工作流程：
 *          1. 检查查询是否包含ORDER BY子句
 *          2. 收集所有涉及表的列元数据信息
 *          3. 定位排序列的具体信息（表名、列名、数据类型等）
 *          4. 创建排序计划节点，包装原有的查询计划
 *          5. 返回新的排序执行计划树
 */
std::shared_ptr<Plan> Planner::generate_sort_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan) {
    // ===========================================
    // 第一阶段：排序需求检测
    // ===========================================

    // 将抽象语法树转换为SelectStmt类型，以便访问ORDER BY信息
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);

    // 检查查询是否包含ORDER BY子句
    // 如果没有排序要求，直接返回原始计划，无需添加排序操作
    if (!x->has_sort) {
        return plan;
    }

    // ===========================================
    // 第二阶段：收集表列元数据信息
    // ===========================================

    // 获取查询涉及的所有表名
    std::vector<std::string> tables = query->tables;

    // 创建所有列的元数据集合，用于后续的排序列查找
    std::vector<ColMeta> all_cols;

    // 遍历查询中涉及的每个表，收集所有列的元数据
    for (auto &sel_tab_name : tables) {
        // 重要注意：这里db_不能写成get_db()，必须直接访问数据库对象
        // 因为需要传递指针而不是值，避免不必要的拷贝操作
        const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;

        // 将当前表的所有列元数据添加到总集合中
        // 这样可以支持跨表排序的情况（虽然通常排序列来自单个表）
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
    }

    // ===========================================
    // 第三阶段：定位排序列信息
    // ===========================================

    // 创建表列对象，用于存储找到的排序列的完整信息
    TabCol sel_col;

    // 在所有列元数据中查找与ORDER BY子句指定的列名匹配的列
    for (auto &col : all_cols) {
        // 比较列名（不区分大小写的字符串比较）
        if (col.name.compare(x->order->cols->col_name) == 0) {
            // 找到匹配的列，构建完整的表列信息
            // 包括表名和列名，这对于多表查询中的列唯一标识至关重要
            sel_col = {.tab_name = col.tab_name, .col_name = col.name};
            break;  // 找到后立即退出循环，避免重复处理
        }
    }

    // ===========================================
    // 第四阶段：生成排序执行计划
    // ===========================================

    // 创建排序计划节点，将排序操作包装在现有查询计划之上
    // 这遵循了查询处理的管道化模式：数据先经过基础计划（扫描、连接等），
    // 然后通过排序算子进行排序处理
    return std::make_shared<SortPlan>(
        T_Sort,                                     // 计划类型：排序操作
        std::move(plan),                            // 子计划：原始查询计划（数据源）
        sel_col,                                    // 排序列：包含表名和列名的完整标识
        x->order->orderby_dir == ast::OrderBy_DESC  // 排序方向：true为降序，false为升序
    );

    // 注意：SortPlan会在执行时接收来自子计划的数据流，
    // 对其进行排序后向上层计划提供有序的数据流
}

/**
 * @brief select plan 生成
 *
 * @param sel_cols select plan 选取的列
 * @param tab_names select plan 目标的表
 * @param conds select plan 选取条件
 */
std::shared_ptr<Plan> Planner::generate_select_plan(std::shared_ptr<Query> query, Context *context) {
    // 逻辑优化
    query = logical_optimization(std::move(query), context);

    // 物理优化

    std::shared_ptr<Plan> plannerRoot = physical_optimization(query, context);

    return plannerRoot;
}

/**
 * @brief 统一查询计划生成器 - SQL语句执行计划的总入口
 *
 * 该函数是RMDB查询优化器的顶层接口，负责将各种类型的SQL语句转换为相应的执行计划。
 * 通过模式匹配识别SQL语句类型，并为每种语句类型生成专门的执行计划树。
 * 支持DDL（数据定义语言）和DML（数据操纵语言）两大类SQL语句。
 *
 * @param query 包含解析后AST的Query对象，包含SQL语句的完整语法信息
 * @param context 查询执行上下文，包含事务信息、锁管理器等运行时环境
 * @return std::shared_ptr<Plan> 返回对应SQL语句类型的执行计划根节点
 *
 * @details 支持的SQL语句类型：
 *          DDL语句：CREATE TABLE, DROP TABLE, CREATE INDEX, DROP INDEX
 *          DML语句：INSERT, DELETE, UPDATE, SELECT
 *
 *          工作流程：
 *          1. 通过dynamic_pointer_cast进行AST类型识别
 *          2. 根据不同语句类型调用相应的计划生成逻辑
 *          3. 对于DML语句，还需考虑访问路径优化（索引选择）
 *          4. 返回完整的执行计划树供执行器使用
 */
std::shared_ptr<Plan> Planner::do_planner(std::shared_ptr<Query> query, Context *context) {
    // 执行计划根节点，将根据SQL语句类型进行初始化
    std::shared_ptr<Plan> plannerRoot;
    // ===========================================
    // DDL语句处理：数据定义语言
    // ===========================================

    if (auto x = std::dynamic_pointer_cast<ast::CreateTable>(query->parse)) {
        // ===============================
        // CREATE TABLE 语句处理
        // ===============================

        // 解析表结构定义，将AST中的列定义转换为内部ColDef格式
        std::vector<ColDef> col_defs;
        for (auto &field : x->fields) {
            // 确保字段类型为列定义
            if (auto sv_col_def = std::dynamic_pointer_cast<ast::ColDef>(field)) {
                // 构建列定义结构体，包含列名、数据类型和长度
                ColDef col_def = {.name = sv_col_def->col_name,
                                  .type = interp_sv_type(sv_col_def->type_len->type),  // 类型转换
                                  .len = sv_col_def->type_len->len};                   // 字段长度
                col_defs.push_back(col_def);
            } else {
                throw InternalError("Unexpected field type");
            }
        }

        // 创建DDL执行计划：表创建操作
        // 参数：操作类型、表名、索引列名（空）、列定义
        plannerRoot = std::make_shared<DDLPlan>(T_CreateTable, x->tab_name, std::vector<std::string>(), col_defs);
    } else if (auto x = std::dynamic_pointer_cast<ast::DropTable>(query->parse)) {
        // ===============================
        // DROP TABLE 语句处理
        // ===============================

        // 创建DDL执行计划：表删除操作
        // 删除表时不需要列定义信息，只需要表名
        plannerRoot = std::make_shared<DDLPlan>(T_DropTable, x->tab_name, std::vector<std::string>(), std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::CreateIndex>(query->parse)) {
        // ===============================
        // CREATE INDEX 语句处理
        // ===============================

        // 创建DDL执行计划：索引创建操作
        // 参数包含表名和要建立索引的列名列表
        plannerRoot = std::make_shared<DDLPlan>(T_CreateIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DropIndex>(query->parse)) {
        // ===============================
        // DROP INDEX 语句处理
        // ===============================

        // 创建DDL执行计划：索引删除操作
        // 需要指定表名和要删除的索引列名
        plannerRoot = std::make_shared<DDLPlan>(T_DropIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    }
    // ===========================================
    // 其他DDL语句处理
    // ===========================================

    else if (auto x = std::dynamic_pointer_cast<ast::ShowIndex>(query->parse)) {
        // ===============================
        // SHOW INDEX FROM 语句处理
        // ===============================

        // 创建其他类型执行计划：显示索引操作
        // 用于查看指定表的索引信息
        plannerRoot = std::make_shared<OtherPlan>(T_ShowIndex, x->tab_name);
    }
    // ===========================================
    // DML语句处理：数据操纵语言
    // ===========================================

    else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(query->parse)) {
        // ===============================
        // INSERT 语句处理
        // ===============================

        // INSERT操作比较简单，不需要复杂的查询计划
        // 直接创建DML计划，包含要插入的值
        plannerRoot = std::make_shared<DMLPlan>(T_Insert, std::shared_ptr<Plan>(), x->tab_name,
                                                query->values, std::vector<Condition>(), std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(query->parse)) {
        // ===============================
        // DELETE 语句处理
        // ===============================

        // DELETE操作需要先扫描表找到要删除的记录，然后执行删除

        // 第一步：生成表扫描计划（访问路径选择）
        std::shared_ptr<Plan> table_scan_executors;

        // 检查是否有可用的索引来优化WHERE条件的扫描
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(x->tab_name, query->conds, index_col_names);

        if (index_exist == false) {
            // 情况1：没有合适的索引，使用顺序扫描
            // 清空索引列名，表示使用全表扫描
            index_col_names.clear();
            table_scan_executors =
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        } else {
            // 情况2：存在可用索引，使用索引扫描
            // 利用索引快速定位符合条件的记录，提升删除效率
            table_scan_executors =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        }

        // 第二步：创建DELETE的DML执行计划
        // 将扫描计划作为子计划传入，形成"扫描->删除"的执行管道
        plannerRoot = std::make_shared<DMLPlan>(T_Delete, table_scan_executors, x->tab_name,
                                                std::vector<Value>(), query->conds, std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(query->parse)) {
        // ===============================
        // UPDATE 语句处理
        // ===============================

        // UPDATE操作类似DELETE，也需要先扫描找到记录，然后执行更新

        // 第一步：生成表扫描计划（访问路径选择）
        std::shared_ptr<Plan> table_scan_executors;

        // 检查WHERE条件是否可以利用索引优化
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(x->tab_name, query->conds, index_col_names);

        if (index_exist == false) {
            // 情况1：没有合适的索引，使用顺序扫描
            index_col_names.clear();
            table_scan_executors =
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        } else {
            // 情况2：存在可用索引，使用索引扫描
            // 索引扫描可以显著减少需要检查的记录数量
            table_scan_executors =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        }

        // 第二步：创建UPDATE的DML执行计划
        // 包含扫描计划、WHERE条件和SET子句信息
        plannerRoot = std::make_shared<DMLPlan>(T_Update, table_scan_executors, x->tab_name,
                                                std::vector<Value>(), query->conds,
                                                query->set_clauses);
    } else if (auto x = std::dynamic_pointer_cast<ast::ExplainStmt>(query->parse)) {
        std::shared_ptr<plannerInfo> root = std::make_shared<plannerInfo>(x);

        // 在move query之前保存需要的信息
        auto table_alias_map = query->table_alias_map;
        bool is_select_star = query->is_select_star;

        std::shared_ptr<Plan> projection = generate_select_plan(std::move(query), context);

        plannerRoot = std::make_shared<DMLPlan>(T_Explain, projection, std::string(), std::vector<Value>(),
                                                std::vector<Condition>(), std::vector<SetClause>(), table_alias_map, is_select_star);
    } else if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse)) {
        // ===============================
        // SELECT 语句处理（最复杂的情况）
        // ===============================

        // SELECT是最复杂的查询类型，可能涉及多表连接、排序、聚合等

        // 创建查询计划信息对象（用于后续优化参考）
        std::shared_ptr<plannerInfo> root = std::make_shared<plannerInfo>(x);

        // 调用专门的SELECT计划生成器进行复杂的查询优化
        // 包括：逻辑优化、物理优化、连接顺序选择、排序处理等
        std::shared_ptr<Plan> projection = generate_select_plan(std::move(query), context);

        // 将SELECT查询结果包装为DML计划
        // 这样统一了所有SQL语句的执行接口
        plannerRoot = std::make_shared<DMLPlan>(T_Select, projection, std::string(), std::vector<Value>(),
                                                std::vector<Condition>(), std::vector<SetClause>());
    } else {
        // ===========================================
        // 异常情况处理
        // ===========================================

        // 如果AST根节点不匹配任何已知的SQL语句类型
        // 这可能是由于：
        // 1. 解析器支持了新的SQL语句类型但优化器尚未实现
        // 2. AST结构异常或损坏
        // 3. 不支持的SQL语法
        throw InternalError("Unexpected AST root");
    }

    // ===========================================
    // 返回完整的执行计划
    // ===========================================

    // 返回构建好的执行计划树，供执行引擎使用
    // 此时的plannerRoot包含了完整的执行逻辑，包括：
    // - 数据访问路径（顺序扫描或索引扫描）
    // - 连接算法选择（对于多表查询）
    // - 排序和投影操作
    // - 具体的操作类型（DDL或DML）
    return plannerRoot;
}

// ========== 查询优化算法实现 ==========

/**
 * @brief 谓词下推优化实现
 * @param query 待优化的查询对象
 * @return 优化后的查询对象
 * @details 将WHERE条件尽可能下推到数据源附近，减少中间结果的数据量
 */
std::shared_ptr<Query> Planner::predicate_pushdown(std::shared_ptr<Query> query) {
    // 谓词下推优化：将选择操作尽可能下推到扫描操作附近
    // 目前在make_one_rel中已经实现了基本的条件下推逻辑
    // 这里可以添加更复杂的谓词下推规则，比如：
    // 1. 跨连接的条件下推
    // 2. 复杂表达式的简化和下推
    // 3. 条件的重新排序以提高过滤效率

    // 当前实现：保持现有的条件下推逻辑不变
    // 在make_one_rel中的pop_conds函数已经实现了基本的单表条件下推

    return query;
}

/**
 * @brief 投影下推优化实现
 * @param query 待优化的查询对象
 * @return 优化后的查询对象
 * @details 将SELECT列选择尽可能下推，减少不必要的列传输
 */
std::shared_ptr<Query> Planner::projection_pushdown(std::shared_ptr<Query> query) {
    // 投影下推优化：只传输查询真正需要的列
    // 分析SELECT子句和WHERE子句中用到的列，
    // 确保只从存储层读取必要的列数据

    auto select_stmt = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    if (!select_stmt) {
        return query;
    }

    // 收集真正需要的列
    std::set<std::string> needed_cols;

    // 1. 添加SELECT子句中的列
    for (const auto &col : query->cols) {
        needed_cols.insert(col.tab_name + "." + col.col_name);
    }

    // 2. 添加WHERE条件中涉及的列
    for (const auto &cond : query->conds) {
        needed_cols.insert(cond.lhs_col.tab_name + "." + cond.lhs_col.col_name);
        if (!cond.is_rhs_val) {
            needed_cols.insert(cond.rhs_col.tab_name + "." + cond.rhs_col.col_name);
        }
    }

    // 3. 如果有ORDER BY，添加排序列
    if (select_stmt->has_sort && select_stmt->order) {
        // 需要根据具体的AST结构来获取排序列信息
        // 这里简化处理，实际实现需要根据ast::OrderBy的结构调整
    }

    // 当前简化实现：保持原有列选择不变
    // 完整的投影下推需要更深入的执行计划树分析

    return query;
}

/**
 * @brief 连接顺序优化实现
 * @param query 待优化的查询对象
 * @return 优化后的查询对象
 * @details 使用贪心算法基于表的基数(cardinality)优化连接顺序
 */
std::shared_ptr<Query> Planner::join_order_optimization(std::shared_ptr<Query> query) {
    // 连接顺序优化：使用贪心算法基于表基数进行优化
    // 基本策略：优先连接小表，减少中间结果的大小

    if (query->tables.size() <= 2) {
        // 对于单表或两表查询，无需优化连接顺序
        return query;
    }

    // 收集每个表的统计信息（基数估计）
    std::vector<std::pair<std::string, size_t>> table_stats;

    for (const auto &table_name : query->tables) {
        try {
            // 获取表的实际行数估计
            size_t estimated_cardinality = estimate_table_cardinality(table_name);
            table_stats.emplace_back(table_name, estimated_cardinality);
        } catch (...) {
            // 如果无法获取表信息，使用默认值
            table_stats.emplace_back(table_name, 1000);
        }
    }

    // 实现贪心连接顺序优化算法
    std::vector<std::string> optimized_tables = greedy_join_order_optimization(table_stats, query->conds);
    query->tables = std::move(optimized_tables);

    return query;
}

/**
 * @brief 估算表的行数基数
 * @param table_name 表名
 * @return 估算的表行数
 * @details 通过扫描表文件的页面来估算实际记录数量
 */
size_t Planner::estimate_table_cardinality(const std::string &table_name) {
    try {
        // 获取表的文件句柄
        if (sm_manager_->fhs_.find(table_name) == sm_manager_->fhs_.end()) {
            // 表文件未打开，使用默认估计值
            return 1000;
        }

        auto &file_handle = sm_manager_->fhs_[table_name];
        auto file_hdr = file_handle->get_file_hdr();

        // 使用页面数量和每页记录数来粗略估算
        // 假设页面利用率为70%
        size_t estimated_pages = file_hdr.num_pages > 1 ? file_hdr.num_pages - 1 : 0;  // 减去文件头页面
        size_t estimated_records = estimated_pages * file_hdr.num_records_per_page * 0.7;

        return estimated_records > 0 ? estimated_records : 1;  // 至少返回1避免除零错误
    } catch (...) {
        // 如果出现任何错误，返回默认值
        return 1000;
    }
}

/**
 * @brief 贪心连接顺序优化算法
 * @param table_stats 表的统计信息（表名和基数）
 * @param conditions 连接条件列表
 * @return 优化后的表顺序
 * @details 实现贪心算法：从最小的两个表开始，逐步添加能最小化中间结果大小的表
 */
std::vector<std::string> Planner::greedy_join_order_optimization(
    const std::vector<std::pair<std::string, size_t>> &table_stats,
    const std::vector<Condition> &conditions) {
    if (table_stats.size() <= 2) {
        // 对于两个表或更少，直接按基数排序
        std::vector<std::pair<std::string, size_t>> sorted_stats = table_stats;
        std::sort(sorted_stats.begin(), sorted_stats.end(),
                  [](const auto &a, const auto &b) { return a.second < b.second; });

        std::vector<std::string> result;
        for (const auto &stat : sorted_stats) {
            result.push_back(stat.first);
        }
        return result;
    }

    // 创建表名到基数的映射
    std::unordered_map<std::string, size_t> cardinality_map;
    for (const auto &stat : table_stats) {
        cardinality_map[stat.first] = stat.second;
    }

    // 分析连接图：找出哪些表之间有连接条件
    std::unordered_map<std::string, std::set<std::string>> join_graph;
    for (const auto &cond : conditions) {
        if (!cond.is_rhs_val) {  // 只考虑表间连接条件
            join_graph[cond.lhs_col.tab_name].insert(cond.rhs_col.tab_name);
            join_graph[cond.rhs_col.tab_name].insert(cond.lhs_col.tab_name);
        }
    }

    std::vector<std::string> result;
    std::set<std::string> used_tables;

    // 第一步：找出基数最小的两个表作为起始点
    std::vector<std::pair<std::string, size_t>> sorted_stats = table_stats;
    std::sort(sorted_stats.begin(), sorted_stats.end(),
              [](const auto &a, const auto &b) { return a.second < b.second; });

    result.push_back(sorted_stats[0].first);
    used_tables.insert(sorted_stats[0].first);

    if (sorted_stats.size() > 1) {
        result.push_back(sorted_stats[1].first);
        used_tables.insert(sorted_stats[1].first);
    }

    // 第二步：贪心地添加剩余的表
    while (used_tables.size() < table_stats.size()) {
        std::string best_table;
        size_t min_cost = SIZE_MAX;

        for (const auto &stat : table_stats) {
            const std::string &table = stat.first;
            if (used_tables.count(table)) continue;  // 已经使用过的表

            // 检查这个表是否与已选择的表有连接条件
            bool has_join_condition = false;
            for (const std::string &used_table : used_tables) {
                if (join_graph[table].count(used_table)) {
                    has_join_condition = true;
                    break;
                }
            }

            // 如果没有连接条件但还有其他表有连接条件，暂时跳过
            if (!has_join_condition) {
                bool other_tables_have_joins = false;
                for (const auto &other_stat : table_stats) {
                    if (used_tables.count(other_stat.first) || other_stat.first == table) continue;
                    for (const std::string &used_table : used_tables) {
                        if (join_graph[other_stat.first].count(used_table)) {
                            other_tables_have_joins = true;
                            break;
                        }
                    }
                    if (other_tables_have_joins) break;
                }
                if (other_tables_have_joins) continue;
            }

            // 估算添加这个表的成本（简化版：直接使用表的基数）
            size_t cost = cardinality_map[table];

            if (cost < min_cost) {
                min_cost = cost;
                best_table = table;
            }
        }

        if (!best_table.empty()) {
            result.push_back(best_table);
            used_tables.insert(best_table);
        } else {
            // 如果没有找到最佳表，选择剩余表中基数最小的
            for (const auto &stat : sorted_stats) {
                if (!used_tables.count(stat.first)) {
                    result.push_back(stat.first);
                    used_tables.insert(stat.first);
                    break;
                }
            }
        }
    }

    return result;
}

/**
 * @brief 在物理计划中应用投影下推
 * @param plan 基础计划树
 * @param query 查询对象，包含投影信息
 * @return 插入Project节点后的计划树
 */
std::shared_ptr<Plan> Planner::apply_projection_pushdown(std::shared_ptr<Plan> plan, std::shared_ptr<Query> query) {
    if (!plan)
        return plan;

    // 获取SELECT语句
    auto select_stmt = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    if (!select_stmt)
        return plan;

    // 分析查询中需要的列
    std::set<std::string> needed_columns;

    // 添加SELECT子句中的列
    for (const auto &col : query->cols) {
        needed_columns.insert(col.tab_name + "." + col.col_name);
    }

    // 添加WHERE条件中涉及的列
    for (const auto &cond : query->conds) {
        needed_columns.insert(cond.lhs_col.tab_name + "." + cond.lhs_col.col_name);
        if (!cond.is_rhs_val) {
            needed_columns.insert(cond.rhs_col.tab_name + "." + cond.rhs_col.col_name);
        }
    }
    collect_join_columns_from_plan(plan, needed_columns);

    // 根据要求：
    // - SELECT *：只在根节点添加Project节点
    // - 其他情况：在Scan节点上方添加Project节点，并在根节点添加最终的Project节点
    if (query->is_select_star) {
        // SELECT *：只在根节点添加Project节点
        plan = std::make_shared<ProjectionPlan>(T_Projection, std::move(plan), query->cols);
    } else {
        // 其他情况：在Scan节点上方添加Project节点
        plan = insert_project_nodes(plan, needed_columns, query->cols, query);

        // 为多表查询在根节点添加最终的Project节点来选择输出列
        std::set<std::string> plan_tables;
        collect_table_names_from_plan(plan, plan_tables);

        if (plan_tables.size() > 1) {
            // 多表查询需要根节点Project来选择最终输出列
            plan = std::make_shared<ProjectionPlan>(T_Projection, std::move(plan), query->cols);
        }
    }

    return plan;
}

/**
 * @brief 在计划树中插入Project节点
 */
std::shared_ptr<Plan> Planner::insert_project_nodes(std::shared_ptr<Plan> plan,
                                                    const std::set<std::string> &needed_columns,
                                                    const std::vector<TabCol> &select_cols,
                                                    std::shared_ptr<Query> query) {
    if (!plan)
        return plan;

    // 根据计划类型决定如何插入Project节点
    if (auto join_plan = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        // 对于Join节点，递归处理左右子树
        join_plan->left_ = insert_project_nodes(join_plan->left_, needed_columns, select_cols, query);
        join_plan->right_ = insert_project_nodes(join_plan->right_, needed_columns, select_cols, query);
        return plan;
    } else if (auto scan_plan = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        // 对于Scan节点，只添加Project节点（不处理Filter）
        std::string table_name = scan_plan->tab_name_;
        std::shared_ptr<Plan> current_plan = plan;

        // 为每个Scan节点添加Project节点（除了SELECT *）
        std::vector<TabCol> table_needed_cols;

        // 只添加SELECT子句中属于当前表的列
        for (const auto &col : select_cols) {
            if (col.tab_name == table_name) {
                table_needed_cols.push_back(col);
            }
        }

        // 添加JOIN条件中需要的列（但不包括WHERE过滤条件中的列）
        // 检查是否有JOIN操作，如果有，需要添加JOIN条件中涉及的列
        std::set<std::string> join_columns;

        // 从query的条件中收集JOIN条件涉及的列
        for (const auto &cond : query->conds) {
            if (!cond.is_rhs_val) {  // JOIN条件：两边都是列
                // 添加该条件涉及的列
                if (cond.lhs_col.tab_name == table_name) {
                    join_columns.insert(cond.lhs_col.tab_name + "." + cond.lhs_col.col_name);
                }
                if (cond.rhs_col.tab_name == table_name) {
                    join_columns.insert(cond.rhs_col.tab_name + "." + cond.rhs_col.col_name);
                }
            }
        }

        for (const auto &join_col : join_columns) {
            size_t dot_pos = join_col.find('.');
            if (dot_pos != std::string::npos) {
                std::string tab_name = join_col.substr(0, dot_pos);
                std::string col_name = join_col.substr(dot_pos + 1);

                if (tab_name == table_name) {
                    TabCol tab_col;
                    tab_col.tab_name = tab_name;
                    tab_col.col_name = col_name;

                    // 避免重复添加
                    bool already_exists = false;
                    for (const auto &existing_col : table_needed_cols) {
                        if (existing_col.tab_name == tab_col.tab_name &&
                            existing_col.col_name == tab_col.col_name) {
                            already_exists = true;
                            break;
                        }
                    }
                    if (!already_exists) {
                        table_needed_cols.push_back(tab_col);
                    }
                }
            }
        }

        // 除了SELECT *之外，所有Scan节点都应该有Project节点
        // 如果table_needed_cols为空，添加该表的所有列
        if (table_needed_cols.empty()) {
            std::cerr << "DEBUG: table_needed_cols is empty for table " << table_name << ", adding all columns" << std::endl;
            try {
                const auto &all_table_cols = sm_manager_->db_.get_table(table_name).cols;
                for (const auto &col_meta : all_table_cols) {
                    TabCol tab_col;
                    tab_col.tab_name = table_name;
                    tab_col.col_name = col_meta.name;
                    table_needed_cols.push_back(tab_col);
                }
            } catch (const std::exception &e) {
                std::cerr << "DEBUG: Exception getting table info for " << table_name << ": " << e.what() << std::endl;
            }
        }

        // 为所有Scan节点添加Project节点（除了SELECT *情况已在上层处理）
        if (!table_needed_cols.empty()) {
            std::cerr << "DEBUG: Adding Project node for table " << table_name << " with " << table_needed_cols.size() << " columns" << std::endl;
            current_plan = std::make_shared<ProjectionPlan>(T_Projection, current_plan, table_needed_cols);
        }

        return current_plan;
    }

    return plan;
}

/**
 * @brief 检查条件是否可以下推到指定计划
 */
bool Planner::can_push_condition_to_plan(const Condition &cond, std::shared_ptr<Plan> plan) {
    if (!plan)
        return false;

    // 对于扫描节点，检查条件是否涉及该表
    if (auto scan_plan = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        return cond.lhs_col.tab_name == scan_plan->tab_name_ && cond.is_rhs_val;
    }

    // 对于连接节点，条件可能涉及两个表
    if (auto join_plan = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        return true;  // 连接条件通常在连接节点处理
    }

    return false;
}

/**
 * @brief 分析子树需要的列
 */
void Planner::analyze_required_columns_for_subtree(std::shared_ptr<Plan> plan,
                                                   const std::set<std::string> &all_needed,
                                                   std::set<std::string> &subtree_needed) {
    if (!plan)
        return;

    if (auto scan_plan = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        // 对于扫描节点，收集该表需要的列
        for (const auto &col : all_needed) {
            if (col.find(scan_plan->tab_name_ + ".") == 0) {
                subtree_needed.insert(col);
            }
        }
    } else if (auto join_plan = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        // 递归处理连接节点
        analyze_required_columns_for_subtree(join_plan->left_, all_needed, subtree_needed);
        analyze_required_columns_for_subtree(join_plan->right_, all_needed, subtree_needed);
    }
}

/**
 * @brief 将字符串列名转换为TabCol对象
 */
std::vector<TabCol> Planner::convert_to_tabcol(const std::set<std::string> &col_names) {
    std::vector<TabCol> result;
    for (const auto &col_name : col_names) {
        auto dot_pos = col_name.find('.');
        if (dot_pos != std::string::npos) {
            TabCol col;
            col.tab_name = col_name.substr(0, dot_pos);
            col.col_name = col_name.substr(dot_pos + 1);
            result.push_back(col);
        }
    }
    return result;
}

/**
 * @brief 检查是否是SELECT *查询
 */
bool Planner::is_select_all(std::shared_ptr<ast::SelectStmt> select_stmt) {
    if (!select_stmt || select_stmt->cols.empty())
        return false;

    // 检查是否只有一个列且为*
    if (select_stmt->cols.size() == 1) {
        auto first_col = select_stmt->cols.front();
        if (auto col = std::dynamic_pointer_cast<ast::Col>(first_col)) {
            return col->col_name == "*";
        }
    }
    return false;
}

/**
 * @brief 从计划树中提取条件
 * @param plan 计划节点
 * @param conditions 提取的条件列表（输出参数）
 */
void Planner::extract_conditions_from_plan(std::shared_ptr<Plan> plan, std::vector<Condition> &conditions) {
    if (!plan)
        return;

    if (auto scan_plan = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        // 从ScanPlan中提取条件
        for (const auto &cond : scan_plan->conds_) {
            conditions.push_back(cond);
        }
    } else if (auto join_plan = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        // 递归处理子节点
        extract_conditions_from_plan(join_plan->left_, conditions);
        extract_conditions_from_plan(join_plan->right_, conditions);
    }
}

/**
 * @brief 清除计划树中的条件
 * @param plan 计划节点
 */
void Planner::clear_conditions_from_plan(std::shared_ptr<Plan> plan) {
    if (!plan)
        return;

    if (auto scan_plan = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        // 清除ScanPlan中的条件
        scan_plan->conds_.clear();
    } else if (auto join_plan = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        // 递归处理子节点
        clear_conditions_from_plan(join_plan->left_);
        clear_conditions_from_plan(join_plan->right_);
    }
}

/**
 * @brief 从计划树中收集JOIN条件中涉及的列
 * @param plan 计划节点
 * @param join_columns 收集到的JOIN列集合（输出参数）
 */
void Planner::collect_join_columns_from_plan(std::shared_ptr<Plan> plan, std::set<std::string> &join_columns) {
    if (!plan)
        return;

    if (auto join_plan = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        // 收集当前JOIN节点的条件中涉及的列
        for (const auto &cond : join_plan->conds_) {
            join_columns.insert(cond.lhs_col.tab_name + "." + cond.lhs_col.col_name);
            if (!cond.is_rhs_val) {
                join_columns.insert(cond.rhs_col.tab_name + "." + cond.rhs_col.col_name);
            }
        }

        // 递归处理子节点
        collect_join_columns_from_plan(join_plan->left_, join_columns);
        collect_join_columns_from_plan(join_plan->right_, join_columns);
    } else if (auto proj_plan = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        // 递归处理ProjectionPlan的子节点
        collect_join_columns_from_plan(proj_plan->subplan_, join_columns);
    }
    // ScanPlan不包含JOIN条件，无需处理
}

/**
 * @brief 从计划树中收集表名
 */
void Planner::collect_table_names_from_plan(std::shared_ptr<Plan> plan, std::set<std::string> &table_names) {
    if (!plan)
        return;

    if (auto scan_plan = std::dynamic_pointer_cast<ScanPlan>(plan)) {
        table_names.insert(scan_plan->tab_name_);
    } else if (auto join_plan = std::dynamic_pointer_cast<JoinPlan>(plan)) {
        collect_table_names_from_plan(join_plan->left_, table_names);
        collect_table_names_from_plan(join_plan->right_, table_names);
    } else if (auto proj_plan = std::dynamic_pointer_cast<ProjectionPlan>(plan)) {
        collect_table_names_from_plan(proj_plan->subplan_, table_names);
    }
}