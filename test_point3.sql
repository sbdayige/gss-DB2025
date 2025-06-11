create table grade (course char(20),id int,score float);

insert into grade values('DataStructure',1,95);
insert into grade values('DataStructure',2,93.5);
insert into grade values('DataStructure',3,94.5);
insert into grade values('ComputerNetworks',1,99);
insert into grade values('ComputerNetworks',2,88.5);
insert into grade values('ComputerNetworks',3,92.5);

-- SELECT 列表中不能出现没有在 GROUP BY 子句中的非聚集列
select id , score from grade group by course;

-- WHERE 子句中不能用聚集函数作为条件表达式
select id, MAX(score) as max_score from grade where MAX(score) > 90 group by id;

drop table grade;
