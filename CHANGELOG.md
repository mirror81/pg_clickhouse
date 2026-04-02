# Changelog

All notable changes to this project will be documented in this file. It uses the
[Keep a Changelog] format, and this project adheres to [Semantic Versioning].

  [Keep a Changelog]: https://keepachangelog.com/en/1.1.0/
  [Semantic Versioning]: https://semver.org/spec/v2.0.0.html
    "Semantic Versioning 2.0.0"

## [v0.1.7] — Unreleased

### ⚡ Improvements

*   Added mapping for the `JSON` and `JSONB` `-> TEXT` and `->> TEXT`
    operators to be passed down to ClickHouse using its [sub-column syntax]

### 📔 Notes

*   Eliminated use of a constant that required libcurl 7.87.0, restoring
    support for earlier versions.

  [v0.1.7]: https://github.com/ClickHouse/pg_clickhouse/compare/v0.1.6...v0.1.7
  https://clickhouse.com/docs/sql-reference/data-types/newjson#reading-json-paths-as-sub-columns
    "ClickHouse Docs: Reading JSON paths as sub-columns"

## [v0.1.6] — 2026-04-02

This release makes binary-only changes. Once installed, any existing use of
pg_clickhouse v0.1 will get its benefits on reload without needing to
`ALTER EXTENSION UPDATE`.

### ⚡ Improvements

*   Added support for the PostgreSQL `to_timestamp(float8)` function, mapped to
    `fromUnixTimestamp(toInt64())` in ClickHouse.

### 🪲 Bug Fixes

*   Disabled query pushdown for JSONB subscript syntax (e.g.,
    `col_name['field']`) for now. Thanks Kaushik Iska for the PR (#161).
*   Added query cancellation via Ctrl-C and `statement_timeout` to the binary
    driver. Thanks Kaushik Iska for the PR (#162) that fixed this issue (#41).
*   Fixed `LIKE`, `ILIKE`, and regex operator pushdown, including `~~*`,
    `!~~*`, `~`, `!~`, `~*`, `!~*`. Thanks Kaushik Iska for the PR (#164).

  [v0.1.6]: https://github.com/ClickHouse/pg_clickhouse/compare/v0.1.5...v0.1.6

## [v0.1.5] — 2026-03-20

This release makes binary-only changes. Once installed, any existing use of
pg_clickhouse v0.1 will get its benefits on reload without needing to
`ALTER EXTENSION UPDATE`.

### 🚨 Security Fixes

*   Changed the http driver connection function to raise an error if the
    database name contains line ending characters to prevent HTTP header
    injection.
*   Fixed an SQL injection vulnerability in parsing of parameters to the
    `engine 'CollapsingMergeTree($sign)` option to `CREATE FOREIGN TABLE`.

### 🪲 Bug Fixes

*   Fixed a crash due to an unexpected EOF while the http driver parses a
    response. Thanks to @serprex for the fix (#153)
*   Fixed a crash due to an unchecked memory allocation while the http driver
    reads a response. Thanks to @serprex for the fix (#154).

  [v0.1.5]: https://github.com/ClickHouse/pg_clickhouse/compare/v0.1.4...v0.1.5

## [v0.1.4] — 2026-02-17

This release makes binary-only changes. Once installed, any existing use of
pg_clickhouse v0.1 will get its benefits on reload without needing to
`ALTER EXTENSION UPDATE`.

### ⚡ Improvements

*   Added support for the PostgreSQL `md5()` function, mapped to
    `lower(hex(MD5()))` in ClickHouse.
*   Added support for mapping PostgreSQL BYTEA columns to ClickHouse String
    columns.
*   Added explicit setting of `format_tsv_null_representation` and
    `output_format_tsv_crlf_end_of_line` to all http requests, as unexpected
    values will interfere with its operation.
*   Improved the error message from the binary driver when attempting to
    insert a `NULL` into a column that is not `Nullable(T)`.

### 🪲 Bug Fixes

*   Fixed binary driver errors when attempting to insert a `NULL` value into
    `Nullable` Numeric, Text, `Enum`, `UUID`, and `INET` columns. Thanks to
    Rahul Mehta for the report (#140).
*   Fixed http driver array parsing, which previously did not properly parse
    string values and would raise an error on values containing brackets
    (`[]`). Thanks to Philip Dubé for the spot (#142).
*   Fixed a bug where the binary driver would raise an error on an empty
    array.

### 📔 Notes

*   Refactored and improved the http engine's result processing, bringing it
    into closer alignment with the binary engine and removing double
    processing of row values.
*   The http driver now ignores the following session settings from the
    `pg_clickhouse.session_settings` to prevent them from interfering with its
    operation: `date_time_output_format`, `format_tsv_null_representation`,
    and `output_format_tsv_crlf_end_of_line`.

  [v0.1.4]: https://github.com/ClickHouse/pg_clickhouse/compare/v0.1.3...v0.1.4

## [v0.1.3] — 2026-01-23

This release makes binary-only changes. Once installed, any existing use of
pg_clickhouse v0.1 will get its benefits on reload without needing to
`ALTER EXTENSION UPDATE`.

### ⚡ Improvements

*   Changed the default mapping for `DateTime` and `DateTime64` values from
    `TIMESTAMP` to `TIMESTAMPTZ`, because ClickHouse stores `DateTime`s as a
    Unix timestamp, always normalized to UTC, even if it displays as a
    different time zone. As of the first bug fix listed below, pg_clickhouse
    (almost) always fetches these values in UTC, so can store them as
    `TIMESTAMPTZ` values.
*   Implemented `INSERT` support for the UUID and INET (IPv4 and IPv6) types.
    Thanks to Rahul Mehta for the report ([#127])!

### 🪲 Bug Fixes

*   Fixed time zone conversion in the http engine. Does not work with
    parameterized execution on ClickHouse versions prior to 25.8 due to
    [ClickHouse Issue 88088]; recommend using the binary engine for tables
    with timestamp values on earlier ClickHouse versions to avoid the issue.
*   Fixed a server crash when attempting to insert types not yet supported by
    the binary engine.

### 🚀 Distribution

*   Added the [security policy](SECURITY.md).

### 📔 Notes

*   [Scripted] the generation of the TPC-H results table and updated it in the
    [README](README.md).
*   Cleaned up some comments and old references to postgres_fdw left from the
    original fork in 2019.
*   Added tests demonstrating subqueries that pg_clickhouse does not yet push
    down, to be improved in future releases.

### 🏗️ Build Setup

*   Added pre-commit hooks to lint the code, including for indentation
    enforced by `pg_bsd_indent`. A new workflow ensures consistency for these
    quality checks. Relatedly, a number of issues found by the linters have
    been corrected.
*   Configured `make installcheck` to run the tests in parallel, resulting in
    far faster test execution on multi-core systems. Adjusted the schemas in
    which some of the tests work to ensure they don't stomp on each other.

  [v0.1.3]: https://github.com/ClickHouse/pg_clickhouse/compare/v0.1.2...v0.1.3
  [ClickHouse Issue 88088]: https://github.com/ClickHouse/ClickHouse/pull/88088
  [#127]: https://github.com/ClickHouse/pg_clickhouse/issues/127
    "ClickHouse/pg_clickhouse#127 INSERT into ClickHouse table with UUID column fails: unexpected column type for 2950: UUID"
  [Scripted]: https://github.com/ClickHouse/pg_clickhouse/tree/main/dev/tpch
    "pg_clickhouse: TPC-H Benchmark"

## [v0.1.2] — 2026-01-07

This release makes binary-only changes. Once installed, any existing use of
pg_clickhouse v0.1 will get its benefits on reload without needing to
`ALTER EXTENSION UPDATE`.

### ⚡ Improvements

*   Added support for parameterized execution, including `PREPARE` and
    `EXECUTE`, by converting PostgreSQL `$1`-style parameters to ClickHouse
    `{param:type}`-style parameters.
*   Added support for inserting arrays to the http engine.

### 🪲 Bug Fixes

*   Fixed the http engine's parsing of UUID arrays selected from ClickHouse.
*   Fixed the binary engine's conversion of Date values, which in arrays ended
    up too large by several orders of magnitude (e.g., `2025-12-05` would be
    converted to `10529827-09-17` 😱). Thanks to Tom Lane for the pointer to
    the proper function to easily convert epoch seconds to a date.
*   Fixed a binary engine bug where dates and timestamps for epoch 0
    (`1970-01-01 00:00:00`) rendered as `NULL`.
*   Added support for the `Date32` ClickHouse type.
*   Fixed conversion of `array_agg()` to support ClickHouse versions prior to
    23.8.
*   Fixed the precision of fractional seconds in the binary engine's
    conversion of ClickHouse `DateTime64` values to Postgres `TIMESTAMP`
    (#114).

### 📔 Notes

*   Removed unused code designed to support custom PostgreSQL extensions:
    [ajbool], ajtime, [country], and [istore].
*   Tweaked cost estimation to encourage pushdown of `min()` and `max()`.

### 📚 Documentation

*   Documented `IMPORT FOREIGN SCHEMA` identifier case preservation behavior.
*   Fixed the Postgres Docker start and connect info in the
    [tutorial](doc/tutorial.md).
*   Fixed the commands to start and connect to the pg_clickhouse Docker image
    in the [tutorial].
*   Added complete DML documentation to the [reference
    docs](doc/pg_clickhouse.md), including the new `PREPARE`/`EXECUTE` support
    and `INSERT`, `SET`, `COPY`, as well as shared library preloading.
*   Documented the Postgres aggregate functions known (via new tests) to push
    down to ClickHouse.

  [v0.1.2]: https://github.com/clickhouse/pg_clickhouse/compare/v0.1.1...v0.1.2
  [ajbool]: https://pgxn.org/dist/ajbool/ "ajbool on PGXN"
  [country]: https://pgxn.org/dist/country/ "country on PGXN"
  [istore]: https://pgxn.org/dist/istore/ "istore on PGXN"
  [tutorial]: ./doc/tutorial.md

## [v0.1.1] — 2025-12-17

This release makes binary-only changes. Once installed, any existing use of
pg_clickhouse v0.1 will get its benefits on reload without needing to
`ALTER EXTENSION UPDATE`.

### ⚡ Improvements

*   Refactored the internal handling of the `pg_clickhouse.session_settings`
    GUC to parse the settings only once rather than for every query sent to
    ClickHouse

### 🚨 Security Fixes

*   Updated the SQL query rewriting to properly quote strings and identifiers
    in SQL queries sent to ClickHouse, fixing potential SQL injection
    vulnerabilities, notably via `IMPORT SCHEMA`. This has the effect of
    preserving mixed-case and uppercase identifiers.

### 🪲 Bug Fixes

*   Fixed a crash when sending an empty `COPY FROM` via the binary driver

### ⬆️ Dependency Updates

*   Updated vendored clickhouse-cpp library

### 🏗️ Build Setup

*   Fixed the `Makefile` targets so that calling `make install` without first
    calling `make` will properly create the versioned SQL file.

### 📚 Documentation

*   Added a [versioning policy] the documentation
*   Fixed the badges and broken TPC-H links in [README.md]
*   Added PGXN installation instructions to [README.md]

  [v0.1.1]: https://github.com/clickhouse/pg_clickhouse/compare/v0.1.0...v0.1.1
  [versioning policy]: ./doc/pg_clickhouse.md#versioning-policy
  [README.md]: README.md

## [v0.1.0] — 2025-12-09

### ⚡ Improvements

*   Improved function and aggregate pushdown
*   Added TLS support to both the http and binary engines
*   Added pushdown aggregate functions:
    *   `uniq()`
    *   `uniqExact()`
    *   `uniqCombined()`
    *   `uniqCombined64()`
    *   `uniqExact()`
    *   `uniqHLL12()`
    *   `uniqTheta()`
*   Added pushdown functions:
    *   `toUInt8()`
    *   `toUInt16()`
    *   `toUInt32()`
    *   `toUInt64()`
    *   `toUInt128()`
    *   `quantile()`
    *   `quantileExact()`
*   Mapped PostgreSQL `regexp_like()` to push down to ClickHouse `match()`
    function
*   Mapped PostgreSQL `extract()` to push down to equivalent ClickHouse
    DateTime extraction functions (already mapped to `date_part()`)
*   Mapped PostgreSQL `percentile_cont()` ordered set aggregate function to
    push down to ClickHouse `quantile()` parametrized
*   Mapped the `COUNT()` return value to `bigint`
*   Added the query text and, for the http engine, the status code to error
    messages
*   Added `pg_clickhouse.session_settings` GUC, defaulting to
    `join_use_nulls 1, group_by_use_nulls 1, final 1`
*   Added mappings and support for additional data types:
    *   `Bool` => `boolean`
    *   `Decimal` => `numeric`
    *   `JSON` => `jsonb` (http engine only)
*   Added the `dbname` option to http engine connections, including
    `clickhouse_raw_query()`
*   Added LEFT SEMI JOIN pushdown for `EXISTS()` subqueries

### 🏗️ Build Setup

*   Ported from clickhouse_fdw
*   Made lots of general code cleanup
*   Added PGXS build pipeline
*   Added PGXN and GitHub release workflows
*   Added `pg_clickhouse` OCI image workflow that publishes to
    ghcr.io/clickhouse/pg_clickhouse, with tags for PostgreSQL versions 13-18

### 📚 Documentation

*   Added tutorial in [doc/tutorial.md](doc/tutorial.md)
*   Added reference documentation in [doc/pg_clickhouse.md](doc/pg_clickhouse.md)

### 🪲 Bug Fixes

*   Fixed `dictGet()`, `argMin()`, and `argMax()`
*   Fixed bug in filtered `COUNT()`
*   Fixed `AggregateFunction` option to propagate to a nested aggregate
    function call
*   Improved unsigned integer support

### ⬆️ Dependency Updates

*   Updated vendored clickhouse-cpp library
*   Reimplemented binary engine inserts with clickhouse-cpp improvements
*   Support and tested against PostgreSQL 13-18
*   Support and tested against ClickHouse 23-25

### 📔 Notes

*   Set full version in `PG_MODULE_MAGIC_EXT`
*   Set to default ports to TLS for ClickHouse Cloud host names

  [v0.1.0]: https://github.com/clickhouse/pg_clickhouse/compare/a1487bd...v0.1.0
  [RFC 9535]: https://www.rfc-editor.org/rfc/rfc9535.html
    "RFC 9535 JSONPath: Query Expressions for JSON"
