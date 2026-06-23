-- 1. 创建数据库 db0、db1、db2
create database db0;
create database db1;
create database db2;
show databases;

-- 2. 在 db0 上建表 account（不加 UNIQUE，由代码自动为 PRIMARY KEY 建索引）
use db0;
create table account(
  id int,
  name char(16),
  balance float,
  primary key(id)
);
show tables;

-- 3a. 执行 sql_gen (分 10 次插入，每次 10000 行)
execfile "sql_gen/account00.txt";
execfile "sql_gen/account01.txt";
execfile "sql_gen/account02.txt";
execfile "sql_gen/account03.txt";
execfile "sql_gen/account04.txt";
execfile "sql_gen/account05.txt";
execfile "sql_gen/account06.txt";
execfile "sql_gen/account07.txt";
execfile "sql_gen/account08.txt";
execfile "sql_gen/account09.txt";

-- 3b. 全表扫描
select * from account;

-- 4. 点查询 / 不等值查询
select * from account where id = 12556789;
select * from account where balance = 514.35;
select * from account where name = "name56789";
select * from account where id <> 12556789;
select * from account where balance <> 514.35;
select * from account where name <> "name56789";

-- 5. 多条件 + 投影
select id, name from account where balance >= 100 and balance < 200;
select name, balance from account where balance > 500 and id <= 12501000;
select * from account where id < 12515000 and name > "name14500";
select * from account where id < 12500200 and name < "name00100";

-- 6. 唯一约束冲突 (主键已存在)
insert into account values(12556789, "name99999", 1.0);

-- 7. 索引 + 时间比较
create index idx01 on account(name);
select * from account where name = "name56789";
select * from account where name = "name45678";
select * from account where id < 12500200 and name < "name00100";
delete from account where name = "name45678";
insert into account values(12545678, "name45678", 456.78);
drop index idx01;
select * from account where name = "name45678";

-- 8. update
update account set id = 12556789, balance = 9999.99 where name = "name56789";
select * from account where name = "name56789";

-- 9. delete / drop
delete from account where balance = 9999.99;
select * from account where balance = 9999.99;
delete from account;
select * from account;
drop table account;
show tables;

-- quit
quit;
