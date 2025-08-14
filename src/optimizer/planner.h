#pragma once

#include <cassert>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "analyze/analyze.h"
#include "common/common.h"
#include "common/context.h"
#include "execution/execution_defs.h"
#include "execution/execution_manager.h"
#include "parser/parser.h"
#include "plan.h"
#include "record/rm.h"
#include "system/sm.h"

extern SmManager sm_manager;

class Planner {
   private:
    bool enable_nestedloop_join = true;
    bool enable_sortmerge_join = false;

   public:
    Planner() = default;

    std::shared_ptr<Plan> do_planner(std::shared_ptr<Query> query, Context *context);

    void set_enable_nestedloop_join(bool set_val) { enable_nestedloop_join = set_val; }

    void set_enable_sortmerge_join(bool set_val) { enable_sortmerge_join = set_val; }

   private:
    std::shared_ptr<Query> logical_optimization(std::shared_ptr<Query> query, Context *context);

    std::shared_ptr<Query> predicate_pushdown(std::shared_ptr<Query> query);

    std::shared_ptr<Query> projection_pushdown(std::shared_ptr<Query> query);

    std::shared_ptr<Query> join_order_optimization(std::shared_ptr<Query> query);

    size_t estimate_table_cardinality(const std::string &table_name);

    std::vector<std::string> greedy_join_order_optimization(
        const std::vector<std::pair<std::string, size_t>> &table_stats,
        const std::vector<Condition> &conditions);

    void collect_table_names_from_plan(std::shared_ptr<Plan> plan, std::set<std::string> &table_names);

    std::shared_ptr<Plan> apply_projection_pushdown(std::shared_ptr<Plan> plan, std::shared_ptr<Query> query);

    std::shared_ptr<Plan> insert_project_nodes(std::shared_ptr<Plan> plan,
                                               const std::set<std::string> &needed_columns,
                                               const std::vector<TabCol> &select_cols,
                                               std::shared_ptr<Query> query);

    void analyze_required_columns_for_subtree(std::shared_ptr<Plan> plan,
                                              const std::set<std::string> &all_needed,
                                              std::set<std::string> &subtree_needed);

    std::vector<TabCol> convert_to_tabcol(const std::set<std::string> &col_names);

    size_t estimate_join_result_size(const std::string &table1, const std::string &table2,
                                     const std::unordered_map<std::string, size_t> &cardinality_map,
                                     const std::unordered_map<std::string, std::set<std::string>> &join_graph,
                                     const std::vector<Condition> &conditions);

    size_t estimate_join_with_intermediate_result(size_t intermediate_size, const std::string &new_table,
                                                  const std::unordered_map<std::string, size_t> &cardinality_map,
                                                  const std::unordered_map<std::string, std::set<std::string>> &join_graph,
                                                  const std::vector<Condition> &conditions,
                                                  const std::set<std::string> &used_tables);

    std::shared_ptr<Plan> physical_optimization(std::shared_ptr<Query> query, Context *context);

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

    std::shared_ptr<Plan> generate_select_plan(std::shared_ptr<Query> query, Context *context);

    bool get_index_cols(std::string tab_name, std::vector<Condition> curr_conds, std::vector<std::string> &index_col_names);

    ColType interp_sv_type(ast::SvType sv_type) {
        std::map<ast::SvType, ColType> m = {
            {ast::SV_TYPE_INT, TYPE_INT}, {ast::SV_TYPE_FLOAT, TYPE_FLOAT}, {ast::SV_TYPE_STRING, TYPE_STRING}};
        return m.at(sv_type);
    }

    void collect_join_columns_from_plan(std::shared_ptr<Plan> plan, std::set<std::string> &join_columns);
};
