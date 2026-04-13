\unset ECHO
CREATE SERVER regex_loopback FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'regex_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER regex_loopback;

SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS regex_test');
SELECT clickhouse_raw_query('CREATE DATABASE regex_test');

SELECT clickhouse_raw_query($$
	CREATE TABLE regex_test.strings(val String) engine=TinyLog()
$$);

SELECT clickhouse_raw_query($$
	INSERT INTO regex_test.strings
	VALUES ('val1'),
		   ('val2'),
		   ('a,b,c'),
		   ('foo,bar,baz'),
		   ('sleep   no   more'),
		   ('aa-T-bb-t-cc')
$$);

CREATE FOREIGN TABLE strings (val text) SERVER regex_loopback;


\unset ECHO
-- Use DO to test functions available in Postgres 15+
-- PG15+: trim_array → match()
DO $_$
DECLARE
    rec record;
    tc jsonb;
	output JSONB;
    tests jsonb[] := ARRAY[
        $${
          "func": "regexp_like",
          "where": "regexp_like(val, '^val\\d')",
          "push": "(match(val, concat('(?-s)', '^val\\\\d')))"
        }$$,
        $${
          "func": "case-insensitive regexp_like",
          "where": "regexp_like(val, '^VAL\\d', 'i')",
          "push": "(match(val, concat('(?i-s)', '^VAL\\\\d')))"
        }$$,
        $${
          "func": "regexp_like with unsupported flag",
          "where": "regexp_like(val, '^VAL\\d', 'in')"
        }$$,
        $${
          "func": "regexp_like with t flag",
          "where": "regexp_like(val, '^VAL\\d', 'it')",
          "push": "(match(val, concat('(?i-s)', '^VAL\\\\d')))"
        }$$
    ];
BEGIN
    IF current_setting('server_version_num')::int >= 150000 THEN
        FOREACH tc IN ARRAY tests LOOP
            EXECUTE format('EXPLAIN (VERBOSE, FORMAT JSON) SELECT val FROM strings WHERE %s', tc->>'where') INTO output;
            If tc ? 'push' THEN
                RAISE NOTICE '% PUSHED DOWN: %', tc->>'func', jsonb_path_query(
                    output, '$[0].Plan'
                )->>'Remote SQL' = format('SELECT val FROM regex_test.strings WHERE %s', tc->>'push');
            ELSE
                RAISE NOTICE '% NOT PUSHED DOWN: %', tc->>'func', jsonb_path_query(
                    output, '$[0].Plan'
                )->>'Remote SQL' = 'SELECT val FROM regex_test.strings';
            END IF;
            FOR rec IN EXECUTE format('SELECT val FROM strings WHERE %s', tc->>'where') LOOP
                RAISE NOTICE '%', rec;
            END LOOP;
        END LOOP;
    ELSE
        -- Fake it on earlier versions.
        RAISE NOTICE 'regexp_like PUSHED DOWN: t';
        RAISE NOTICE '(val1)';
        RAISE NOTICE '(val2)';
        RAISE NOTICE 'case-insensitive regexp_like PUSHED DOWN: t';
        RAISE NOTICE '(val1)';
        RAISE NOTICE '(val2)';
        RAISE NOTICE 'regexp_like with unsupported flag NOT PUSHED DOWN: t';
        RAISE NOTICE '(val1)';
        RAISE NOTICE '(val2)';
        RAISE NOTICE 'regexp_like with t flag PUSHED DOWN: t';
        RAISE NOTICE '(val1)';
        RAISE NOTICE '(val2)';
    END IF;
END;
$_$;

\set ECHO all

-- Check regexp_split_to_array.
EXPLAIN (VERBOSE, COSTS OFF) SELECT val FROM strings WHERE regexp_split_to_array(val, ',') = '{a,b,c}'::text[];
SELECT val FROM strings WHERE regexp_split_to_array(val, ',') = '{a,b,c}'::text[];
EXPLAIN (VERBOSE, COSTS OFF) SELECT val FROM strings WHERE regexp_split_to_array(val, '\s+') = '{sleep,no,more}'::text[];
SELECT val FROM strings WHERE regexp_split_to_array(val, '\s+') = '{sleep,no,more}'::text[];
EXPLAIN (VERBOSE, COSTS OFF) SELECT val FROM strings WHERE regexp_split_to_array(val, '-t-', 'i') = '{aa,bb,cc}'::text[];
SELECT val FROM strings WHERE regexp_split_to_array(val, '-t-', 'i') = '{aa,bb,cc}'::text[];

-- Check regexp_replace().
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM strings WHERE regexp_replace(val, '[0,1]$', '_xyz') = 'val_xyz';
SELECT * FROM strings WHERE regexp_replace(val, '[0,1]$', '_xyz') = 'val_xyz';
-- No replace returns unmodified string.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM strings WHERE regexp_replace(val, '^x', 'y') = 'val2';
SELECT * FROM strings WHERE regexp_replace(val, '^x', 'y') = 'val2';
-- Case-insensitive, refer to capture.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM strings WHERE regexp_replace(val, 'VAL([0,1])$', 'x-\1', 'i') = 'x-1';
SELECT * FROM strings WHERE regexp_replace(val, 'VAL([0,1])$', 'x-\1', 'i') = 'x-1';
-- Case-insensitive.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM strings WHERE regexp_replace(val, '[VL]', 'x', 'i') = 'xal1';
SELECT * FROM strings WHERE regexp_replace(val, '[VL]', 'x', 'i') = 'xal1';
-- Replace all case-insensitive.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM strings WHERE regexp_replace(val, '[VL]', 'x', 'gig') = 'xax1';
SELECT * FROM strings WHERE regexp_replace(val, '[VL]', 'x', 'gig') = 'xax1';
-- Refer to full match.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM strings WHERE regexp_replace(val, '^val', 'x-\0', 'i') = 'x-val1';
SELECT * FROM strings WHERE regexp_replace(val, '^val', 'x-\0', 'i') = 'x-val1';

-- Ensure no pushdown when we disable it.
SET pg_clickhouse.pushdown_regex = 'false';
EXPLAIN (VERBOSE, COSTS OFF) SELECT val FROM strings WHERE regexp_split_to_array(val, ',') = '{a,b,c}'::text[];
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM strings WHERE regexp_replace(val, '^x', 'y') = 'val2';

\unset ECHO
-- Use DO to test functions available in Postgres 15+
-- PG15+: trim_array → match()
DO $_$
DECLARE
	output JSONB;
BEGIN
    IF current_setting('server_version_num')::int >= 150000 THEN
        EXECUTE format($$EXPLAIN (VERBOSE, FORMAT JSON) SELECT val FROM strings WHERE regexp_like(val, '^val\d')$$) INTO output;
        RAISE NOTICE 'regexp_like: %', jsonb_path_query(output, '$[0].Plan')->>'Remote SQL';
    ELSE
        -- Fake it on earlier versions.
        RAISE NOTICE 'regexp_like: SELECT val FROM regex_test.strings';
    END IF;
END;
$_$;
\set ECHO all

DROP USER MAPPING FOR CURRENT_USER SERVER regex_loopback;
SELECT clickhouse_raw_query('DROP DATABASE regex_test');
DROP SERVER regex_loopback CASCADE;
