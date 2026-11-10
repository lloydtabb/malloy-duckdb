# malloy-duckdb

A DuckDB extension that lets you query with [Malloy](https://www.malloydata.dev)
instead of SQL. Malloy compiles to DuckDB SQL inside the extension, and DuckDB
runs the result, so Malloy queries compose with ordinary SQL.

> **Status: early prototype.** See [Status](#status) below.

```sql
LOAD malloy;

-- Load a model: a local path, or with httpfs loaded, https:// or s3://.
-- Imports resolve relative to the model file.
SELECT * FROM malloy_load('models/flights.malloy');

-- Run Malloy; the result is a table like any other.
SELECT * FROM malloy('run: flights -> by_carrier');

-- ...so it composes with SQL.
SELECT nickname, flight_count
FROM malloy('run: flights -> by_carrier')
WHERE flight_count > 1000;

-- Without a model, queries can name tables directly.
SELECT * FROM malloy('run: duckdb.table(''flights'') -> { group_by: carrier; aggregate: n is count() }');

-- See the SQL a query compiles to.
SELECT sql FROM malloy_sql('run: flights -> by_carrier');

-- Bare Malloy statements, once the parser override is allowed:
SET allow_parser_override_extension = 'fallback';
run: flights -> { group_by: carrier; aggregate: n is count() }
```

## Functions

| Function | Returns |
|---|---|
| `malloy(query [, model := location])` | The query's rows |
| `malloy_sql(query [, model := location])` | One row: the SQL the query compiles to |
| `malloy_load(location)` | Sets this connection's model, and lists what it exports (`kind`, `name`, `model`) |
| `malloy_version()` | The Malloy version compiled in |

Inside DuckDB the Malloy connection is always named `duckdb`, as in
`duckdb.table('flights')` and `duckdb.sql("""...""")`.

## How it works

The Malloy compiler is TypeScript. This extension embeds
[QuickJS-ng](https://github.com/quickjs-ng/quickjs), a small JavaScript
engine written in C, and runs the compiler in it as one bundled script
(`js/malloy-embedded.js`, built by Malloy's `packages/malloy-embedded`).

The compiler does no IO itself; it asks for what it needs. `js/driver.js`
runs that loop and calls back into C++ (`src/malloy_engine.cpp`) for:

- **files** (the model and its imports), read through DuckDB's file system,
  so anything DuckDB can open works as a model location;
- **schemas** of `duckdb.table(...)` and `duckdb.sql(...)` sources, found by
  binding `SELECT * FROM <source>` in the calling connection, so temporary
  tables and uncommitted changes are visible.

`malloy()` then hands DuckDB the generated SQL as a subquery (the same
mechanism as DuckDB's `query()` function), and DuckDB plans and runs it.

Bare `run:` statements use DuckDB's parser override hook, which sees the
statement before DuckDB splits it at semicolons (Malloy uses semicolons
inside braces). DuckDB only calls that hook when
`allow_parser_override_extension` is set, so without it a `run:` statement
gets an error explaining how to enable it.

## Status

- Written against DuckDB v1.5.6. The first builds and test runs are on
  GitHub Actions; a local build of DuckDB and QuickJS has not been run.
- Known gaps:
  - Timestamp literals and timezones (`@2003-01-15`, `timezone:`) don't
    compile yet: Malloy uses `luxon`, which needs `Intl`, and QuickJS has none.
  - `STRUCT` and `LIST` columns come through as SQL-native types, so Malloy's
    nested-data features don't apply to them yet.
  - Every query re-reads the model and re-describes its tables; nothing is
    cached yet.
  - Only `run:` statements are recognized as bare Malloy.
- CI builds `linux_amd64` and `osx_arm64` only, for now.

## Building

```sh
git clone --recurse-submodules https://github.com/lloydtabb/malloy-duckkdb
cd malloy-duckkdb
make            # builds DuckDB and the extension; takes a while the first time
make test       # runs test/sql/*.test
./build/release/duckdb
```

The shell built by `make` has the extension linked in. To load the built
extension into another DuckDB of the same version:

```sql
-- start duckdb with -unsigned
LOAD 'build/release/extension/malloy/malloy.duckdb_extension';
```

To pick up a newer Malloy, rebuild the bundle from a Malloy checkout:

```sh
scripts/update-bundle.sh /path/to/malloy
```

## Layout

| Path | What |
|---|---|
| `src/malloy_extension.cpp` | Table functions, parser hooks, per-connection model |
| `src/malloy_engine.cpp` | QuickJS runtime and the C++ side of the compile loop |
| `js/driver.js` | The JavaScript side of the compile loop |
| `js/malloy-embedded.js` | The Malloy compiler bundle (generated; see `js/BUNDLE_SOURCE`) |
| `cmake/embed_js.cmake` | Compiles the JavaScript into the extension as byte arrays |
| `test/sql/malloy.test` | SQL tests; models in `test/models/` |
| `duckdb`, `extension-ci-tools`, `third_party/quickjs` | Submodules |
