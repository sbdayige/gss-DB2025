-- 题目五：聚合函数与分组统计 测试脚本

-- 测试点1：单独使用聚合函数
CREATE TABLE grade (course char(20), id int, score float);

INSERT INTO grade VALUES('DataStructure', 1, 95);
INSERT INTO grade VALUES('DataStructure', 2, 93.5);
INSERT INTO grade VALUES('DataStructure', 4, 87);
INSERT INTO grade VALUES('DataStructure', 3, 85);
INSERT INTO grade VALUES('DB', 1, 94);
INSERT INTO grade VALUES('DB', 2, 74.5);
INSERT INTO grade VALUES('DB', 4, 83);
INSERT INTO grade VALUES('DB', 3, 87);

-- 测试基本查询功能是否正常
SELECT * FROM grade;

-- 测试聚合函数
SELECT MAX(id) AS max_id FROM grade;
SELECT MIN(score) AS min_score FROM grade WHERE course = 'DB';
SELECT AVG(score) AS avg_score FROM grade WHERE course = 'DataStructure';
SELECT COUNT(course) AS course_num FROM grade;
SELECT COUNT(*) AS row_num FROM grade;
SELECT COUNT(*) AS row_num FROM grade WHERE score < 60;
SELECT SUM(score) AS sum_score FROM grade WHERE id = 1;

DROP TABLE grade; 