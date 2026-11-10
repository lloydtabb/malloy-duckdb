# malloy-duckdb

A DuckDB extension that lets you query with [Malloy](https://www.malloydata.dev)
instead of SQL. Malloy compiles to DuckDB SQL inside the extension, and DuckDB
runs the result, so Malloy queries compose with ordinary SQL.

> **Status: early prototype.** Works on Linux against DuckDB v1.5.6; see [Status](#status).

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

Early prototype. Built and tested on Linux x86-64 against the official
DuckDB v1.5.6 CLI; macOS and other platforms not yet tried.

Compile speed in QuickJS, on 4 cores with the two-file flights model:

| | Time |
|---|---|
| First compile in a process (creates the JS runtime, loads the 2.2 MB bundle) | ~2.2 s |
| Each compile after that | 50–75 ms |

Known gaps:

- Timestamp literals and timezones (`@2003-01-15`, `timezone:`) don't
  compile yet: Malloy uses `luxon`, which needs `Intl`, and QuickJS has none.
- `STRUCT` and `LIST` columns come through as SQL-native types, so Malloy's
  nested-data features don't apply to them yet.
- Every query re-reads the model and re-describes its tables; nothing is
  cached yet.
- Only `run:` statements are recognized as bare Malloy.
- A bare `run:` in the same script as the `LOAD malloy` that enables it
  (for example in one `duckdb -c '...'`) is a syntax error: DuckDB parses
  the whole script before the extension loads.
- The CI workflow is in `ci/` until it is moved to `.github/workflows/`
  (see `ci/README.md`); it builds `linux_amd64` and `osx_arm64` only.

## Building

The quick way builds only the extension, not DuckDB, and downloads the
official DuckDB CLI of the matching version to load it into:

```sh
git clone --recurse-submodules https://github.com/lloydtabb/malloy-duckkdb
cd malloy-duckkdb
scripts/quick-build.sh
build/duckdb-cli/duckdb -unsigned
```

```sql
LOAD 'build/release/extension/malloy/malloy.duckdb_extension';
```

On macOS, give `LOAD` an absolute path, e.g.
`LOAD '/path/to/malloy-duckkdb/build/release/extension/malloy/malloy.duckdb_extension';`.
The official CLI is a hardened binary, and macOS refuses to `dlopen` a
relative path from one ("relative path not allowed in hardened program").
`scripts/quick-build.sh` prints the absolute path to use.

`-unsigned` is needed because the build isn't signed by DuckDB. The
extension only loads into the exact DuckDB version it was built against
(v1.5.6, the `duckdb` submodule).

The full build, which also builds DuckDB, its shell with the extension
linked in, and the test runner for `test/sql/*.test`:

```sh
GEN=ninja make release
make test
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
