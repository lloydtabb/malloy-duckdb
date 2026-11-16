# malloy-duckdb

A DuckDB extension for using [Malloy](https://www.malloydata.dev) inside
DuckDB SQL. Install a Malloy model into a database, and anyone who opens it
can query the model's sources.

> **Status: early prototype.** Works against DuckDB v1.5.6 on Linux and
> macOS; see [Status](#status).

```sql
LOAD malloy;

-- Copy a model, and every file it imports, into this database.
malloy install 'models/imdb.malloy';
malloy sources;
malloy describe movies;

-- Malloy inside SQL, anywhere a table can go ...
SELECT genre, movie_count
FROM %{ movies -> by_genre }%
WHERE movie_count > 100;

-- ... or on its own.
run: movies -> by_genre;

-- Test a change to your files in this session only, then publish it.
malloy import 'models/imdb.malloy';
malloy install 'models/imdb.malloy';
```

The full guide, covering getting started, installing models and testing
changes, is [docs/malloy-in-duckdb.md](docs/malloy-in-duckdb.md). `malloy`
on its own lists the commands. In the DuckDB CLI, end each statement with
`;`, as for SQL.

## How it works

The Malloy compiler is TypeScript. This extension embeds
[QuickJS-ng](https://github.com/quickjs-ng/quickjs), a small JavaScript
engine written in C, and runs Malloy in it as one bundled script,
`js/malloy-embedded.js`, built by `bundle/build.mjs`. The bundle holds
Malloy's own packages, so the extension is an ordinary Malloy host (a
`Runtime` with a URL reader and a DuckDB connection), and the core of
malloyyo's MCP engine, so `malloy sources` and `malloy describe` answer as
malloyyo's `list_sources` and `describe_source` do.

- **Recognizing Malloy.** DuckDB's parser override hook offers the
  extension every script before DuckDB parses it. `src/malloy_statements.cpp`
  splits it into statements and rewrites the Malloy in it (`run:`,
  `source:`, `query:`, `malloy` commands, and `%{ }%` inside SQL, following
  MalloySQL's syntax) into SQL calling the extension's table functions.
  Scripts with no Malloy are left for DuckDB to parse as usual.
- **Compiling.** `js/driver.js` keeps one Malloy `Runtime` for the
  process. It calls back into C++ (`src/malloy_engine.cpp`) for files, read
  through DuckDB's file system or, for installed models, from the database,
  and for the schemas of `duckdb.table(...)` and `duckdb.sql(...)` sources,
  found by binding `SELECT * FROM <source>` in the calling connection.
  Malloy's DuckDB connection code turns those into Malloy types. The Runtime
  API returns promises; the C++ side runs QuickJS's job queue until each
  one settles.
- **Caching.** Models compiled for queries and for adding to the session
  are kept between statements by Malloy's `CacheManager`. A file's cache
  key is what it says: for an installed model, the digest `malloy install`
  wrote; otherwise its text. Beside each cached model the extension keeps
  the schemas its compile described, and reuses the model only while each
  is unchanged, bound again in the calling connection. So a reinstalled
  model, `CREATE OR REPLACE TABLE` (on a table read by any file the model
  imports), or another connection's temporary table means a recompile.
  `malloy sources`, `malloy describe` and `malloy install` always compile
  afresh.
- **Running.** `malloy()` hands DuckDB the generated SQL as a subquery (the
  same mechanism as DuckDB's `query()` function), and DuckDB plans and runs
  it.
- **The session model.** Each connection keeps the statements it has
  imported and defined, and compiles queries against them. A query that
  names an installed source also gets an `import` of that source's model,
  so only the models a query uses are compiled. A view defined with
  `malloy()` is a database object, so its Malloy sees installed sources but
  not the session's statements.
- **Installed models** are rows in `malloy.files` and `malloy.sources`
  (`src/malloy_store.cpp`), read and written through a second connection to
  the same database. Install also writes each file's digest, its cache key.

## Status

Early prototype. Built and tested against the official DuckDB v1.5.6 CLI
on Linux x86-64 and macOS arm64; other platforms not yet tried.

Compile speed in QuickJS, on 4 cores with the two-file flights model:

| | Time |
|---|---|
| First compile in a process (creates the JS runtime, loads the 2.3 MB bundle) | ~3 s |
| A query against a model already compiled | ~30 ms |

Known gaps:

- The session model belongs to one connection and ends with it.
- Not yet tried with MotherDuck beyond a simple query.
- Installed models in a database whose name is not a plain identifier
  (`ATTACH ... AS "My Db"`) are not found.
- An error in a Malloy statement is reported with the SQL it was rewritten
  into (`LINE 1: SELECT * FROM malloy(...)`), not the text you typed.
- A Malloy statement in the same script as the `LOAD malloy` before it
  (for example in one `duckdb -c '...'`) is a syntax error: DuckDB parses
  the whole script before the extension loads. Use `-cmd` for the `LOAD`.
- CI (`.github/workflows/MainDistributionPipeline.yml`) builds
  `linux_amd64` and `osx_arm64` only.

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

To run the SQL tests against that CLI, without a full build:

```sh
scripts/run-tests-cli.py
```

The full build, which also builds DuckDB, its shell with the extension
linked in, and the test runner for `test/sql/*.test`:

```sh
GEN=ninja make release
make test
```

To pick up a newer Malloy or malloyyo, rebuild the bundle and the help
topics from built checkouts of both (Malloy: `npm run build`; malloyyo:
`npm run gen -w packages/mcp-engine`), then build the extension again:

```sh
(cd bundle && npm install)
node bundle/build.mjs /path/to/malloy /path/to/malloyyo
scripts/update-help.py /path/to/malloyyo
```

## Layout

| Path | What |
|---|---|
| `src/malloy_extension.cpp` | Table functions, the session model, registration |
| `src/malloy_statements.cpp` | Recognizing and rewriting Malloy statements and `%{ }%` |
| `src/malloy_store.cpp` | Installed models and their cache keys: the `malloy.files` and `malloy.sources` tables |
| `src/malloy_engine.cpp` | QuickJS runtime, and the files and schemas the driver asks for |
| `js/driver.js` | The Malloy host: Runtime, model cache, and the extension's commands |
| `js/malloy-embedded.js` | Malloy and malloyyo's engine core as one script (generated; see `js/BUNDLE_SOURCE`) |
| `bundle/` | Builds `js/malloy-embedded.js`, with the web-platform globals QuickJS lacks |
| `cmake/embed_js.cmake` | Compiles the JavaScript into the extension as byte arrays |
| `test/sql/malloy.test` | SQL tests; models in `test/models/` |
| `scripts/run-tests-cli.py` | Runs `test/sql/malloy.test` through the DuckDB CLI, without a full build |
| `duckdb`, `extension-ci-tools`, `third_party/quickjs` | Submodules |
