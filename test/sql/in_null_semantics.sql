-- NULL semantics of the IN family (ScalarArrayOpExpr pushdown).
--
-- ClickHouse evaluates IN under two-valued logic: a probe that finds no
-- match yields 0 even when the searched set contains a NULL, where Postgres
-- computes NULL; The ClickHouse has()/countEqual() functions also match a NULL probe as
-- if it were an ordinary value. A FALSE-for-NULL substitution is invisible
-- while the result only qualifies rows (WHERE/JOIN/HAVING through AND/OR),
-- but observable under NOT and anywhere the boolean's value is consumed.
-- These tests ensure the shapes deparse.c ships and that the rows returned
-- match local (Postgres) semantics either way.
CREATE SERVER in_null_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'in_null_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER in_null_svr;

SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS in_null_test');
SELECT clickhouse_raw_query('CREATE DATABASE in_null_test');

-- xn imports as a plain (nullable) int column, xp as int NOT NULL
SELECT clickhouse_raw_query('CREATE TABLE in_null_test.tnull
    (id Int32, xn Nullable(Int32), xp Int32)
    ENGINE = MergeTree ORDER BY id');
SELECT clickhouse_raw_query($$
    INSERT INTO in_null_test.tnull VALUES (1, 1, 1), (2, 2, 2), (3, NULL, 3)
$$);

CREATE SCHEMA in_null_test;
IMPORT FOREIGN SCHEMA in_null_test FROM SERVER in_null_svr INTO in_null_test;
SET SESSION search_path = in_null_test,public;

-- The import must carry ClickHouse nullability into the declarations the
-- never-null proofs rely on
\d tnull

-- ============================================================
-- 1. IN over a constant list: pushable in a WHERE condition
-- ============================================================
-- NULL-free list is exactly equivalent
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xn IN (1, 500) ORDER BY id;
SELECT id FROM tnull WHERE xn IN (1, 500) ORDER BY id;

-- A NULL in the list only pulls results toward FALSE, which a WHERE treats
-- like the NULL Postgres computes: still pushable
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xn IN (1, NULL) ORDER BY id;
SELECT id FROM tnull WHERE xn IN (1, NULL) ORDER BY id;

-- ============================================================
-- 2. NOT IN over a constant list: pushable only when NULL-free
-- ============================================================
-- ClickHouse's native NOT IN yields NULL for a NULL probe, so a NULL-free
-- list is exact: id 3 (xn IS NULL) is dropped on both systems
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xn NOT IN (1, 500) ORDER BY id;
SELECT id FROM tnull WHERE xn NOT IN (1, 500) ORDER BY id;

-- With a NULL in the list Postgres returns no rows (every result is NULL
-- or FALSE) where ClickHouse would return id 2: must stay local
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xn NOT IN (1, NULL) ORDER BY id;
SELECT id FROM tnull WHERE xn NOT IN (1, NULL) ORDER BY id;

-- NOT over IN is the same thing arriving unrewritten
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE NOT (xn IN (1, NULL)) ORDER BY id;
SELECT id FROM tnull WHERE NOT (xn IN (1, NULL)) ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE NOT (xn IN (1, 500)) ORDER BY id;
SELECT id FROM tnull WHERE NOT (xn IN (1, 500)) ORDER BY id;

-- ============================================================
-- 3. Non-constant arrays: the has()/countEqual() deparse forms
-- ============================================================
-- has() matches a NULL probe against a NULL element where Postgres computes
-- NULL, so = ANY needs a provably non-NULL probe; xp is declared NOT NULL
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xp = ANY(ARRAY[xn, 5]) ORDER BY id;
SELECT id FROM tnull WHERE xp = ANY(ARRAY[xn, 5]) ORDER BY id;

-- A nullable probe against an array that may hold a NULL would turn
-- Postgres's NULL (id 3: NULL = ANY({NULL,5})) into TRUE: must stay local
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xn = ANY(ARRAY[xn, 5]) ORDER BY id;
SELECT id FROM tnull WHERE xn = ANY(ARRAY[xn, 5]) ORDER BY id;

-- countEqual() = 0 turns any NULL involved into TRUE (id 3: 3 <> ALL of
-- {NULL,5} is NULL on Postgres): <> ALL needs both sides provably non-NULL
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xp <> ALL(ARRAY[xn, 5]) ORDER BY id;
SELECT id FROM tnull WHERE xp <> ALL(ARRAY[xn, 5]) ORDER BY id;

-- ...and with both sides proven, it ships
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xp <> ALL(ARRAY[xp, 500]) ORDER BY id;
SELECT id FROM tnull WHERE xp <> ALL(ARRAY[xp, 500]) ORDER BY id;

-- <> ANY has no correct deparse (not has() computes <> ALL: it would return
-- no rows here, where one differing element makes every row qualify): local
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xp <> ANY(ARRAY[xp, 500]) ORDER BY id;
SELECT id FROM tnull WHERE xp <> ANY(ARRAY[xp, 500]) ORDER BY id;

-- = ALL ships with both sides proven non-NULL...
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xp = ALL(ARRAY[xp]) ORDER BY id;
SELECT id FROM tnull WHERE xp = ALL(ARRAY[xp]) ORDER BY id;

-- ...but countEqual() counts NULL as equal to NULL (id 3 would come back
-- where Postgres computes NULL = ALL({NULL}) as NULL): local
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xn = ALL(ARRAY[xn]) ORDER BY id;
SELECT id FROM tnull WHERE xn = ALL(ARRAY[xn]) ORDER BY id;

-- ============================================================
-- 4. Value contexts observe the difference even without NOT
-- ============================================================
-- Grouping on the IN result exposes FALSE-vs-NULL directly: Postgres groups
-- id 2 and id 3 together under NULL; ClickHouse would put id 2 under FALSE.
-- A NULL-free list is exact and ships; a NULL in the list keeps it local.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT xn IN (1, 500) AS flag, count(*) FROM tnull GROUP BY flag ORDER BY flag;
SELECT xn IN (1, 500) AS flag, count(*) FROM tnull GROUP BY flag ORDER BY flag;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT xn IN (1, NULL) AS flag, count(*) FROM tnull GROUP BY flag ORDER BY flag;
SELECT xn IN (1, NULL) AS flag, count(*) FROM tnull GROUP BY flag ORDER BY flag;

-- An aggregate FILTER only qualifies rows, so it keeps truth-context rules
EXPLAIN (VERBOSE, COSTS OFF)
SELECT count(*) FILTER (WHERE xn IN (1, NULL)) FROM tnull;
SELECT count(*) FILTER (WHERE xn IN (1, NULL)) FROM tnull;

-- ============================================================
-- 5. Shippability-walker regressions fixed alongside the rules
-- ============================================================
-- A NULL array constant survives const-folding when the probe is a column;
-- inspecting it for the IN-list deparse would detoast a null datum. Local.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xn = ANY(NULL::int[]) ORDER BY id;
SELECT id FROM tnull WHERE xn = ANY(NULL::int[]) ORDER BY id;

-- A CASE arg WHEN .. with an unshippable tested expression shipped without
-- its branches ever being checked. Local, with correct rows.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE CASE pg_backend_pid() WHEN 0 THEN xp = 1 ELSE xp > 0 END
ORDER BY id;
SELECT id FROM tnull WHERE CASE pg_backend_pid() WHEN 0 THEN xp = 1 ELSE xp > 0 END
ORDER BY id;

-- Cleanup
SET SESSION search_path = public;
DROP SCHEMA in_null_test CASCADE;
SELECT clickhouse_raw_query('DROP DATABASE in_null_test');
DROP USER MAPPING FOR CURRENT_USER SERVER in_null_svr;
DROP SERVER in_null_svr;
