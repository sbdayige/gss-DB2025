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
 * @file planner.h
 * @brief 查询规划器类定义
 * @details 该文件定义了RMDB数据库系统中的查询规划器(Planner)类，
 *          负责将解析后的SQL查询转换为可执行的物理查询计划。
 *          Planner是查询优化器的核心组件，实现了逻辑优化和物理优化功能。
 */

#pragma once

#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <set>

#include "execution/execution_defs.h"
#include "execution/execution_manager.h"
#include "record/rm.h"
#include "system/sm.h"
#include "common/context.h"
#include "plan.h"
#include "parser/parser.h"
#include "common/common.h"
#include "analyze/analyze.h"

/**
 * @class Planner
 * @brief 查询规划器类
 * @details 负责将解析后的SQL查询转换为优化的物理执行计划。
 *          实现了完整的查询优化流程，包括逻辑优化和物理优化两个阶段。
 *          支持多种连接算法的选择和索引优化策略。
 */
class Planner
{
private:
    SmManager *sm_manager_; ///< 系统管理器指针，提供元数据访问和表管理功能

    // 连接算法控制标志
    bool enable_nestedloop_join = true; ///< 是否启用嵌套循环连接算法，默认启用
    bool enable_sortmerge_join = false; ///< 是否启用排序归并连接算法，默认禁用

public:
    /**
     * @brief 构造函数
     * @param sm_manager 系统管理器指针，用于访问数据库元数据和表信息
     */
    Planner(SmManager *sm_manager) : sm_manager_(sm_manager) {}

    /**
     * @brief 查询规划的主入口函数
     * @param query 解析后的查询对象，包含SQL语句的抽象语法树
     * @param context 查询执行上下文，包含事务信息等
     * @return 生成的执行计划树根节点
     * @details 根据SQL语句类型(DDL/DML/SELECT)生成相应的执行计划，
     *          是整个查询优化流程的统一入口点
     */
    std::shared_ptr<Plan> do_planner(std::shared_ptr<Query> query, Context *context);

    /**
     * @brief 设置是否启用嵌套循环连接算法
     * @param set_val true表示启用，false表示禁用
     * @details 嵌套循环连接是最基本的连接算法，适用于所有连接类型，
     *          但在大表连接时性能较差
     */
    void set_enable_nestedloop_join(bool set_val) { enable_nestedloop_join = set_val; }

    /**
     * @brief 设置是否启用排序归并连接算法
     * @param set_val true表示启用，false表示禁用
     * @details 排序归并连接在连接列有序或可以高效排序时性能较好，
     *          特别适用于等值连接和大表连接场景
     */
    void set_enable_sortmerge_join(bool set_val) { enable_sortmerge_join = set_val; }

private:
    // ========== 查询优化核心函数 ==========

    /**
     * @brief 逻辑优化阶段
     * @param query 待优化的查询对象
     * @param context 查询执行上下文
     * @return 逻辑优化后的查询对象
     * @details 执行与具体物理实现无关的优化，如：
     *          - 谓词下推(predicate pushdown)
     *          - 投影下推(projection pushdown)
     *          - 连接重排序(join reordering)
     *          - 常量折叠(constant folding)
     *          目前可能是预留接口，用于扩展逻辑优化规则
     */
    std::shared_ptr<Query> logical_optimization(std::shared_ptr<Query> query, Context *context);

    /**
     * @brief 谓词下推优化
     * @param query 待优化的查询对象
     * @return 优化后的查询对象
     * @details 将WHERE条件尽可能下推到数据源附近，减少中间结果的数据量
     */
    std::shared_ptr<Query> predicate_pushdown(std::shared_ptr<Query> query);

    /**
     * @brief 投影下推优化
     * @param query 待优化的查询对象
     * @return 优化后的查询对象
     * @details 将SELECT列选择尽可能下推，减少不必要的列传输
     */
    std::shared_ptr<Query> projection_pushdown(std::shared_ptr<Query> query);

    /**
     * @brief 连接顺序优化
     * @param query 待优化的查询对象
     * @return 优化后的查询对象
     * @details 使用贪心算法基于表的基数(cardinality)优化连接顺序
     */
    std::shared_ptr<Query> join_order_optimization(std::shared_ptr<Query> query);

    /**
     * @brief 在物理计划中应用谓词下推
     * @param plan 基础计划树
     * @param query 查询对象，包含谓词信息
     * @return 插入Filter节点后的计划树
     */
    std::shared_ptr<Plan> apply_predicate_pushdown(std::shared_ptr<Plan> plan, std::shared_ptr<Query> query);

    /**
     * @brief 递归地将Filter节点下推到计划树的合适位置
     */
    std::shared_ptr<Plan> push_filters_down(std::shared_ptr<Plan> plan, std::shared_ptr<Query> query);

    /**
     * @brief 从计划树中收集表名
     */
    void collect_table_names_from_plan(std::shared_ptr<Plan> plan, std::set<std::string> &table_names);

    /**
     * @brief 在物理计划中应用投影下推
     * @param plan 基础计划树
     * @param query 查询对象，包含投影信息
     * @return 插入Project节点后的计划树
     */
    std::shared_ptr<Plan> apply_projection_pushdown(std::shared_ptr<Plan> plan, std::shared_ptr<Query> query);

    // ========== 优化辅助方法 ==========

    /**
     * @brief 在计划树中插入Filter节点
     */
    std::shared_ptr<Plan> insert_filter_nodes(std::shared_ptr<Plan> plan, std::vector<Condition> &conditions);

    /**
     * @brief 在计划树中插入Project节点
     */
    std::shared_ptr<Plan> insert_project_nodes(std::shared_ptr<Plan> plan,
                                               const std::set<std::string> &needed_columns,
                                               const std::vector<TabCol> &select_cols);

    /**
     * @brief 检查条件是否可以下推到指定计划
     */
    bool can_push_condition_to_plan(const Condition &cond, std::shared_ptr<Plan> plan);

    /**
     * @brief 分析子树需要的列
     */
    void analyze_required_columns_for_subtree(std::shared_ptr<Plan> plan,
                                              const std::set<std::string> &all_needed,
                                              std::set<std::string> &subtree_needed);

    /**
     * @brief 将字符串列名转换为TabCol对象
     */
    std::vector<TabCol> convert_to_tabcol(const std::set<std::string> &col_names);

    /**
     * @brief 检查是否是SELECT *查询
     */
    bool is_select_all(std::shared_ptr<ast::SelectStmt> select_stmt);

    /**
     * @brief 物理优化阶段
     * @param query 逻辑优化后的查询对象
     * @param context 查询执行上下文
     * @return 物理执行计划树
     * @details 将逻辑查询转换为具体的物理执行计划：
     *          - 选择访问路径(顺序扫描vs索引扫描)
     *          - 选择连接算法(嵌套循环vs排序归并)
     *          - 确定连接顺序
     *          - 添加排序操作
     */
    std::shared_ptr<Plan> physical_optimization(std::shared_ptr<Query> query, Context *context);

    // ========== 专门化规划函数 ==========

    /**
     * @brief 多表连接规划的核心函数
     * @param query 包含多表查询信息的查询对象
     * @return 多表连接的执行计划树
     * @details 负责复杂的多表连接规划：
     *          - 为每个表选择最优扫描策略(SeqScan/IndexScan)
     *          - 根据WHERE条件确定表的连接顺序
     *          - 选择合适的连接算法
     *          - 处理连接条件的下推优化
     *          - 构建层次化的连接执行计划树
     */
    std::shared_ptr<Plan> make_one_rel(std::shared_ptr<Query> query);

    /**
     * @brief 生成聚合执行计划
     * @param query 包含聚合函数信息的查询对象
     * @param plan 需要添加聚合操作的基础执行计划
     * @return 添加聚合操作后的执行计划
     * @details 处理聚合函数：
     *          - 检查查询是否包含聚合函数(COUNT, MAX, MIN, SUM, AVG)
     *          - 在现有计划基础上添加聚合操作符
     *          - 支持GROUP BY和HAVING子句
     */
    std::shared_ptr<Plan> generate_aggregate_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan);

    /**
     * @brief 生成排序执行计划
     * @param query 包含ORDER BY信息的查询对象
     * @param plan 需要添加排序操作的基础执行计划
     * @return 添加排序操作后的执行计划
     * @details 处理ORDER BY子句：
     *          - 检查查询是否包含排序要求
     *          - 确定排序列和排序方向(ASC/DESC)
     *          - 在现有计划基础上添加排序操作符
     */
    std::shared_ptr<Plan> generate_sort_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan);

    /**
     * @brief SELECT语句的专门规划函数
     * @param query SELECT查询对象
     * @param context 查询执行上下文
     * @return SELECT语句的完整执行计划
     * @details 协调SELECT查询的完整优化流程：
     *          - 依次调用逻辑优化和物理优化
     *          - 在优化后的计划上添加投影操作
     *          - 处理SELECT子句中的列选择和表达式计算
     */
    std::shared_ptr<Plan> generate_select_plan(std::shared_ptr<Query> query, Context *context);

    // ========== 辅助工具函数 ==========

    // int get_indexNo(std::string tab_name, std::vector<Condition> curr_conds); // 已弃用的函数

    /**
     * @brief 索引选择和匹配函数
     * @param tab_name 表名
     * @param curr_conds 当前表的查询条件列表
     * @param index_col_names 输出参数，匹配的索引列名
     * @return true表示找到可用索引，false表示无可用索引
     * @details 核心的索引优化功能：
     *          - 分析WHERE条件中的等值谓词
     *          - 检查表上是否存在匹配的索引
     *          - 目前的匹配规则：完全匹配索引字段且全部为单点查询
     *          - 为索引扫描提供必要的列信息
     *          - 实现基于成本的访问路径选择基础
     */
    bool get_index_cols(std::string tab_name, std::vector<Condition> curr_conds, std::vector<std::string> &index_col_names);

    /**
     * @brief AST类型到系统内部类型的转换函数
     * @param sv_type AST中定义的数据类型
     * @return 系统内部使用的列类型
     * @details 类型系统转换的关键函数：
     *          - SV_TYPE_INT → TYPE_INT (整数类型)
     *          - SV_TYPE_FLOAT → TYPE_FLOAT (浮点类型)
     *          - SV_TYPE_STRING → TYPE_STRING (字符串类型)
     *          - 确保解析阶段和执行阶段类型系统的一致性
     */
    ColType interp_sv_type(ast::SvType sv_type)
    {
        std::map<ast::SvType, ColType> m = {
            {ast::SV_TYPE_INT, TYPE_INT}, {ast::SV_TYPE_FLOAT, TYPE_FLOAT}, {ast::SV_TYPE_STRING, TYPE_STRING}};
        return m.at(sv_type);
    }

    /**
     * @brief 从计划树中提取条件
     * @param plan 计划节点
     * @param conditions 提取的条件列表（输出参数）
     */
    void extract_conditions_from_plan(std::shared_ptr<Plan> plan, std::vector<Condition> &conditions);

    /**
     * @brief 清除计划树中的条件
     * @param plan 计划节点
     */
    void clear_conditions_from_plan(std::shared_ptr<Plan> plan);
};
