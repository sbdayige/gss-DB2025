CREATE TABLE test_table (id int, value float);
INSERT INTO test_table VALUES (1, 10.0);
INSERT INTO test_table VALUES (2, 20.0);
INSERT INTO test_table VALUES (3, 15.0);
SELECT * FROM test_table;
SELECT MAX(id) FROM test_table; 