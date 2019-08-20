set statement_mem="1800";
create table cost_agg_t1(a int, b int, c int);
insert into cost_agg_t1 select i, random() * 99999, i % 2000 from generate_series(1, 1000000) i;
create table cost_agg_t2 as select * from cost_agg_t1 with no data;
insert into cost_agg_t2 select i, random() * 99999, i % 300000 from generate_series(1, 1000000) i;
explain select avg(b) from cost_agg_t1 group by c;
explain select avg(b) from cost_agg_t2 group by c;
insert into cost_agg_t2 select i, random() * 99999,1 from generate_series(1, 200000) i;
analyze cost_agg_t2;
explain select avg(b) from cost_agg_t2 group by c;
set statement_mem = '125MB';
