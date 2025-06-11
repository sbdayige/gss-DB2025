create table test (id int, score float);
insert into test values(1, 95.0);
insert into test values(2, 85.0);
select MAX(score) from test;
select COUNT(*) from test;
drop table test; 