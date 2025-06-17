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

class Planner {
   private:
    SmManager *sm_manager_;

    bool enable_nestedloop_join = true;
    bool enable_sortmerge_join = false;

   public:
    Planner(SmManager *sm_manager) : sm_manager_(sm_manager) {}

    std::shared_ptr<Plan> do_planner(std::shared_ptr<Query> query, Context *context);

    void set_enable_nestedloop_join(bool set_val) { enable_nestedloop_join = set_val; }

    void set_enable_sortmerge_join(bool set_val) { enable_sortmerge_join = set_val; }

   private:
    void collect_table_names_from_plan(std::shared_ptr<Plan> plan, std::set<std::string> &table_names);

    std::shared_ptr<Plan> apply_projection_pushdown(std::shared_ptr<Plan> plan, std::shared_ptr<Query> query);

    std::shared_ptr<Plan> insert_project_nodes(std::shared_ptr<Plan> plan,
                                               const std::set<std::string> &needed_columns,
                                               const std::vector<TabCol> &select_cols,
                                               std::shared_ptr<Query> query);

    bool can_push_condition_to_plan(const Condition &cond, std::shared_ptr<Plan> plan);

    void analyze_required_columns_for_subtree(std::shared_ptr<Plan> plan,
                                              const std::set<std::string> &all_needed,
                                              std::set<std::string> &subtree_needed);

    std::vector<TabCol> convert_to_tabcol(const std::set<std::string> &col_names);

    bool is_select_all(std::shared_ptr<ast::SelectStmt> select_stmt);

    std::shared_ptr<Plan> physical_optimization(std::shared_ptr<Query> query, Context *context);

    std::shared_ptr<Plan> make_one_rel(std::shared_ptr<Query> query);

    std::shared_ptr<Plan> generate_sort_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan);

    bool get_index_cols(std::string tab_name, std::vector<Condition> curr_conds, std::vector<std::string> &index_col_names);

    ColType interp_sv_type(ast::SvType sv_type) {
        std::map<ast::SvType, ColType> m = {
            {ast::SV_TYPE_INT, TYPE_INT}, {ast::SV_TYPE_FLOAT, TYPE_FLOAT}, {ast::SV_TYPE_STRING, TYPE_STRING}};
        return m.at(sv_type);
    }

    void extract_conditions_from_plan(std::shared_ptr<Plan> plan, std::vector<Condition> &conditions);

    void clear_conditions_from_plan(std::shared_ptr<Plan> plan);

    void collect_join_columns_from_plan(std::shared_ptr<Plan> plan, std::set<std::string> &join_columns);
};
