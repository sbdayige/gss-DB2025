%{
#include "ast.h"
#include "yacc.tab.h"
#include <iostream>
#include <memory>

int yylex(YYSTYPE *yylval, YYLTYPE *yylloc);

void yyerror(YYLTYPE *locp, const char* s) {
    std::cerr << "Parser Error at line " << locp->first_line << " column " << locp->first_column << ": " << s << std::endl;
}

using namespace ast;
%}

// request a pure (reentrant) parser
%define api.pure full
// enable location in error handler
%locations
// enable verbose syntax error message
%define parse.error verbose

// keywords
%token SHOW TABLES CREATE TABLE DROP DESC INSERT INTO VALUES DELETE FROM ASC ORDER BY 
%token WHERE UPDATE SET SELECT INT CHAR FLOAT INDEX AND JOIN INNER LEFT RIGHT FULL SEMI ANTI
%token ON AS EXIT HELP TXN_BEGIN TXN_COMMIT TXN_ABORT TXN_ROLLBACK ORDER_BY 
%token ENABLE_NESTLOOP ENABLE_SORTMERGE STATIC_CHECKPOINT CRASH
%token GROUP HAVING LIMIT COUNT MAX MIN SUM AVG LOAD
// non-keywords
%token LEQ NEQ GEQ T_EOF
%token EXPLAIN

// 优先级和结合性规则
%right JOIN  // JOIN 右结合，优先级较高
%left INNER LEFT RIGHT FULL SEMI ANTI  // JOIN类型左结合，优先级较低

// arithmetic operators
%left '+' '-'
%left '*' '/'
%right UMINUS  // 一元减号优先级最高

// type-specific tokens
%token <sv_str> IDENTIFIER VALUE_STRING VALUE_PATH
%token <sv_int> VALUE_INT
%token <sv_float> VALUE_FLOAT
%token <sv_bool> VALUE_BOOL

// specify types for non-terminal symbol
%type <sv_node> stmt dbStmt ddl dml txnStmt setStmt
%type <sv_field> field
%type <sv_fields> fieldList
%type <sv_type_len> type
%type <sv_comp_op> op
%type <sv_expr> expr
%type <sv_val> value
%type <sv_vals> valueList
%type <sv_str> tbName colName fileName
%type <sv_strs> colNameList
%type <sv_col> col
%type <sv_cols> colList selector groupByClause optGroupByClause
%type <sv_col> selectExpr
%type <sv_table_ref> tableRef
%type <sv_table_refs> baseTableList
%type <sv_join_exprs> joinList
%type <sv_join_expr> joinExpr
%type <sv_join_type> joinType
%type <sv_set_clause> setClause
%type <sv_set_clauses> setClauses
%type <sv_cond> condition havingCondition
%type <sv_conds> whereClause optWhereClause havingClause optHavingClause
%type <sv_orderby>  order_clause opt_order_clause
%type <sv_orderby_dir> opt_asc_desc
%type <sv_orderby_item> orderByItem
%type <sv_orderby_items> orderByList
%type <sv_setKnobType> set_knob_type

%type <sv_agg_expr> aggregateExpr
%type <sv_limit_clause> optLimitClause

%%
start:
        stmt ';'
    {
        parse_tree = $1;
        YYACCEPT;
    }
    |   HELP
    {
        parse_tree = std::make_shared<Help>();
        YYACCEPT;
    }
    |   EXIT
    {
        parse_tree = nullptr;
        YYACCEPT;
    }
    |   T_EOF
    {
        parse_tree = nullptr;
        YYACCEPT;
    }
    ;

stmt:
        dbStmt
    |   ddl
    |   dml
    |   txnStmt
    |   setStmt
    ;

txnStmt:
        TXN_BEGIN
    {
        $$ = std::make_shared<TxnBegin>();
    }
    |   TXN_COMMIT
    {
        $$ = std::make_shared<TxnCommit>();
    }
    |   TXN_ABORT
    {
        $$ = std::make_shared<TxnAbort>();
    }
    | TXN_ROLLBACK
    {
        $$ = std::make_shared<TxnRollback>();
    }
    ;

dbStmt:
        SHOW TABLES
    {
        $$ = std::make_shared<ShowTables>();
    }
    |   SHOW INDEX FROM tbName
    {
        $$ = std::make_shared<ShowIndex>($4);
    }
    |   LOAD fileName INTO tbName
    {
         $$ = std::make_shared<LoadStmt>($2, $4);
    }
    ;

setStmt:
        SET set_knob_type '=' VALUE_BOOL
    {
        $$ = std::make_shared<SetStmt>($2, $4);
    }
    ;

ddl:
        CREATE TABLE tbName '(' fieldList ')'
    {
        $$ = std::make_shared<CreateTable>($3, $5);
    }
    |   DROP TABLE tbName
    {
        $$ = std::make_shared<DropTable>($3);
    }
    |   DESC tbName
    {
        $$ = std::make_shared<DescTable>($2);
    }
    |   CREATE INDEX tbName '(' colNameList ')'
    {
        $$ = std::make_shared<CreateIndex>($3, $5);
    }
    |   DROP INDEX tbName '(' colNameList ')'
    {
        $$ = std::make_shared<DropIndex>($3, $5);
    }
    |   CREATE STATIC_CHECKPOINT
    {
        $$ = std::make_shared<CreateStaticCheckpoint>();
    }
    |   CRASH
    {
        $$ = std::make_shared<Crash>();
    }
    ;

dml:
        INSERT INTO tbName VALUES '(' valueList ')'
    {
        $$ = std::make_shared<InsertStmt>($3, $6);
    }
    |   DELETE FROM tbName optWhereClause
    {
        $$ = std::make_shared<DeleteStmt>($3, $4);
    }
    |   UPDATE tbName SET setClauses optWhereClause
    {
        $$ = std::make_shared<UpdateStmt>($2, $4, $5);
    }
    |   SELECT selector FROM baseTableList joinList optWhereClause optGroupByClause optHavingClause opt_order_clause optLimitClause
    {
        // 收集聚合表达式
        std::vector<std::shared_ptr<AggregateExpr>> agg_exprs;
        for (auto& col : $2) {
            // 检查列名是否表示聚合函数
            if (col->col_name.find("COUNT(") == 0 || col->col_name.find("MAX(") == 0 || 
                col->col_name.find("MIN(") == 0 || col->col_name.find("SUM(") == 0 || 
                col->col_name.find("AVG(") == 0) {
                // 根据列名重建聚合表达式
                std::shared_ptr<AggregateExpr> agg_expr;
                
                if (col->col_name.find("COUNT(*)") == 0) {
                    agg_expr = std::make_shared<AggregateExpr>(AGG_COUNT, nullptr, col->alias);
                } else if (col->col_name.find("COUNT(") == 0) {
                    // 提取列名
                    std::string col_part = col->col_name.substr(6, col->col_name.length() - 7);
                    auto col_obj = std::make_shared<Col>("", col_part);
                    agg_expr = std::make_shared<AggregateExpr>(AGG_COUNT, col_obj, col->alias);
                } else if (col->col_name.find("MAX(") == 0) {
                    std::string col_part = col->col_name.substr(4, col->col_name.length() - 5);
                    auto col_obj = std::make_shared<Col>("", col_part);
                    agg_expr = std::make_shared<AggregateExpr>(AGG_MAX, col_obj, col->alias);
                } else if (col->col_name.find("MIN(") == 0) {
                    std::string col_part = col->col_name.substr(4, col->col_name.length() - 5);
                    auto col_obj = std::make_shared<Col>("", col_part);
                    agg_expr = std::make_shared<AggregateExpr>(AGG_MIN, col_obj, col->alias);
                } else if (col->col_name.find("SUM(") == 0) {
                    std::string col_part = col->col_name.substr(4, col->col_name.length() - 5);
                    auto col_obj = std::make_shared<Col>("", col_part);
                    agg_expr = std::make_shared<AggregateExpr>(AGG_SUM, col_obj, col->alias);
                } else if (col->col_name.find("AVG(") == 0) {
                    std::string col_part = col->col_name.substr(4, col->col_name.length() - 5);
                    auto col_obj = std::make_shared<Col>("", col_part);
                    agg_expr = std::make_shared<AggregateExpr>(AGG_AVG, col_obj, col->alias);
                }
                
                if (agg_expr) {
                    agg_exprs.push_back(agg_expr);
                }
            }
        }
        
        // 过滤掉聚合函数列，只保留普通列
        std::vector<std::shared_ptr<Col>> regular_cols;
        for (auto& col : $2) {
            // 检查是否是聚合函数（通过col_name中的特殊标记识别）
            if (!(col->col_name.find("COUNT(") == 0 || col->col_name.find("MAX(") == 0 || 
                  col->col_name.find("MIN(") == 0 || col->col_name.find("SUM(") == 0 || 
                  col->col_name.find("AVG(") == 0)) {
                // 只有普通列才添加到regular_cols中
                regular_cols.push_back(col);
            }
        }
        
        $$ = std::make_shared<SelectStmt>(regular_cols, $4, $6, $5, agg_exprs, $7, $8, $9, $10);
    }    
    | EXPLAIN SELECT selector FROM baseTableList joinList optWhereClause optGroupByClause optHavingClause opt_order_clause optLimitClause   
    {
        $$ = std::make_shared<ExplainStmt>($3, $5, $7, $6, std::vector<std::shared_ptr<AggregateExpr>>{}, $8, $9, $10, $11);
    }
    ;

fieldList:
        field
    {
        $$ = std::vector<std::shared_ptr<Field>>{$1};
    }
    |   fieldList ',' field
    {
        $$.push_back($3);
    }
    ;

colNameList:
        colName
    {
        $$ = std::vector<std::string>{$1};
    }
    | colNameList ',' colName
    {
        $$.push_back($3);
    }
    ;

field:
        colName type
    {
        $$ = std::make_shared<ColDef>($1, $2);
    }
    ;

type:
        INT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_INT, sizeof(int));
    }
    |   CHAR '(' VALUE_INT ')'
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_STRING, $3);
    }
    |   FLOAT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_FLOAT, sizeof(float));
    }
    ;

valueList:
        value
    {
        $$ = std::vector<std::shared_ptr<Value>>{$1};
    }
    |   valueList ',' value
    {
        $$.push_back($3);
    }
    ;

value:
        VALUE_INT
    {
        $$ = std::make_shared<IntLit>($1);
    }
    |   VALUE_FLOAT
    {
        $$ = std::make_shared<FloatLit>($1);
    }
    |   VALUE_STRING
    {
        $$ = std::make_shared<StringLit>($1);
    }
    |   VALUE_BOOL
    {
        $$ = std::make_shared<BoolLit>($1);
    }
    |   '-' VALUE_INT %prec UMINUS
    {
        $$ = std::make_shared<IntLit>(-$2);
    }
    |   '-' VALUE_FLOAT %prec UMINUS
    {
        $$ = std::make_shared<FloatLit>(-$2);
    }
    ;

condition:
        col op expr
    {
        $$ = std::make_shared<BinaryExpr>($1, $2, $3);
    }
    ;

optWhereClause:
        /* epsilon */ { /* ignore*/ }
    |   WHERE whereClause
    {
        $$ = $2;
    }
    ;

whereClause:
        condition 
    {
        $$ = std::vector<std::shared_ptr<BinaryExpr>>{$1};
    }
    |   whereClause AND condition
    {
        $$.push_back($3);
    }
    ;

col:
        tbName '.' colName
    {
        $$ = std::make_shared<Col>($1, $3);
    }
    |   tbName '.' colName AS IDENTIFIER
    {
        $$ = std::make_shared<Col>($1, $3, $5);
    }
    |   colName
    {
        $$ = std::make_shared<Col>("", $1);
    }
    |   colName AS IDENTIFIER
    {
        $$ = std::make_shared<Col>("", $1, $3);
    }
    ;

colList:
        selectExpr
    {
        $$ = std::vector<std::shared_ptr<Col>>{$1};
    }
    |   colList ',' selectExpr
    {
        $$.push_back($3);
    }
    ;

selectExpr:
        col
    {
        $$ = $1;
    }
    |   aggregateExpr
    {
        // 将聚合函数表达式转换为列，用于兼容现有的SelectStmt结构
        auto agg_col = std::make_shared<Col>("", "");
        
        // 设置别名
        if (!$1->alias.empty()) {
            agg_col->alias = $1->alias;
        }
        
        // 总是为聚合函数生成col_name（用于识别）
        switch ($1->agg_type) {
            case AGG_COUNT:
                agg_col->col_name = $1->col ? "COUNT(" + $1->col->col_name + ")" : "COUNT(*)";
                break;
            case AGG_MAX:
                agg_col->col_name = "MAX(" + $1->col->col_name + ")";
                break;
            case AGG_MIN:
                agg_col->col_name = "MIN(" + $1->col->col_name + ")";
                break;
            case AGG_SUM:
                agg_col->col_name = "SUM(" + $1->col->col_name + ")";
                break;
            case AGG_AVG:
                agg_col->col_name = "AVG(" + $1->col->col_name + ")";
                break;
        }
        
        $$ = agg_col;
    }
    ;

op:
        '='
    {
        $$ = SV_OP_EQ;
    }
    |   '<'
    {
        $$ = SV_OP_LT;
    }
    |   '>'
    {
        $$ = SV_OP_GT;
    }
    |   NEQ
    {
        $$ = SV_OP_NE;
    }
    |   LEQ
    {
        $$ = SV_OP_LE;
    }
    |   GEQ
    {
        $$ = SV_OP_GE;
    }
    ;

expr:
        value
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    |   col
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    |   aggregateExpr
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    |   expr '+' expr
    {
        $$ = std::make_shared<ArithExpr>($1, ARITH_OP_ADD, $3);
    }
    |   expr '-' expr
    {
        $$ = std::make_shared<ArithExpr>($1, ARITH_OP_SUB, $3);
    }
    |   expr '*' expr
    {
        $$ = std::make_shared<ArithExpr>($1, ARITH_OP_MUL, $3);
    }
    |   expr '/' expr
    {
        $$ = std::make_shared<ArithExpr>($1, ARITH_OP_DIV, $3);
    }
    |   '-' expr %prec UMINUS
    {
        // 一元减号（负号）
        auto zero = std::make_shared<IntLit>(0);
        $$ = std::make_shared<ArithExpr>(zero, ARITH_OP_SUB, $2);
    }
    |   '(' expr ')'
    {
        $$ = $2;
    }
    ;

setClauses:
        setClause
    {
        $$ = std::vector<std::shared_ptr<SetClause>>{$1};
    }
    |   setClauses ',' setClause
    {
        $$.push_back($3);
    }
    ;

setClause:
        colName '=' expr
    {
        $$ = std::make_shared<SetClause>($1, $3);
    }
    ;

selector:
        '*'
    {
        $$ = {};
    }
    |   colList
    ;

tableRef:
        tbName
    {
        $$ = std::make_shared<TableRef>($1);
    }
    |   tbName AS IDENTIFIER
    {
        $$ = std::make_shared<TableRef>($1, $3);
    }
    |   tbName IDENTIFIER
    {
        $$ = std::make_shared<TableRef>($1, $2);
    }
    ;



joinType:
        INNER
    {
        $$ = INNER_JOIN;
    }
    |   LEFT
    {
        $$ = LEFT_JOIN;
    }
    |   RIGHT
    {
        $$ = RIGHT_JOIN;
    }
    |   FULL
    {
        $$ = FULL_JOIN;
    }
    |   SEMI
    {
        $$ = SEMI_JOIN;
    }
    | ANTI
    {
         $$ = ANTI_JOIN;
    }
    ;

baseTableList:
        tableRef
    {
        $$ = std::vector<std::shared_ptr<TableRef>>{$1};
    }
    |   baseTableList ',' tableRef
    {
        $$.push_back($3);
    }
    ;

joinList:
        /* epsilon */
    {
        $$ = std::vector<std::shared_ptr<JoinExpr>>{};
    }
    |   joinList joinExpr
    {
        $$.push_back($2);
    }
    ;

joinExpr:
        joinType JOIN tableRef ON whereClause
    {
        $$ = std::make_shared<JoinExpr>(
            "",           // 左表名，在语义分析阶段确定
            $3,           // 右表引用（包含别名）
            $5,           // 连接条件列表（支持AND连接）
            $1            // JOIN类型
        );
    }
    |   joinType JOIN tableRef
    {
        // JOIN without ON clause - 支持将连接条件放在WHERE子句中
        $$ = std::make_shared<JoinExpr>(
            "",           // 左表名，在语义分析阶段确定
            $3,           // 右表引用（包含别名）
            std::vector<std::shared_ptr<BinaryExpr>>{}, // 空的连接条件
            $1            // JOIN类型
        );
    }
    |   JOIN tableRef ON whereClause
    {
        // 默认为INNER JOIN
        $$ = std::make_shared<JoinExpr>(
            "",           // 左表名，在语义分析阶段确定
            $2,           // 右表引用（包含别名）
            $4,           // 连接条件列表（支持AND连接）
            INNER_JOIN    // 默认为内连接
        );
    }
    |   JOIN tableRef
    {
        // JOIN without ON clause - 默认为INNER JOIN
        $$ = std::make_shared<JoinExpr>(
            "",           // 左表名，在语义分析阶段确定
            $2,           // 右表引用（包含别名）
            std::vector<std::shared_ptr<BinaryExpr>>{}, // 空的连接条件
            INNER_JOIN    // 默认为内连接
        );
    }
    ;

opt_order_clause:
    ORDER BY order_clause      
    { 
        $$ = $3; 
    }
    |   /* epsilon */ { /* ignore*/ }
    ;

order_clause:
    orderByList
    { 
        $$ = std::make_shared<OrderBy>($1);
    }
    ;   

orderByList:
    orderByItem
    {
        $$ = std::vector<std::shared_ptr<OrderByItem>>{$1};
    }
    |   orderByList ',' orderByItem
    {
        $$.push_back($3);
    }
    ;

orderByItem:
    col opt_asc_desc
    {
        $$ = std::make_shared<OrderByItem>($1, $2);
    }
    ;

opt_asc_desc:
    ASC          { $$ = OrderBy_ASC;     }
    |  DESC      { $$ = OrderBy_DESC;    }
    |       { $$ = OrderBy_DEFAULT; }
    ;    

set_knob_type:
    ENABLE_NESTLOOP { $$ = EnableNestLoop; }
    |   ENABLE_SORTMERGE { $$ = EnableSortMerge; }
    ;

aggregateExpr:
    COUNT '(' col ')'
    {
        $$ = std::make_shared<AggregateExpr>(AGG_COUNT, $3);
    }
    |   COUNT '(' col ')' AS IDENTIFIER
    {
        $$ = std::make_shared<AggregateExpr>(AGG_COUNT, $3, $6);
    }
    |   COUNT '(' '*' ')'
    {
        $$ = std::make_shared<AggregateExpr>(AGG_COUNT, nullptr);
    }
    |   COUNT '(' '*' ')' AS IDENTIFIER
    {
        $$ = std::make_shared<AggregateExpr>(AGG_COUNT, nullptr, $6);
    }
    |   MAX '(' col ')'
    {
        $$ = std::make_shared<AggregateExpr>(AGG_MAX, $3);
    }
    |   MAX '(' col ')' AS IDENTIFIER
    {
        $$ = std::make_shared<AggregateExpr>(AGG_MAX, $3, $6);
    }
    |   MIN '(' col ')'
    {
        $$ = std::make_shared<AggregateExpr>(AGG_MIN, $3);
    }
    |   MIN '(' col ')' AS IDENTIFIER
    {
        $$ = std::make_shared<AggregateExpr>(AGG_MIN, $3, $6);
    }
    |   SUM '(' col ')'
    {
        $$ = std::make_shared<AggregateExpr>(AGG_SUM, $3);
    }
    |   SUM '(' col ')' AS IDENTIFIER
    {
        $$ = std::make_shared<AggregateExpr>(AGG_SUM, $3, $6);
    }
    |   AVG '(' col ')'
    {
        $$ = std::make_shared<AggregateExpr>(AGG_AVG, $3);
    }
    |   AVG '(' col ')' AS IDENTIFIER
    {
        $$ = std::make_shared<AggregateExpr>(AGG_AVG, $3, $6);
    }
    ;

optGroupByClause:
    /* epsilon */ { $$ = std::vector<std::shared_ptr<Col>>{}; }
    |   GROUP BY groupByClause { $$ = $3; }
    ;

groupByClause:
    col
    {
        $$ = std::vector<std::shared_ptr<Col>>{$1};
    }
    |   groupByClause ',' col
    {
        $$.push_back($3);
    }
    ;

optHavingClause:
    /* epsilon */ { $$ = std::vector<std::shared_ptr<BinaryExpr>>{}; }
    |   HAVING havingClause { $$ = $2; }
    ;

havingClause:
    havingCondition
    {
        $$ = std::vector<std::shared_ptr<BinaryExpr>>{$1};
    }
    |   havingClause AND havingCondition
    {
        $$.push_back($3);
    }
    ;

havingCondition:
    aggregateExpr op expr
    {
        // Create a column that represents the aggregate function for HAVING
        auto agg_col = std::make_shared<Col>("", "");
        
        // Generate a name for the aggregate function so it can be recognized
        switch ($1->agg_type) {
            case AGG_COUNT:
                agg_col->col_name = $1->col ? "COUNT(" + $1->col->col_name + ")" : "COUNT(*)";
                break;
            case AGG_MAX:
                agg_col->col_name = "MAX(" + $1->col->col_name + ")";
                break;
            case AGG_MIN:
                agg_col->col_name = "MIN(" + $1->col->col_name + ")";
                break;
            case AGG_SUM:
                agg_col->col_name = "SUM(" + $1->col->col_name + ")";
                break;
            case AGG_AVG:
                agg_col->col_name = "AVG(" + $1->col->col_name + ")";
                break;
        }
        
        $$ = std::make_shared<BinaryExpr>(agg_col, $2, $3);
    }
    |   col op expr
    {
        $$ = std::make_shared<BinaryExpr>($1, $2, $3);
    }
    ;

optLimitClause:
    /* epsilon */ { /* ignore */ }
    |   LIMIT VALUE_INT
    {
        $$ = std::make_shared<LimitClause>($2);
    }
    ;

tbName: IDENTIFIER;

colName: IDENTIFIER;

fileName: VALUE_PATH;
%%
