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
 * @file ast.h
 * @brief 抽象语法树(AST)定义文件
 *
 * 这个文件定义了RMDB数据库系统的抽象语法树结构。
 * AST是编译器前端的核心数据结构，用于表示SQL语句的语法结构。
 *
 * 整体架构说明：
 * 1. 使用继承体系：所有AST节点都继承自TreeNode基类
 * 2. 采用组合模式：复杂语句通过组合简单节点构建
 * 3. 使用智能指针：通过shared_ptr管理内存，避免内存泄漏
 * 4. 支持多态：通过虚析构函数支持多态销毁
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

/**
 * @brief JOIN连接类型枚举
 * 定义SQL中支持的各种JOIN操作类型
 */
enum JoinType {
    INNER_JOIN,  // 内连接：只返回两表匹配的记录
    LEFT_JOIN,   // 左连接：返回左表所有记录和右表匹配记录
    RIGHT_JOIN,  // 右连接：返回右表所有记录和左表匹配记录
    FULL_JOIN,   // 全连接：返回两表所有记录
    SEMI_JOIN    // 半连接：返回左表中与右表匹配的记录，但不包含右表列
};

namespace ast {
// 前向声明
struct TableRef;

/**
 * @brief 数据类型枚举
 * 定义SQL中支持的基本数据类型
 */
enum SvType {
    SV_TYPE_INT,     // 整数类型
    SV_TYPE_FLOAT,   // 浮点数类型
    SV_TYPE_STRING,  // 字符串类型
    SV_TYPE_BOOL     // 布尔类型
};

/**
 * @brief 比较操作符枚举
 * 定义WHERE子句中支持的比较运算符
 */
enum SvCompOp {
    SV_OP_EQ,  // 等于 =
    SV_OP_NE,  // 不等于 <> 或 !=
    SV_OP_LT,  // 小于 <
    SV_OP_GT,  // 大于 >
    SV_OP_LE,  // 小于等于 <=
    SV_OP_GE   // 大于等于 >=
};

/**
 * @brief 排序方向枚举
 * 定义ORDER BY子句中的排序方向
 */
enum OrderByDir {
    OrderBy_DEFAULT,  // 默认排序（通常为升序）
    OrderBy_ASC,      // 升序排序
    OrderBy_DESC      // 降序排序
};

/**
 * @brief 系统配置参数类型枚举
 * 定义SET语句可以设置的系统参数类型
 */
enum SetKnobType {
    EnableNestLoop,  // 启用嵌套循环连接算法
    EnableSortMerge  // 启用排序合并连接算法
};

/**
 * @brief AST节点基类
 *
 * 所有AST节点的基类，提供统一的接口。
 * 使用虚析构函数确保派生类对象能正确销毁，支持多态。
 */
struct TreeNode {
    virtual ~TreeNode() = default;  // 启用多态销毁
};

/**
 * @brief HELP命令AST节点
 * 表示用户输入的HELP命令，用于显示帮助信息
 */
struct Help : public TreeNode {
};

/**
 * @brief SHOW TABLES命令AST节点
 * 表示显示数据库中所有表的命令
 */
struct ShowTables : public TreeNode {
};

/**
 * @brief 事务开始命令AST节点
 * 表示BEGIN TRANSACTION命令
 */
struct TxnBegin : public TreeNode {
};

/**
 * @brief 事务提交命令AST节点
 * 表示COMMIT TRANSACTION命令
 */

struct TxnCommit : public TreeNode {
};

/**
 * @brief 事务中止命令AST节点
 * 表示ABORT TRANSACTION命令
 */
struct TxnAbort : public TreeNode {
};

/**
 * @brief 事务回滚命令AST节点
 * 表示ROLLBACK TRANSACTION命令
 */
struct TxnRollback : public TreeNode {
};

/**
 * @brief 创建静态检查点命令AST节点
 * 表示CREATE STATIC_CHECKPOINT命令
 */
struct CreateStaticCheckpoint : public TreeNode {
};

/**
 * @brief 系统崩溃命令AST节点
 * 表示CRASH命令，用于测试故障恢复功能
 */
struct Crash : public TreeNode {
};

/**
 * @brief 类型长度信息AST节点
 *
 * 存储数据类型及其长度信息。
 * 例如：INT类型长度为4字节，CHAR(20)类型长度为20字节
 *
 * @param type 数据类型枚举值
 * @param len 类型长度（字节数）
 */
struct TypeLen : public TreeNode {
    SvType type;  // 数据类型
    int len;      // 类型长度

    TypeLen(SvType type_, int len_) : type(type_), len(len_) {}
};

/**
 * @brief 字段基类AST节点
 *
 * 用于表示表结构中的字段信息，是ColDef等具体字段类型的基类
 */
struct Field : public TreeNode {
};

/**
 * @brief 列定义AST节点
 *
 * 表示CREATE TABLE语句中的列定义，包含列名和类型信息
 * 例如：CREATE TABLE student (id INT, name CHAR(20))
 * 其中 "id INT" 就是一个ColDef节点
 *
 * @param col_name 列名
 * @param type_len 列的类型和长度信息
 */
struct ColDef : public Field {
    std::string col_name;               // 列名
    std::shared_ptr<TypeLen> type_len;  // 类型信息指针

    ColDef(std::string col_name_, std::shared_ptr<TypeLen> type_len_) : col_name(std::move(col_name_)), type_len(std::move(type_len_)) {}
};

/**
 * @brief 创建表语句AST节点
 *
 * 表示CREATE TABLE语句的完整结构
 * 例如：CREATE TABLE student (id INT, name CHAR(20), age INT)
 *
 * @param tab_name 表名
 * @param fields 字段定义列表
 */
struct CreateTable : public TreeNode {
    std::string tab_name;                        // 表名
    std::vector<std::shared_ptr<Field>> fields;  // 字段定义列表

    CreateTable(std::string tab_name_, std::vector<std::shared_ptr<Field>> fields_) : tab_name(std::move(tab_name_)), fields(std::move(fields_)) {}
};

/**
 * @brief 删除表语句AST节点
 *
 * 表示DROP TABLE语句
 * 例如：DROP TABLE student
 *
 * @param tab_name 要删除的表名
 */
struct DropTable : public TreeNode {
    std::string tab_name;  // 表名

    DropTable(std::string tab_name_) : tab_name(std::move(tab_name_)) {}
};

/**
 * @brief 描述表结构语句AST节点
 *
 * 表示DESC TABLE语句，用于查看表的结构信息
 * 例如：DESC student
 *
 * @param tab_name 要查看的表名
 */
struct DescTable : public TreeNode {
    std::string tab_name;  // 表名

    DescTable(std::string tab_name_) : tab_name(std::move(tab_name_)) {}
};

/**
 * @brief 显示索引语句AST节点
 *
 * 表示SHOW INDEX FROM语句，用于查看表的索引信息
 * 例如：SHOW INDEX FROM student
 *
 * @param tab_name 要查看索引的表名
 */
struct ShowIndex : public TreeNode {
    std::string tab_name;  // 表名

    ShowIndex(std::string tab_name_) : tab_name(std::move(tab_name_)) {}
};

/**
 * @brief 加载数据语句AST节点
 *
 * 表示LOAD DATA INFILE INTO TABLE语句，用于从文件加载数据到表中
 * 例如：LOAD DATA INFILE 'data.csv' INTO TABLE student
 *
 * @param file_name 数据文件名
 * @param tab_name 目标表名
 */
struct LoadStmt : public TreeNode {
    std::string file_name;
    std::string tab_name;

    LoadStmt(std::string file_name_, std::string table_name_) : file_name(std::move(file_name_)), tab_name(std::move(table_name_)) {}
};

/**
 * @brief 创建索引语句AST节点
 *
 * 表示CREATE INDEX语句
 * 例如：CREATE INDEX student (id, name)
 *
 * @param tab_name 表名
 * @param col_names 索引列名列表
 */
struct CreateIndex : public TreeNode {
    std::string tab_name;                // 表名
    std::vector<std::string> col_names;  // 索引列名列表

    CreateIndex(std::string tab_name_, std::vector<std::string> col_names_) : tab_name(std::move(tab_name_)), col_names(std::move(col_names_)) {}
};

/**
 * @brief 删除索引语句AST节点
 *
 * 表示DROP INDEX语句
 * 例如：DROP INDEX student (id, name)
 *
 * @param tab_name 表名
 * @param col_names 索引列名列表
 */
struct DropIndex : public TreeNode {
    std::string tab_name;                // 表名
    std::vector<std::string> col_names;  // 索引列名列表

    DropIndex(std::string tab_name_, std::vector<std::string> col_names_) : tab_name(std::move(tab_name_)), col_names(std::move(col_names_)) {}
};

/**
 * @brief 表达式基类AST节点
 *
 * 所有表达式节点的基类，包括值表达式、列引用表达式等
 */
struct Expr : public TreeNode {
};

/**
 * @brief 值基类AST节点
 *
 * 所有字面值的基类，包括整数、浮点数、字符串、布尔值等
 */
struct Value : public Expr {
};

/**
 * @brief 整数字面值AST节点
 *
 * 表示SQL中的整数常量
 * 例如：INSERT INTO student VALUES (123, 'John', 20)
 * 其中123和20就是IntLit节点
 *
 * @param val 整数值
 */
struct IntLit : public Value {
    int val;  // 整数值

    IntLit(int val_) : val(val_) {}
};

/**
 * @brief 浮点数字面值AST节点
 *
 * 表示SQL中的浮点数常量
 * 例如：UPDATE student SET gpa = 3.75 WHERE id = 1
 * 其中3.75就是FloatLit节点
 *
 * @param val 浮点数值
 */
struct FloatLit : public Value {
    float val;  // 浮点数值

    FloatLit(float val_) : val(val_) {}
};

/**
 * @brief 字符串字面值AST节点
 *
 * 表示SQL中的字符串常量
 * 例如：INSERT INTO student VALUES (1, 'John Doe', 20)
 * 其中'John Doe'就是StringLit节点
 *
 * @param val 字符串值
 */
struct StringLit : public Value {
    std::string val;  // 字符串值

    StringLit(std::string val_) : val(std::move(val_)) {}
};

/**
 * @brief 布尔值字面值AST节点
 *
 * 表示SQL中的布尔常量
 * 例如：SET enable_nestloop = TRUE
 * 其中TRUE就是BoolLit节点
 *
 * @param val 布尔值
 */
struct BoolLit : public Value {
    bool val;  // 布尔值

    BoolLit(bool val_) : val(val_) {}
};

/**
 * @brief 聚合函数类型枚举
 * 定义支持的聚合函数类型
 */
enum AggType {
    AGG_COUNT,  // COUNT函数
    AGG_MAX,    // MAX函数
    AGG_MIN,    // MIN函数
    AGG_SUM,    // SUM函数
    AGG_AVG     // AVG函数
};

/**
 * @brief 列引用AST节点
 *
 * 表示SQL中对表列的引用，支持带表名前缀和不带表名前缀两种形式
 * 也支持列别名功能
 * 例如：
 * - student.name (带表名前缀)
 * - name (不带表名前缀)
 * - student.name AS student_name (带别名)
 *
 * @param tab_name 表名（可为空）
 * @param col_name 列名
 * @param alias 列别名（可为空）
 */
struct Col : public Expr {
    std::string tab_name;  // 表名（空字符串表示未指定）
    std::string col_name;  // 列名
    std::string alias;     // 列别名（空字符串表示无别名）

    Col(std::string tab_name_, std::string col_name_)
        : tab_name(std::move(tab_name_)), col_name(std::move(col_name_)), alias("") {}

    Col(std::string tab_name_, std::string col_name_, std::string alias_)
        : tab_name(std::move(tab_name_)), col_name(std::move(col_name_)), alias(std::move(alias_)) {}
};

/**
 * @brief 聚合函数AST节点
 *
 * 表示SQL中的聚合函数调用
 * 例如：COUNT(*), MAX(score), SUM(id)
 *
 * @param agg_type 聚合函数类型
 * @param col 聚合的列（COUNT(*)时为nullptr）
 * @param alias 函数别名（可为空）
 */
struct AggregateExpr : public Expr {
    AggType agg_type;          // 聚合函数类型
    std::shared_ptr<Col> col;  // 聚合的列（COUNT(*)时为nullptr）
    std::string alias;         // 函数别名

    AggregateExpr(AggType agg_type_, std::shared_ptr<Col> col_)
        : agg_type(agg_type_), col(std::move(col_)), alias("") {}

    AggregateExpr(AggType agg_type_, std::shared_ptr<Col> col_, std::string alias_)
        : agg_type(agg_type_), col(std::move(col_)), alias(std::move(alias_)) {}
};

/**
 * @brief 算术操作符枚举
 * 定义算术表达式中支持的运算符
 */
enum ArithOp {
    ARITH_OP_ADD,  // 加法 +
    ARITH_OP_SUB,  // 减法 -
    ARITH_OP_MUL,  // 乘法 *
    ARITH_OP_DIV   // 除法 /
};

/**
 * @brief 算术表达式AST节点
 *
 * 表示SQL中的算术表达式，支持基本的数学运算
 * 例如：UPDATE student SET score = score + 10 WHERE id = 1
 * 其中 "score + 10" 就是一个ArithExpr节点
 *
 * @param lhs 左操作数（可以是列引用或值）
 * @param op 算术操作符
 * @param rhs 右操作数（可以是列引用或值）
 */
struct ArithExpr : public Expr {
    std::shared_ptr<Expr> lhs;  // 左操作数
    ArithOp op;                 // 算术操作符
    std::shared_ptr<Expr> rhs;  // 右操作数

    ArithExpr(std::shared_ptr<Expr> lhs_, ArithOp op_, std::shared_ptr<Expr> rhs_)
        : lhs(std::move(lhs_)), op(op_), rhs(std::move(rhs_)) {}
};

/**
 * @brief 赋值子句AST节点
 *
 * 表示UPDATE语句中的SET子句中的单个赋值操作
 * 例如：UPDATE student SET name = 'John', age = 21 WHERE id = 1
 * 其中 "name = 'John'" 和 "age = 21" 就是SetClause节点
 *
 * @param col_name 要赋值的列名
 * @param val 赋值的值
 */
struct SetClause : public TreeNode {
    std::string col_name;
    std::shared_ptr<Expr> val;  // Changed from Value to Expr

    SetClause(std::string col_name_, std::shared_ptr<Expr> val_)  // Changed from Value to Expr
        : col_name(std::move(col_name_)), val(std::move(val_)) {}
};

/**
 * @brief 二元表达式AST节点
 *
 * 表示WHERE子句中的比较条件，包含左操作数、操作符和右操作数
 * 例如：WHERE student.age > 18 AND student.name = 'John'
 * 其中 "student.age > 18" 就是一个BinaryExpr节点
 *
 * @param lhs 左操作数（必须是列引用）
 * @param op 比较操作符
 * @param rhs 右操作数（可以是值或列引用）
 */
struct BinaryExpr : public TreeNode {
    std::shared_ptr<Col> lhs;   // 左操作数（列引用）
    SvCompOp op;                // 比较操作符
    std::shared_ptr<Expr> rhs;  // 右操作数（值或列引用）

    BinaryExpr(std::shared_ptr<Col> lhs_, SvCompOp op_, std::shared_ptr<Expr> rhs_) : lhs(std::move(lhs_)), op(op_), rhs(std::move(rhs_)) {}
};

/**
 * @brief 单个排序项AST节点
 *
 * 表示ORDER BY子句中的单个排序项，包含列引用和排序方向
 * 例如：ORDER BY name ASC, age DESC 中的 "name ASC" 和 "age DESC"
 *
 * @param col 排序列引用
 * @param orderby_dir 排序方向（ASC/DESC）
 */
struct OrderByItem : public TreeNode {
    std::shared_ptr<Col> col;  // 排序列
    OrderByDir orderby_dir;    // 排序方向

    OrderByItem(std::shared_ptr<Col> col_, OrderByDir orderby_dir_)
        : col(std::move(col_)), orderby_dir(orderby_dir_) {}
};

/**
 * @brief ORDER BY子句AST节点
 *
 * 表示SELECT语句中的ORDER BY子句，支持多列排序
 * 例如：SELECT * FROM student ORDER BY age DESC, name ASC
 *
 * @param order_items 排序项列表，支持多列排序
 * @param cols 排序列引用（为了向后兼容保留）
 * @param orderby_dir 排序方向（为了向后兼容保留）
 */
struct OrderBy : public TreeNode {
    std::vector<std::shared_ptr<OrderByItem>> order_items;  // 多列排序项列表
    std::shared_ptr<Col> cols;                              // 排序列（向后兼容）
    OrderByDir orderby_dir;                                 // 排序方向（向后兼容）

    // 新的构造函数：支持多列排序
    OrderBy(std::vector<std::shared_ptr<OrderByItem>> order_items_)
        : order_items(std::move(order_items_)), orderby_dir(OrderBy_DEFAULT) {
        // 为了向后兼容，如果只有一列，也设置cols和orderby_dir
        if (!order_items.empty()) {
            cols = order_items[0]->col;
            orderby_dir = order_items[0]->orderby_dir;
        }
    }

    // 向后兼容的构造函数：单列排序
    OrderBy(std::shared_ptr<Col> cols_, OrderByDir orderby_dir_)
        : cols(std::move(cols_)), orderby_dir(orderby_dir_) {
        // 创建对应的order_items
        order_items.push_back(std::make_shared<OrderByItem>(cols, orderby_dir));
    }

    // 检查是否为多列排序
    bool is_multi_column() const {
        return order_items.size() > 1;
    }

    // 获取排序列数量
    size_t column_count() const {
        return order_items.size();
    }
};

/**
 * @brief INSERT语句AST节点
 *
 * 表示INSERT INTO语句的完整结构
 * 例如：INSERT INTO student VALUES (1, 'John', 20)
 *
 * @param tab_name 目标表名
 * @param vals 要插入的值列表
 */
struct InsertStmt : public TreeNode {
    std::string tab_name;                      // 表名
    std::vector<std::shared_ptr<Value>> vals;  // 插入值列表

    InsertStmt(std::string tab_name_, std::vector<std::shared_ptr<Value>> vals_) : tab_name(std::move(tab_name_)), vals(std::move(vals_)) {}
};

/**
 * @brief DELETE语句AST节点
 *
 * 表示DELETE FROM语句的完整结构
 * 例如：DELETE FROM student WHERE age > 25
 *
 * @param tab_name 目标表名
 * @param conds WHERE条件列表（多个条件用AND连接）
 */
struct DeleteStmt : public TreeNode {
    std::string tab_name;                            // 表名
    std::vector<std::shared_ptr<BinaryExpr>> conds;  // WHERE条件列表

    DeleteStmt(std::string tab_name_, std::vector<std::shared_ptr<BinaryExpr>> conds_) : tab_name(std::move(tab_name_)), conds(std::move(conds_)) {}
};

/**
 * @brief UPDATE语句AST节点
 *
 * 表示UPDATE语句的完整结构
 * 例如：UPDATE student SET name = 'John', age = 21 WHERE id = 1
 *
 * @param tab_name 目标表名
 * @param set_clauses SET子句列表（多个赋值操作）
 * @param conds WHERE条件列表（多个条件用AND连接）
 */
struct UpdateStmt : public TreeNode {
    std::string tab_name;                                 // 表名
    std::vector<std::shared_ptr<SetClause>> set_clauses;  // SET子句列表
    std::vector<std::shared_ptr<BinaryExpr>> conds;       // WHERE条件列表

    UpdateStmt(std::string tab_name_,
               std::vector<std::shared_ptr<SetClause>> set_clauses_,
               std::vector<std::shared_ptr<BinaryExpr>> conds_) : tab_name(std::move(tab_name_)), set_clauses(std::move(set_clauses_)), conds(std::move(conds_)) {}
};

/**
 * @brief JOIN表达式AST节点
 *
 * 表示SQL中的JOIN操作，包含连接的两个表、连接条件和连接类型
 * 例如：SELECT * FROM student JOIN course ON student.course_id = course.id
 *
 * @param left 左表名
 * @param right 右表名
 * @param conds 连接条件列表
 * @param type 连接类型（INNER/LEFT/RIGHT/FULL）
 */
struct JoinExpr : public TreeNode {
    std::string left;                                // 左表名
    std::shared_ptr<TableRef> right_ref;             // 右表引用（包含别名）
    std::vector<std::shared_ptr<BinaryExpr>> conds;  // 连接条件列表
    JoinType type;                                   // 连接类型

    JoinExpr(std::string left_, std::shared_ptr<TableRef> right_ref_,
             std::vector<std::shared_ptr<BinaryExpr>> conds_, JoinType type_)
        : left(std::move(left_)), right_ref(std::move(right_ref_)), conds(std::move(conds_)), type(type_) {}

    // 保持向后兼容的构造函数
    JoinExpr(std::string left_, std::string right_,
             std::vector<std::shared_ptr<BinaryExpr>> conds_, JoinType type_)
        : left(std::move(left_)), conds(std::move(conds_)), type(type_) {
        right_ref = std::make_shared<TableRef>(right_);
    }
};

/**
 * @brief 表引用AST节点
 *
 * 表示SQL中的表引用，支持表别名
 * 例如：
 * - student (只有表名)
 * - student AS s (带别名)
 * - student s (带别名，省略AS关键字)
 *
 * @param tab_name 表名
 * @param alias 表别名（空字符串表示无别名）
 */
struct TableRef : public TreeNode {
    std::string tab_name;  // 表名
    std::string alias;     // 表别名（空字符串表示无别名）

    TableRef(std::string tab_name_) : tab_name(std::move(tab_name_)), alias("") {}

    TableRef(std::string tab_name_, std::string alias_) : tab_name(std::move(tab_name_)), alias(std::move(alias_)) {}
};

/**
 * @brief LIMIT子句AST节点
 *
 * 表示SELECT语句中的LIMIT子句，用于限制返回的记录数量
 * 例如：SELECT * FROM student LIMIT 10
 *
 * @param count 限制的记录数量
 */
struct LimitClause : public TreeNode {
    int count;  // 限制的记录数量

    LimitClause(int count_) : count(count_) {}
};

/**
 * @brief SELECT语句AST节点
 *
 * 表示SELECT语句的完整结构，是最复杂的SQL语句类型，支持聚合函数和分组统计
 * 例如：SELECT student.name, AVG(score) FROM student WHERE age > 18 GROUP BY name HAVING AVG(score) > 80 ORDER BY name LIMIT 10
 *
 * @param cols 选择列列表（空表示SELECT *）
 * @param table_refs 数据源表引用列表（包含表名和别名）
 * @param conds WHERE条件列表
 * @param jointree JOIN操作列表
 * @param group_by_cols GROUP BY列列表
 * @param having_conds HAVING条件列表
 * @param has_sort 是否包含ORDER BY子句
 * @param order ORDER BY子句信息
 * @param limit_clause LIMIT子句信息
 */
struct SelectStmt : public TreeNode {
    std::vector<std::shared_ptr<Col>> cols;             // 选择列列表
    std::vector<std::shared_ptr<TableRef>> table_refs;  // 数据源表引用列表
    std::vector<std::shared_ptr<BinaryExpr>> conds;     // WHERE条件列表
    std::vector<std::shared_ptr<JoinExpr>> jointree;    // JOIN操作列表

    // 聚合函数和分组统计相关
    std::vector<std::shared_ptr<AggregateExpr>> agg_exprs;  // 聚合表达式列表
    std::vector<std::shared_ptr<Col>> group_by_cols;        // GROUP BY列列表
    std::vector<std::shared_ptr<BinaryExpr>> having_conds;  // HAVING条件列表

    bool has_sort;                              // 是否包含ORDER BY子句
    std::shared_ptr<OrderBy> order;             // ORDER BY子句信息
    std::shared_ptr<LimitClause> limit_clause;  // LIMIT子句信息

    // 新的构造函数，支持聚合函数和分组统计
    SelectStmt(std::vector<std::shared_ptr<Col>> cols_,
               std::vector<std::shared_ptr<TableRef>> table_refs_,
               std::vector<std::shared_ptr<BinaryExpr>> conds_,
               std::vector<std::shared_ptr<JoinExpr>> jointree_,
               std::vector<std::shared_ptr<AggregateExpr>> agg_exprs_,
               std::vector<std::shared_ptr<Col>> group_by_cols_,
               std::vector<std::shared_ptr<BinaryExpr>> having_conds_,
               std::shared_ptr<OrderBy> order_,
               std::shared_ptr<LimitClause> limit_clause_)
        : cols(std::move(cols_)), table_refs(std::move(table_refs_)), conds(std::move(conds_)), jointree(std::move(jointree_)), agg_exprs(std::move(agg_exprs_)), group_by_cols(std::move(group_by_cols_)), having_conds(std::move(having_conds_)), order(std::move(order_)), limit_clause(std::move(limit_clause_)) {
        has_sort = (bool)order;  // 根据order指针是否为空判断是否有排序
    }

    // 保持向后兼容的构造函数
    SelectStmt(std::vector<std::shared_ptr<Col>> cols_,
               std::vector<std::shared_ptr<TableRef>> table_refs_,
               std::vector<std::shared_ptr<BinaryExpr>> conds_,
               std::vector<std::shared_ptr<JoinExpr>> jointree_,
               std::shared_ptr<OrderBy> order_)
        : cols(std::move(cols_)), table_refs(std::move(table_refs_)), conds(std::move(conds_)), jointree(std::move(jointree_)), agg_exprs({}), order(std::move(order_)) {
        has_sort = (bool)order;  // 根据order指针是否为空判断是否有排序
    }

    // 获取表名列表（用于兼容性）
    std::vector<std::string> get_table_names() const {
        std::vector<std::string> tabs;
        for (const auto &ref : table_refs) {
            tabs.push_back(ref->tab_name);
        }
        return tabs;
    }
};

/**
 * @brief SET语句AST节点
 *
 * 表示系统配置设置语句
 * 例如：SET enable_nestloop = TRUE
 *
 * @param set_knob_type_ 配置参数类型
 * @param bool_val_ 配置参数值
 */
struct SetStmt : public TreeNode {
    SetKnobType set_knob_type_;  // 配置参数类型
    bool bool_val_;              // 配置参数值

    SetStmt(SetKnobType &type, bool bool_value) : set_knob_type_(type), bool_val_(bool_value) {}
};

/**
 * @brief EXPLAIN语句AST节点
 *
 * 表示EXPLAIN语句，用于显示查询计划，继承自SelectStmt
 * 例如：EXPLAIN SELECT * FROM student WHERE age > 18
 */
struct ExplainStmt : public SelectStmt {
    // 继承SelectStmt的所有构造函数
    using SelectStmt::SelectStmt;

    // 默认构造函数
    ExplainStmt(std::vector<std::shared_ptr<Col>> cols_,
                std::vector<std::shared_ptr<TableRef>> table_refs_,
                std::vector<std::shared_ptr<BinaryExpr>> conds_,
                std::vector<std::shared_ptr<JoinExpr>> jointree_,
                std::shared_ptr<OrderBy> order_)
        : SelectStmt(std::move(cols_), std::move(table_refs_), std::move(conds_), std::move(jointree_), std::move(order_)) {}
};
struct SemValue {
    // ===== 基本数据类型 =====
    int sv_int;                        // 整数值
    float sv_float;                    // 浮点数值
    std::string sv_str;                // 字符串值
    bool sv_bool;                      // 布尔值
    OrderByDir sv_orderby_dir;         // 排序方向
    std::vector<std::string> sv_strs;  // 字符串列表

    // ===== AST节点指针 =====
    std::shared_ptr<TreeNode> sv_node;  // 通用AST节点指针

    // ===== 操作符类型 =====
    SvCompOp sv_comp_op;  // 比较操作符

    // ===== 类型相关 =====
    std::shared_ptr<TypeLen> sv_type_len;  // 类型长度信息

    // ===== 字段相关 =====
    std::shared_ptr<Field> sv_field;                // 单个字段
    std::vector<std::shared_ptr<Field>> sv_fields;  // 字段列表

    // ===== 表达式相关 =====
    std::shared_ptr<Expr> sv_expr;  // 表达式指针

    // ===== 聚合函数相关 =====
    AggType sv_agg_type;                                       // 聚合函数类型
    std::shared_ptr<AggregateExpr> sv_agg_expr;                // 聚合函数表达式
    std::vector<std::shared_ptr<AggregateExpr>> sv_agg_exprs;  // 聚合表达式列表
    std::shared_ptr<LimitClause> sv_limit_clause;              // LIMIT子句

    // ===== 值相关 =====
    std::shared_ptr<Value> sv_val;                // 单个值
    std::vector<std::shared_ptr<Value>> sv_vals;  // 值列表

    // ===== 列相关 =====
    std::shared_ptr<Col> sv_col;                // 单个列引用
    std::vector<std::shared_ptr<Col>> sv_cols;  // 列引用列表

    // ===== SET子句相关 =====
    std::shared_ptr<SetClause> sv_set_clause;                // 单个SET子句
    std::vector<std::shared_ptr<SetClause>> sv_set_clauses;  // SET子句列表

    // ===== 条件相关 =====
    std::shared_ptr<BinaryExpr> sv_cond;                // 单个条件
    std::vector<std::shared_ptr<BinaryExpr>> sv_conds;  // 条件列表

    // ===== 表引用相关 =====
    std::shared_ptr<TableRef> sv_table_ref;                // 单个表引用
    std::vector<std::shared_ptr<TableRef>> sv_table_refs;  // 表引用列表

    // ===== JOIN相关 =====
    std::shared_ptr<JoinExpr> sv_join_expr;                // 单个JOIN表达式
    std::vector<std::shared_ptr<JoinExpr>> sv_join_exprs;  // JOIN表达式列表
    JoinType sv_join_type;                                 // JOIN类型

    // ===== ORDER BY相关 =====
    std::shared_ptr<OrderBy> sv_orderby;                         // ORDER BY子句
    std::shared_ptr<OrderByItem> sv_orderby_item;                // 单个排序项
    std::vector<std::shared_ptr<OrderByItem>> sv_orderby_items;  // 排序项列表

    // ===== 系统配置相关 =====
    SetKnobType sv_setKnobType;  // 系统配置参数类型
};

/**
 * @brief 全局解析树指针
 *
 * 存储语法分析器生成的完整AST根节点。
 * 解析完成后，整个SQL语句的AST结构都可以通过这个指针访问。
 */
extern std::shared_ptr<ast::TreeNode> parse_tree;
};  // namespace ast

/**
 * @brief yacc语义值类型定义
 *
 * 将AST命名空间中的SemValue定义为yacc的语义值类型YYSTYPE。
 * 这使得yacc可以使用我们定义的语义值结构来传递解析过程中的数据。
 */
#define YYSTYPE ast::SemValue
