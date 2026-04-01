-- Test ILIKE and POSIX regex operator pushdown
SET datestyle = 'ISO';

CREATE SERVER ilike_regex_loopback FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'ilike_regex_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER ilike_regex_loopback;

SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS ilike_regex_test');
SELECT clickhouse_raw_query('CREATE DATABASE ilike_regex_test');

SELECT clickhouse_raw_query($$
    CREATE TABLE ilike_regex_test.events (
        id     Int32,
        name   String,
        path   String
    ) ENGINE = MergeTree ORDER BY id
$$);

SELECT clickhouse_raw_query($$
    INSERT INTO ilike_regex_test.events VALUES
        (1, 'page_view',   '/users/profile'),
        (2, 'Page_View',   '/users/settings'),
        (3, 'PAGE_VIEW',   '/admin/dashboard'),
        (4, 'add_to_cart', '/products/shoes'),
        (5, 'Add_To_Cart', '/products/hats'),
        (6, 'purchase',    '/checkout'),
        (7, 'PURCHASE',    '/checkout/confirm'),
        (8, 'share',       '/social/twitter'),
        (9, 'logout',      '/auth/logout'),
        (10, 'signup',     '/auth/signup')
$$);

CREATE SCHEMA ilike_regex_test;
IMPORT FOREIGN SCHEMA "ilike_regex_test" FROM SERVER ilike_regex_loopback INTO ilike_regex_test;
SET search_path = ilike_regex_test, public;

-- =======================================================
-- ILIKE: case-insensitive LIKE pushdown
-- =======================================================

-- ILIKE should match regardless of case
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM events WHERE name ILIKE '%view%' ORDER BY id;
SELECT id, name FROM events WHERE name ILIKE '%view%' ORDER BY id;

-- NOT ILIKE
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM events WHERE name NOT ILIKE '%view%' ORDER BY id;
SELECT id, name FROM events WHERE name NOT ILIKE '%view%' ORDER BY id;

-- ILIKE in aggregate context
EXPLAIN (VERBOSE, COSTS OFF)
SELECT count(*) FROM events WHERE name ILIKE 'page%';
SELECT count(*) FROM events WHERE name ILIKE 'page%';

-- ILIKE with underscore wildcard
EXPLAIN (VERBOSE, COSTS OFF)
SELECT count(*) FROM events WHERE name ILIKE 'page______';
SELECT count(*) FROM events WHERE name ILIKE 'page______';

-- LIKE (case-sensitive) should still work as before
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM events WHERE name LIKE '%view%' ORDER BY id;
SELECT id, name FROM events WHERE name LIKE '%view%' ORDER BY id;

-- NOT LIKE (case-sensitive)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT count(*) FROM events WHERE name NOT LIKE '%view%';
SELECT count(*) FROM events WHERE name NOT LIKE '%view%';

-- =======================================================
-- POSIX regex: ~ mapped to match(), !~ to NOT match()
-- =======================================================

-- ~ (case-sensitive regex match)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM events WHERE name ~ '^page' ORDER BY id;
SELECT id, name FROM events WHERE name ~ '^page' ORDER BY id;

-- !~ (case-sensitive regex no match)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM events WHERE name !~ '^page' ORDER BY id;
SELECT id, name FROM events WHERE name !~ '^page' ORDER BY id;

-- ~ with alternation pattern
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM events WHERE name ~ '^(purchase|share)$' ORDER BY id;
SELECT id, name FROM events WHERE name ~ '^(purchase|share)$' ORDER BY id;

-- ~ in aggregate context
EXPLAIN (VERBOSE, COSTS OFF)
SELECT count(*) FROM events WHERE name ~ '_view$';
SELECT count(*) FROM events WHERE name ~ '_view$';

-- ~ with path column (test regex with slashes)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, path FROM events WHERE path ~ '^/auth/' ORDER BY id;
SELECT id, path FROM events WHERE path ~ '^/auth/' ORDER BY id;

-- =======================================================
-- ~* and !~* fall back to local evaluation (not pushed)
-- =======================================================

-- ~* (case-insensitive regex) should NOT be pushed down
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM events WHERE name ~* '^page' ORDER BY id;
SELECT id, name FROM events WHERE name ~* '^page' ORDER BY id;

-- !~* (case-insensitive regex negation) should NOT be pushed down
EXPLAIN (VERBOSE, COSTS OFF)
SELECT count(*) FROM events WHERE name !~* '^page';
SELECT count(*) FROM events WHERE name !~* '^page';

-- Cleanup
SELECT clickhouse_raw_query('DROP DATABASE ilike_regex_test');
DROP USER MAPPING FOR CURRENT_USER SERVER ilike_regex_loopback;
DROP SERVER ilike_regex_loopback CASCADE;
