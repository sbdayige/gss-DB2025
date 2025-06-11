create table grade (course char(20),id int,score float);

insert into grade values('DataStructure',1,95);
insert into grade values('DataStructure',2,93.5);
insert into grade values('DataStructure',4,87);
insert into grade values('DataStructure',3,85);
insert into grade values('DB',1,94);
insert into grade values('DB',2,74.5);
insert into grade values('DB',4,83);
insert into grade values('DB',3,87);

-- 测试基本查询
select * from grade;

-- 测试WHERE条件
select * from grade where course = 'DataStructure';
select * from grade where course = 'DB';
select * from grade where score < 60;
select * from grade where id = 1;

drop table grade; 