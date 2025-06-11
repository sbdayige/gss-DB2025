create table grade (course char(20),id int,score float);

insert into grade values('DataStructure',1,95);
insert into grade values('DataStructure',2,93.5);
insert into grade values('DataStructure',4,87);
insert into grade values('DataStructure',3,85);
insert into grade values('DB',1,94);
insert into grade values('DB',2,74.5);
insert into grade values('DB',4,83);
insert into grade values('DB',3,87);

select MAX(id) as max_id from grade;
select MIN(score) as min_score from grade where course = 'DB';
select AVG(score) as avg_score from grade where course = 'DataStructure';
select COUNT(course) as course_num from grade;
select COUNT(*) as row_num from grade;
select COUNT(*) as row_num from grade where score < 60;
select SUM(score) as sum_score from grade where id = 1;

drop table grade; 