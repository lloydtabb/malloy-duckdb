# malloy-duckdb

A DuckDB extension that lets you query with [Malloy](https://www.malloydata.dev)
instead of SQL. Malloy compiles to DuckDB SQL inside the extension, and DuckDB
runs the result, so Malloy queries compose with ordinary SQL.

> **Status: early prototype.** Works on Linux against DuckDB v1.5.6; see [Status](#status).

```sql
LOAD malloy;

-- Each connection has a Malloy model in memory, the session model.
-- import, source: and query: add to it; relative paths resolve from the
-- working directory, and with httpfs loaded, https:// and s3:// work too.
import 'models/flights.malloy';
source: long_flights is flights extend { where: distance > 1000 };
query: busy is flights -> by_carrier;

show: sources;
show: queries;
describe source flights;
show: session;              -- the session model's statements

-- Run Malloy.
run: flights -> by_carrier;
run: busy;

-- Malloy results are tables, so they compose with SQL.
SELECT nickname, flight_count
FROM malloy('run: flights -> by_carrier')
WHERE flight_count > 1000;

-- See the SQL a query compiles to.
SELECT sql FROM malloy_sql('run: flights -> by_carrier');
```

In the DuckDB CLI, end each statement with `;`, as for SQL.

## Statements

| Statement | Does |
|---|---|
| `import '...'` | Adds an import to the session model |
| `source: ...`, `query: ...` | Adds definitions to the session model. A statement that doesn't compile is rejected, and the session model is unchanged |
| `run: ...` | Runs a Malloy query against the session model |
| `show: sources`, `show: queries` | Lists the sources or named queries the session model can see, imported ones included |
| `describe source <name>`, `describe query <name>` | Lists the fields of a source, or a named query's output: name, kind (dimension, measure, join, view), and type |
| `show: session` | Lists the statements in the session model |

`show sources` without the colon works too, except in the DuckDB CLI: it is
valid SQL (`SHOW` of a table named `sources`), and the CLI's PEG parser,
described below, claims it first.

Malloy statements reach the extension through DuckDB's parser override hook,
which `LOAD malloy` turns on by setting `allow_parser_override_extension` to
`fallback`, unless you have set it already. **In the DuckDB CLI this also
turns on DuckDB's experimental PEG SQL parser** (the CLI's `autocomplete`
extension registers it): SQL the PEG parser accepts is parsed by it, and
anything it rejects falls back to DuckDB's usual parser. To turn the
override off, `SET allow_parser_override_extension = 'default'`; the
functions below keep working.

## Functions

Every statement is a call to one of these, which can also be used directly.

| Function | Returns |
|---|---|
| `malloy(query [, model := location])` | The query's rows |
| `malloy_sql(query [, model := location])` | One row: the SQL the query compiles to |
| `malloy_define(statements)` | Adds `import`/`source:`/`query:` statements to the session model; lists what they define (`kind`, `name`) |
| `malloy_load(location)` | The same as `malloy_define` of an `import` of `location` |
| `malloy_sources([model := location])` | Every source and named query visible (`name`, `kind`) |
| `malloy_describe(name [, model := location])` | The fields of a source or named query (`name`, `kind`, `type`) |
| `malloy_session()` | The session model's statements |
| `malloy_version()` | The Malloy version compiled in |

`model :=` compiles against a model file instead of the session model.
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
  tables and uncommitted changes are visible. Column types become Malloy
  types through Malloy's own DuckDB type parser, so `STRUCT` and `LIST`
  columns are records and arrays, as in any other Malloy DuckDB connection.

`malloy()` then hands DuckDB the generated SQL as a subquery (the same
mechanism as DuckDB's `query()` function), and DuckDB plans and runs it.

Statements like `run:` reach the extension through DuckDB's parser
override hook, which sees each statement before DuckDB splits it at
semicolons (Malloy uses semicolons inside braces) and turns it into a call
to one of the functions above.

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
- Every query re-reads the model and re-describes its tables; nothing is
  cached yet.
- The session model belongs to one connection and ends with it.
- A Malloy statement in the same script as the `LOAD malloy` before it
  (for example in one `duckdb -c '...'`) is a syntax error: DuckDB parses
  the whole script before the extension loads. Use `-cmd` for the `LOAD`.
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
| `src/malloy_extension.cpp` | Table functions, statement recognition, the session model |
| `src/malloy_engine.cpp` | QuickJS runtime and the C++ side of the compile loop |
| `js/driver.js` | The JavaScript side of the compile loop |
| `js/malloy-embedded.js` | The Malloy compiler bundle (generated; see `js/BUNDLE_SOURCE`) |
| `cmake/embed_js.cmake` | Compiles the JavaScript into the extension as byte arrays |
| `test/sql/malloy.test` | SQL tests; models in `test/models/` |
| `duckdb`, `extension-ci-tools`, `third_party/quickjs` | Submodules |
