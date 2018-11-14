CREATE SCHEMA on_conflict;
SET search_path TO on_conflict, public;
SET citus.next_shard_id TO 1900000;

SET client_min_messages to debug1;

CREATE TABLE target_table(col_1 int primary key, col_2 int);
SELECT create_distributed_table('target_table','col_1');
INSERT INTO target_table VALUES(1,2),(2,3),(3,4),(4,5),(5,6);

CREATE TABLE source_table_1(col_1 int, col_2 int, col_3 int);
SELECT create_distributed_table('source_table_1','col_1');
INSERT INTO source_table_1 VALUES(1,1,1),(2,2,2),(3,3,3),(4,4,4),(5,5,5);

CREATE TABLE source_table_2(col_1 int, col_2 int, col_3 int);
SELECT create_distributed_table('source_table_2','col_1');
INSERT INTO source_table_2 VALUES(6,6,6),(7,7,7),(8,8,8),(9,9,9),(10,10,10);

-- Generate series directly on the coordinator and on conflict do nothing
INSERT INTO target_table (col_1, col_2) 
SELECT 
	s, s 
FROM 
	generate_series(1,10) s 
ON CONFLICT DO NOTHING;

-- Generate series directly on the coordinator and on conflict update the target table
INSERT INTO target_table (col_1, col_2) 
SELECT s, s 
FROM 
	generate_series(1,10) s 
ON CONFLICT(col_1) DO UPDATE SET col_2 = EXCLUDED.col_2 + 1;

-- Since partition columns do not match, pull the data to the coordinator
-- and do not change conflicted values
INSERT INTO target_table
SELECT 
	col_2, col_3 
FROM
	source_table_1
ON CONFLICT DO NOTHING;

-- Since partition columns do not match, pull the data to the coordinator
-- and update the non-partition column
INSERT INTO target_table
SELECT 
	col_2, col_3 
FROM
	source_table_1
ON CONFLICT(col_1) DO UPDATE SET col_2 = EXCLUDED.col_2 RETURNING *;

-- Subquery should be recursively planned due to the limit and do nothing on conflict
INSERT INTO target_table
SELECT 
	col_1, col_2
FROM (
	SELECT 
		col_1, col_2, col_3 
	FROM
		source_table_1
	LIMIT 5
) as foo
ON CONFLICT DO NOTHING;

-- Subquery should be recursively planned due to the limit and update on conflict
INSERT INTO target_table
SELECT 
	col_1, col_2
FROM (
	SELECT 
		col_1, col_2, col_3 
	FROM
		source_table_1
	LIMIT 5
) as foo
ON CONFLICT(col_1) DO UPDATE SET col_2 = EXCLUDED.col_2 RETURNING *;

-- Test with multiple subqueries
INSERT INTO target_table
SELECT 
	col_1, col_2
FROM (
	(SELECT 
		col_1, col_2, col_3 
	FROM
		source_table_1
	LIMIT 5)
	UNION
	(SELECT
		col_1, col_2, col_3
	FROM
		source_table_2
	LIMIT 5)
) as foo
ON CONFLICT(col_1) DO UPDATE SET col_2 = 0 RETURNING *;

-- Get the select part from cte and do nothing on conflict
WITH cte AS(
	SELECT col_1, col_2 FROM source_table_1
)
INSERT INTO target_table SELECT * FROM cte ON CONFLICT DO NOTHING;

-- Get the select part from cte and update on conflict
WITH cte AS(
	SELECT col_1, col_2 FROM source_table_1
)
INSERT INTO target_table SELECT * FROM cte ON CONFLICT(col_1) DO UPDATE SET col_2 = EXCLUDED.col_2 + 1 RETURNING *;

-- Test with multiple CTEs
WITH cte AS(
	SELECT col_1, col_2 FROM source_table_1
), cte_2 AS(
	SELECT col_1, col_2 FROM source_table_2
)
INSERT INTO target_table ((SELECT * FROM cte) UNION (SELECT * FROM cte_2)) ON CONFLICT(col_1) DO UPDATE SET col_2 = EXCLUDED.col_2 + 1 RETURNING *;

WITH cte AS(
	SELECT col_1, col_2, col_3 FROM source_table_1
), cte_2 AS(
	SELECT col_1, col_2 FROM cte
)
INSERT INTO target_table SELECT * FROM cte_2 ON CONFLICT(col_1) DO UPDATE SET col_2 = EXCLUDED.col_2 + 1 RETURNING 

-- Test with shard_replication_factor = 2

SET citus.shard_replication_factor to 2;

DROP TABLE target_table, source_table_1, source_table_2;

CREATE TABLE target_table(col_1 int primary key, col_2 int);
SELECT create_distributed_table('target_table','col_1');
INSERT INTO target_table VALUES(1,2),(2,3),(3,4),(4,5),(5,6);

CREATE TABLE source_table_1(col_1 int, col_2 int, col_3 int);
SELECT create_distributed_table('source_table_1','col_1');
INSERT INTO source_table_1 VALUES(1,1,1),(2,2,2),(3,3,3),(4,4,4),(5,5,5);

CREATE TABLE source_table_2(col_1 int, col_2 int, col_3 int);
SELECT create_distributed_table('source_table_2','col_1');
INSERT INTO source_table_2 VALUES(6,6,6),(7,7,7),(8,8,8),(9,9,9),(10,10,10);

-- Generate series directly on the coordinator and on conflict do nothing
INSERT INTO target_table (col_1, col_2) 
SELECT 
	s, s 
FROM 
	generate_series(1,10) s 
ON CONFLICT DO NOTHING;

-- Test with multiple subqueries
INSERT INTO target_table
SELECT 
	col_1, col_2
FROM (
	(SELECT 
		col_1, col_2, col_3 
	FROM
		source_table_1
	LIMIT 5)
	UNION
	(SELECT
		col_1, col_2, col_3
	FROM
		source_table_2
	LIMIT 5)
) as foo
ON CONFLICT(col_1) DO UPDATE SET col_2 = 0 RETURNING *;

WITH cte AS(
	SELECT col_1, col_2, col_3 FROM source_table_1
), cte_2 AS(
	SELECT col_1, col_2 FROM cte
)
INSERT INTO target_table SELECT * FROM cte_2 ON CONFLICT(col_1) DO UPDATE SET col_2 = EXCLUDED.col_2 + 1 RETURNING *;

RESET client_min_messages;
DROP SCHEMA on_conflict CASCADE;