create table test (course char(10), score float);
insert into test values('Math', 95.0);
insert into test values('Math', 85.0);
insert into test values('CS', 90.0);
select course, MAX(score) from test group by course;
drop table test; 