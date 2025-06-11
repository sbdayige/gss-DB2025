-- 题目二测试用例
-- 测试点1: 尝试建表
create table t1(id int,name char(4));
show tables;
create table t2(id int);
show tables;
drop table t1;
show tables;
drop table t2;
show tables;

-- 测试点2: 单表插入与条件查询
create table grade (name char(20),id int,score float);
insert into grade values ('Data Structure', 1, 90.5);
insert into grade values ('Data Structure', 2, 95.0);
insert into grade values ('Calculus', 2, 92.0);
insert into grade values ('Calculus', 1, 88.5);
select * from grade;
select score,name,id from grade where score > 90;
select id from grade where name = 'Data Structure';
select name from grade where id = 2 and score > 90;

-- 测试点3：单表更新与条件查询
create table grade2 (name char(20),id int,score float);
insert into grade2 values ('Data Structure', 1, 90.5);
insert into grade2 values ('Data Structure', 2, 95.0);
insert into grade2 values ('Calculus', 2, 92.0);
insert into grade2 values ('Calculus', 1, 88.5);
select * from grade2;
update grade2 set score = 90 where name = 'Calculus';
select * from grade2;

-- 测试点4：单表删除与条件查询
create table grade3 (name char(20),id int,score float);
insert into grade3 values ('Data Structure', 1, 90.5);
select * from grade3;
delete from grade3 where score > 90;
select * from grade3;

-- 测试点5：连接查询
create table t (id int, t_name char(3));
create table d (d_name char(5),id int);
insert into t values (1,'aaa');
insert into t values (2,'baa');
insert into t values (3,'bba');
insert into d values ('12345',1);
insert into d values ('23456',2);
select * from t, d;
select t.id,t_name,d_name from t,d where t.id = d.id; 