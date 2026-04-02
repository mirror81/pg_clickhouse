SET datestyle = 'ISO';
CREATE SERVER binary_json_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'json_test', driver 'binary');
CREATE SERVER http_json_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'json_test', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER binary_json_loopback;
CREATE USER MAPPING FOR CURRENT_USER SERVER http_json_loopback;

SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS json_test');
SELECT clickhouse_raw_query('CREATE DATABASE json_test');
SELECT clickhouse_raw_query($$
    CREATE TABLE json_test.things (
        id   Int32 NOT NULL,
        data JSON NOT NULL
    ) ENGINE = MergeTree PARTITION BY id ORDER BY (id);
$$);

CREATE SCHEMA json_bin;
CREATE SCHEMA json_http;
IMPORT FOREIGN SCHEMA "json_test" FROM SERVER binary_json_loopback INTO json_bin;
\d json_bin.things
IMPORT FOREIGN SCHEMA "json_test" FROM SERVER http_json_loopback INTO json_http;
\d json_http.things

-- Fails pending https://github.com/ClickHouse/clickhouse-cpp/issues/422
INSERT INTO json_bin.things VALUES
    (1, '{"id": 1, "name": "widget", "size": "large", "stocked": true}'),
    (2, '{"id": 2, "name": "sprocket", "size": "small", "stocked": true}')
;

INSERT INTO json_http.things VALUES
    (1, '{"id": 1, "name": "widget", "size": "large", "stocked": true}'),
    (2, '{"id": 2, "name": "sprocket", "size": "small", "stocked": true}'),
    (3, '{"id": 3, "name": "gizmo", "size": "medium", "stocked": true}'),
    (4, '{"id": 4, "name": "doodad", "size": "large", "stocked": false}')
;

SELECT * FROM json_bin.things ORDER BY id;
SELECT * FROM json_http.things ORDER BY id;

-- Subscript access on JSON columns must not be pushed down to ClickHouse.
-- ClickHouse JSON does not support the jsonb `column['key']` syntax (it
-- requires dot notation), so subscripts must be evaluated locally by
-- PostgreSQL.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT data['name'] FROM json_http.things;
SELECT data['name'] FROM json_http.things ORDER BY id;

-- DISTINCT forces an ORDER BY or HashAgg; the subscript must stay local.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT DISTINCT data['size'] FROM json_http.things;
SELECT DISTINCT data['size'] FROM json_http.things;

-- GROUP BY with a JSON subscript expression.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT data['size'], count(*) FROM json_http.things GROUP BY data['size'];
SELECT data['size'], count(*) FROM json_http.things GROUP BY data['size'];

-- The jsonb ->> operator is pushed down in WHERE / ORDER BY clauses, but
-- target-list expressions are evaluated locally (PostgreSQL fetches the whole
-- column and applies the operator after). This query runs -> locally.
-- N.B.: Binary driver JSON data not yet supported.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT data ->> 'name' FROM json_http.things;
SELECT data ->> 'name' FROM json_http.things ORDER BY id;

-- WHERE clause with ->> equality must be pushed down.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.things WHERE data ->> 'name' = 'widget';
SELECT * FROM json_http.things WHERE data ->> 'name' = 'widget';

-- WHERE clause with ->> and LIKE.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.things WHERE data ->> 'name' LIKE 'wid%';

-- WHERE with multiple ->> conditions (AND).
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.things
  WHERE data ->> 'size' = 'large' AND data ->> 'stocked' = 'true';

-- WHERE with ->> in an OR condition.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.things
  WHERE data ->> 'name' = 'widget' OR data ->> 'name' = 'gizmo';

-- ORDER BY with ->> pushdown.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.things ORDER BY data ->> 'size';

-- The jsonb -> operator: target-list expressions are evaluated locally
-- (same as ->>). This query evaluates `->` locally.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT data -> 'name' FROM json_http.things;
SELECT data -> 'name' FROM json_http.things ORDER BY id;

-- WHERE clause with -> equality must be pushed down.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.things WHERE data -> 'name' = '"widget"';
SELECT * FROM json_http.things WHERE data -> 'name' = '"widget"';

-- WHERE clause with -> JSON boolean literal must push down.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.things WHERE data -> 'stocked' = 'true'::jsonb;
SELECT * FROM json_http.things WHERE data -> 'stocked' = 'true'::jsonb ORDER BY id;

-- WHERE clause with -> wraps the dot notation in toJSONString() so that the
-- result is a proper JSON value (-> returns jsonb, not text like ->>).
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.things WHERE data -> 'name' = '"widget"'::jsonb;

-- Edge cases: JSON keys that require identifier quoting.
SELECT clickhouse_raw_query($$
    CREATE TABLE json_test.special_keys (
        id   Int32 NOT NULL,
        data JSON NOT NULL
    ) ENGINE = MergeTree ORDER BY (id);
$$);

CREATE FOREIGN TABLE json_http.special_keys (id integer NOT NULL, data jsonb NOT NULL)
  SERVER http_json_loopback OPTIONS (database 'json_test', table_name 'special_keys');

INSERT INTO json_http.special_keys VALUES
    (1, '{"my field": "hello", "CamelCase": "world", "select": "reserved"}'),
    (2, E'{"The \\"meaning\\" of life": 42, "back\\\\slash": "bs", "dotted.key": "dot", "it''s": "apos", "key/with!special@chars#": "special", "123numeric": "num"}');

-- Key with a space: must be quoted in the remote SQL.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.special_keys WHERE data ->> 'my field' = 'hello';
SELECT data ->> 'my field' FROM json_http.special_keys;

-- Key with mixed case.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.special_keys WHERE data ->> 'CamelCase' = 'world';
SELECT data ->> 'CamelCase' FROM json_http.special_keys;

-- Key that is a SQL reserved word.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.special_keys WHERE data ->> 'select' = 'reserved';
SELECT data ->> 'select' FROM json_http.special_keys;

-- Key containing embedded double quotes.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.special_keys WHERE data ->> 'The "meaning" of life' = '42';
SELECT data ->> 'The "meaning" of life' FROM json_http.special_keys WHERE id = 2;

-- Key containing a backslash.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.special_keys WHERE data ->> 'back\slash' = 'bs';
SELECT data ->> 'back\slash' FROM json_http.special_keys WHERE id = 2;

-- Key containing a dot (must not be confused with nested access).
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.special_keys WHERE data ->> 'dotted.key' = 'dot';
SELECT data ->> 'dotted.key' FROM json_http.special_keys WHERE id = 2;

-- Key containing an apostrophe / single quote.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.special_keys WHERE data ->> 'it''s' = 'apos';
SELECT data ->> 'it''s' FROM json_http.special_keys WHERE id = 2;

-- Key with slashes, bangs, at-signs, etc.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.special_keys WHERE data ->> 'key/with!special@chars#' = 'special';

-- Key that starts with a digit.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.special_keys WHERE data ->> '123numeric' = 'num';

SELECT clickhouse_raw_query('DROP DATABASE json_test');
DROP USER MAPPING FOR CURRENT_USER SERVER binary_json_loopback;
DROP USER MAPPING FOR CURRENT_USER SERVER http_json_loopback;
DROP SERVER binary_json_loopback CASCADE;
DROP SERVER http_json_loopback CASCADE;
