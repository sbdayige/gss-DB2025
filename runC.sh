#!/bin/bash

# 检查客户端程序是否存在
if [ ! -f "rmdb_client/build/rmdb_client" ]; then
    echo "错误: rmdb_client 不存在，请先编译项目"
    exit 1
fi

# 创建测试SQL文件

cat > test_commands.sql << EOF

-- 测试1: 多列索引创建和展示
create table warehouse (w_id int, w_name char(8), w_city char(10), w_tax float);
insert into warehouse values (10, 'WH_A', 'Shanghai', 0.05);
insert into warehouse values (20, 'WH_B', 'Beijing', 0.06);
insert into warehouse values (30, 'WH_C', 'Guangzhou', 0.07);
insert into warehouse values (40, 'WH_D', 'Shenzhen', 0.08);
insert into warehouse values (50, 'WH_E', 'Hangzhou', 0.09);

-- 创建单列索引
create index warehouse (w_id);
show index from warehouse;

-- 创建双列索引
create index warehouse (w_id, w_name);
show index from warehouse;

-- 创建三列索引
create index warehouse (w_name, w_city, w_tax);
show index from warehouse;

-- 测试2: 多列索引查询 - 最左匹配原则
-- 使用双列索引 (w_id, w_name)
select * from warehouse where w_id = 10 and w_name = 'WH_A';
select * from warehouse where w_id = 20;
select * from warehouse where w_id > 10 and w_id < 40;

-- 使用三列索引 (w_name, w_city, w_tax) 
select * from warehouse where w_name = 'WH_B' and w_city = 'Beijing' and w_tax = 0.06;
select * from warehouse where w_name = 'WH_C' and w_city = 'Guangzhou';
select * from warehouse where w_name = 'WH_D';
select * from warehouse where w_name > 'WH_A' and w_name < 'WH_E';

-- 测试3: 多列索引维护 - 插入操作
insert into warehouse values (60, 'WH_F', 'Tianjin', 0.10);
select * from warehouse where w_id = 60;
select * from warehouse where w_name = 'WH_F';

-- 测试4: 多列索引维护 - 更新操作
update warehouse set w_name = 'WH_NEW' where w_id = 10;
select * from warehouse where w_id = 10;
select * from warehouse where w_name = 'WH_NEW';

-- 测试5: 多列索引维护 - 删除操作
delete from warehouse where w_id = 20;
select * from warehouse where w_id = 20;
select * from warehouse where w_name = 'WH_B';

-- 测试6: 复杂表结构的多列索引（修正数据以符合唯一索引约束）
create table orders (o_id int, o_c_id int, o_d_id int, o_w_id int, o_entry_d char(19));
insert into orders values (1, 100, 1, 10, '2023-01-01 10:00:00');
insert into orders values (2, 101, 2, 20, '2023-01-01 11:00:00');
insert into orders values (3, 102, 3, 30, '2023-01-01 12:00:00');
insert into orders values (4, 103, 4, 40, '2023-01-01 13:00:00');
insert into orders values (5, 104, 5, 50, '2023-01-01 14:00:00');

-- 创建多个多列索引（确保唯一性）
create index orders (o_w_id, o_d_id);
create index orders (o_c_id, o_entry_d);
create index orders (o_w_id, o_d_id, o_c_id);

show index from orders;

-- 测试多列索引查询
select * from orders where o_w_id = 10 and o_d_id = 1;
select * from orders where o_w_id = 20;
select * from orders where o_c_id = 101 and o_entry_d = '2023-01-01 11:00:00';
select * from orders where o_c_id = 100;

-- ===========================================
-- 事务回滚与多列索引测试
-- ===========================================

-- 测试7: 无索引情况下的事务回滚
create table student (id int, name char(8), score float);
insert into student values (1, 'xiaohong', 90.0);

begin;
insert into student values (2, 'xiaoming', 99.0);
insert into student values (3, 'xiaogang', 85.0);
delete from student where id = 1;
abort;

select * from student;

-- 测试8: 单列索引情况下的事务回滚
create index student (id);

begin;
insert into student values (4, 'xiaoli', 95.0);
update student set score = 100.0 where id = 1;
delete from student where id = 1;
abort;

select * from student;

-- 测试9: 多列索引情况下的事务回滚
create index student (id, name);

begin;
insert into student values (5, 'xiaowang', 88.0);
insert into student values (6, 'xiaozhao', 92.0);
update student set name = 'newhong' where id = 1;
delete from student where id = 1;
abort;

select * from student;
select * from student where id = 1 and name = 'xiaohong';

-- 测试10: 复杂事务回滚 - 多表多索引
create table customer (c_id int, c_name char(10), c_city char(8));
insert into customer values (100, 'Customer1', 'City1');
insert into customer values (101, 'Customer2', 'City2');

create index customer (c_id);
create index customer (c_id, c_name);

begin;
-- 插入新客户
insert into customer values (102, 'Customer3', 'City3');
insert into customer values (103, 'Customer4', 'City4');

-- 更新现有客户
update customer set c_name = 'UpdatedC1' where c_id = 100;
update customer set c_city = 'NewCity' where c_id = 101;

-- 删除客户
delete from customer where c_id = 101;

-- 在orders表中插入订单（确保唯一性）
insert into orders values (6, 102, 6, 60, '2023-01-02 10:00:00');
insert into orders values (7, 103, 7, 70, '2023-01-02 11:00:00');

-- 更新订单
update orders set o_c_id = 105 where o_id = 1;

abort;

-- 验证回滚结果
select * from customer;
select * from orders;
select * from customer where c_id = 100 and c_name = 'Customer1';

-- 测试11: 提交事务后的索引一致性
begin;
insert into customer values (104, 'Customer5', 'City5');
update customer set c_name = 'ModifiedC1' where c_id = 100;
delete from customer where c_id = 101;
commit;

select * from customer;
select * from customer where c_id = 100;
select * from customer where c_id = 104;

-- 测试12: 唯一索引约束测试
create table product (p_id int, p_name char(10), p_category char(8));
insert into product values (1, 'Product1', 'Cat1');
insert into product values (2, 'Product2', 'Cat2');

create index product (p_id);

-- 测试唯一约束
begin;
insert into product values (3, 'Product3', 'Cat3');
update product set p_name = 'Updated1' where p_id = 1;
commit;

select * from product;

-- 测试13: 范围查询与多列索引
create table item (i_id int, i_name char(12), i_price float, i_data char(50));
insert into item values (1, 'Item1', 10.5, 'Data for item 1');
insert into item values (2, 'Item2', 20.0, 'Data for item 2');
insert into item values (3, 'Item3', 15.5, 'Data for item 3');
insert into item values (4, 'Item4', 25.0, 'Data for item 4');
insert into item values (5, 'Item5', 30.0, 'Data for item 5');

create index item (i_price, i_id);
create index item (i_name, i_price);

-- 范围查询测试
select * from item where i_price > 15.0 and i_price < 30.0;
select * from item where i_name > 'Item2' and i_name < 'Item5';
select * from item where i_price > 20.0 and i_id > 2;

-- 测试14: 事务中的索引范围查询回滚
begin;
insert into item values (6, 'Item6', 35.0, 'Data for item 6');
insert into item values (7, 'Item7', 40.0, 'Data for item 7');
update item set i_price = 50.0 where i_id = 5;
delete from item where i_id = 1;
abort;

-- 验证范围查询
select * from item where i_price > 15.0 and i_price < 35.0;
select * from item where i_id >= 1 and i_id <= 5;

-- 测试15: 清理测试 - 删除索引
drop index warehouse (w_id);
drop index warehouse (w_id, w_name);
drop index warehouse (w_name, w_city, w_tax);
show index from warehouse;

drop index orders (o_w_id, o_d_id);
drop index orders (o_c_id, o_entry_d);
drop index orders (o_w_id, o_d_id, o_c_id);
show index from orders;

drop index student (id);
drop index student (id, name);
show index from student;

-- 最终验证 - 无索引情况下的查询
select * from warehouse where w_id = 30;
select * from orders where o_w_id = 10;
select * from student where id = 1;

EOF

echo "正在启动数据库客户端并执行测试命令..."
echo "=========================================="

# 运行客户端并传入测试命令
rmdb_client/build/rmdb_client < stress_test_multi_index.sql

echo "=========================================="
echo "测试完成"

# 清理临时文件
rm -f test_commands.sql

# cd /home/guo/code/copy/gss-DB2025 && chmod +x runC.sh && ./runC.sh