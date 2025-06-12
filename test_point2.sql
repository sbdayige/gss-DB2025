create table grade (course char(20),id int,score float);

insert into grade values('DataStructure',1,95);
insert into grade values('DataStructure',2,93.5);
insert into grade values('DataStructure',3,94.5);
insert into grade values('ComputerNetworks',1,99);
insert into grade values('ComputerNetworks',2,88.5);
insert into grade values('ComputerNetworks',3,92.5);
insert into grade values('C++',1,92);
insert into grade values('C++',2,89);
insert into grade values('C++',3,89.5);

select id,max(score) as max_score,min(score) as min_score,sum(score) as sum_score from grade group by id;

select id,max(score) as max_score from grade group by id having count(*) > 3;

insert into grade values ('ParallelCompute',1,100);

select id,max(score) as max_score from grade group by id having count(*) > 3;

select id,max(score) as max_score,min(score) as min_score from grade group by id having count(*) > 1 and min(score) > 88;

select course ,COUNT(*) as row_num , COUNT(id) as student_num , MAX(score) as top_score, MIN(score) as lowest_score from grade group by course;

select course, id, score from grade order by score desc;

drop table grade;
