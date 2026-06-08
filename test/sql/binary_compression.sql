SET datestyle = 'ISO';

-- Seed a small table through the http helper (clickhouse_raw_query is http).
SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS compression_test');
SELECT clickhouse_raw_query('CREATE DATABASE compression_test');
SELECT clickhouse_raw_query('CREATE TABLE compression_test.t (
    c1 Int32, c2 String
) ENGINE = MergeTree ORDER BY c1;');
SELECT clickhouse_raw_query('INSERT INTO compression_test.t
    SELECT number, format(''row {0}'', toString(number)) FROM numbers(5);');

-- One server, reconfigured per case: ALTER SERVER invalidates the cached
-- connection, so each subsequent query reconnects with the new compression.
CREATE SERVER comp FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'compression_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER comp;
CREATE FOREIGN TABLE ft (c1 int, c2 text)
    SERVER comp OPTIONS (table_name 't');

-- Default compression (lz4) when the option is omitted.
SELECT * FROM ft ORDER BY c1;

-- Explicit lz4.
ALTER SERVER comp OPTIONS (ADD compression 'lz4');
SELECT * FROM ft ORDER BY c1;

-- zstd; uppercase exercises case-insensitive parsing.
ALTER SERVER comp OPTIONS (SET compression 'ZSTD');
SELECT * FROM ft ORDER BY c1;

-- Disabled.
ALTER SERVER comp OPTIONS (SET compression 'none');
SELECT * FROM ft ORDER BY c1;

-- INSERT round-trips through the compressed write path.
ALTER SERVER comp OPTIONS (SET compression 'zstd');
INSERT INTO ft VALUES (100, 'inserted');
SELECT * FROM ft WHERE c1 = 100;

-- Unknown value errors when the connection is opened.
ALTER SERVER comp OPTIONS (SET compression 'bogus');
SELECT * FROM ft ORDER BY c1;

DROP FOREIGN TABLE ft;
DROP USER MAPPING FOR CURRENT_USER SERVER comp;
SELECT clickhouse_raw_query('DROP DATABASE compression_test');
DROP SERVER comp CASCADE;
