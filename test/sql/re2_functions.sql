SELECT EXISTS(SELECT 1 FROM pg_available_extensions WHERE name = 're2') AS have_re2 \gset
\if :have_re2

CREATE SERVER re2_svr FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 're2_test');
CREATE USER MAPPING FOR CURRENT_USER SERVER re2_svr;

SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS re2_test');
SELECT clickhouse_raw_query('CREATE DATABASE re2_test');
SELECT clickhouse_raw_query($$
    CREATE TABLE re2_test.t1 (
        id   Int32,
        val  String
    ) ENGINE = MergeTree ORDER BY id
$$);
SELECT clickhouse_raw_query($$
    INSERT INTO re2_test.t1 VALUES
        (1, 'POSIX uses BRE and ERE'),
        (2, 're2 uses finite automata'),
        (3, 'PCRE supports backtracking')
$$);

CREATE SCHEMA re2_test;
IMPORT FOREIGN SCHEMA re2_test FROM SERVER re2_svr INTO re2_test;
SET search_path = re2_test, public;

CREATE EXTENSION re2;

EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE re2match(val, 're2');
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE re2extract(val, '(re2)') = 're2';
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE re2extractall(val, '[A-Z]+') = ARRAY['POSIX','BRE','ERE'];
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE re2regexpextract(val, '(re2)', 1) = 're2';
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE re2extractgroups(val, '(POSIX) uses (BRE)') = ARRAY['POSIX','BRE'];
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE re2replaceregexpone(val, 'POSIX', 're2') = 're2 uses BRE and ERE';
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE re2replaceregexpall(val, ' ', '-') = 're2-uses-finite-automata';
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE re2countmatches(val, 'e') > 0;
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE re2countmatchescaseinsensitive(val, 'E') > 0;
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE re2multimatchany(val, 'POSIX','PCRE');
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE re2multimatchanyindex(val, 'POSIX','PCRE') > 0;
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE re2multimatchallindices(val, 'POSIX','PCRE') = ARRAY[1];

DROP EXTENSION re2;
DROP USER MAPPING FOR CURRENT_USER SERVER re2_svr;
SELECT clickhouse_raw_query('DROP DATABASE re2_test');
DROP SERVER re2_svr CASCADE;

\else
\echo 'SKIP: re2 extension not available'
\endif
