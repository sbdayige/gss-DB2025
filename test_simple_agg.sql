-- 简单的聚合函数测试
CREATE TABLE test_scores (id int, score float);
INSERT INTO test_scores VALUES (1, 95.5);
INSERT INTO test_scores VALUES (2, 87.0);
INSERT INTO test_scores VALUES (3, 92.5);
INSERT INTO test_scores VALUES (4, 78.0);
INSERT INTO test_scores VALUES (5, 88.5);

-- 测试基本查询
SELECT * FROM test_scores;

-- 测试COUNT函数
SELECT COUNT(*) FROM test_scores;

-- 简单聚合函数测试
-- 确保题目二功能正常
CREATE TABLE grade (course char(20), id int, score float);
INSERT INTO grade VALUES('DataStructure', 1, 95);
INSERT INTO grade VALUES('DataStructure', 2, 93.5);
INSERT INTO grade VALUES('DataStructure', 4, 87);
INSERT INTO grade VALUES('DataStructure', 3, 85);
INSERT INTO grade VALUES('DB', 1, 94);
INSERT INTO grade VALUES('DB', 2, 74.5);
INSERT INTO grade VALUES('DB', 4, 83);
INSERT INTO grade VALUES('DB', 3, 87);

-- 测试基本查询（题目二）
SELECT * FROM grade;
SELECT * FROM grade WHERE course = 'DB';

-- 测试聚合函数（题目五）
SELECT COUNT(*) FROM grade;
-- SELECT MAX(id) FROM grade;
-- SELECT MIN(score) FROM grade WHERE course = 'DB';
-- SELECT AVG(score) FROM grade WHERE course = 'DataStructure';

DROP TABLE grade;

create table test (id int, score float);
insert into test values(1, 95.0);
insert into test values(2, 85.0);
select MAX(score) from test;
select COUNT(*) from test;
drop table test; 