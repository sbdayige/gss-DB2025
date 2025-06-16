create table records (vendor char(5), invoice_number int, amount float);

insert into records values('alpha', 1001, 98.0);
insert into records values('bravo', 2002, 76.5);
insert into records values('charl', 3003, 99.0);
insert into records values('delta', 1001, 98.5);
insert into records values('nancy', 1001, 89.75);
insert into records values('sunny', 1001, 95.25);

-- 测试1：多列排序，先按invoice_number升序，再按amount升序
select * from records order by invoice_number asc, amount asc;

-- 测试2：多列排序，先按invoice_number升序，再按amount降序  
select * from records order by invoice_number asc, amount desc;

-- 测试3：多列排序，先按invoice_number降序，再按amount升序
select * from records order by invoice_number desc, amount asc;

drop table records; 