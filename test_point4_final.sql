create table records (vendor char(5), invoice_number int, amount float);

insert into records values('alpha', 1001, 98.0);
insert into records values('bravo', 2002, 76.5);
insert into records values('charl', 3003, 99.0);
insert into records values('delta', 1001, 98.5);
insert into records values('echoo', 4004, 88.25);
insert into records values('foxxx', 4004, 77.0);
insert into records values('golfy', 5005, 97.75);
insert into records values('hotel', 5005, 86.75);
insert into records values('indio', 6006, 76.25);
insert into records values('julie', 3003, 88.0);
insert into records values('karen', 5005, 89.25);
insert into records values('lenny', 2002, 91.125);
insert into records values('mango', 6006, 98.5);
insert into records values('nancy', 1001, 89.75);
insert into records values('oscar', 2002, 90.0);
insert into records values('peter', 3003, 95.0);
insert into records values('quack', 6006, 88.625);
insert into records values('romeo', 4004, 92.0);
insert into records values('sunny', 1001, 95.25);
insert into records values('tonny', 7007, 98.125);
insert into records values('ultra', 4004, 91.5);
insert into records values('vivid', 7007, 98.3125);

select * from records order by invoice_number asc limit 2;

select * from records where invoice_number = 1001 order by amount asc;

drop table records; 