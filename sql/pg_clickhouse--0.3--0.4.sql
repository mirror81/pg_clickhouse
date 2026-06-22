-- Report the ClickHouse server version ("major.minor.patch") for a foreign
-- server, connecting if necessary.
CREATE FUNCTION clickhouse_server_version(server_name TEXT)
RETURNS TEXT
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;
