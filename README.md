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
engine written in C, and runs the compiler in it as one bundled script
(`js/malloy-embedded.js`, built by Malloy's `packages/malloy-embedded`).

- **Recognizing Malloy.** DuckDB's parser override hook offers the
  extension every script before DuckDB parses it. `src/malloy_statements.cpp`
  splits it into statements and rewrites the Malloy in it (`run:`,
  `source:`, `query:`, `malloy` commands, and `%{ }%` inside SQL, following
  MalloySQL's syntax) into SQL calling the extension's table functions.
  Scripts with no Malloy are left for DuckDB to parse as usual.
- **Compiling.** The compiler does no IO itself; it asks for what it needs.
  `js/driver.js` runs that loop and calls back into C++
  (`src/malloy_engine.cpp`) for files, read through DuckDB's file system or,
  for installed models, from the database, and for the schemas of
  `duckdb.table(...)` and `duckdb.sql(...)` sources, found by binding
  `SELECT * FROM <source>` in the calling connection. Column types become
  Malloy types through Malloy's own DuckDB type parser.
- **Running.** `malloy()` hands DuckDB the generated SQL as a subquery (the
  same mechanism as DuckDB's `query()` function), and DuckDB plans and runs
  it.
- **The session model.** Each connection keeps the statements it has
  imported and defined, and compiles queries against them. A query that
  names an installed source also gets an `import` of that source's model,
  so only the models a query uses are compiled.
- **Installed models** are rows in `malloy.files` and `malloy.sources`
  (`src/malloy_store.cpp`), read and written through a second connection to
  the same database.

## Status

Early prototype. Built and tested against the official DuckDB v1.5.6 CLI
on Linux x86-64 and macOS arm64; other platforms not yet tried.

Compile speed in QuickJS, on 4 cores with the two-file flights model:

| | Time |
|---|---|
| First compile in a process (creates the JS runtime, loads the 2.2 MB bundle) | ~2.2 s |
| Each compile after that | 50–75 ms |

Known gaps:

- Timestamp literals and timezones (`@2003-01-15`, `timezone:`) don't
  compile yet: Malloy uses `luxon`, which needs `Intl`, and QuickJS has none.
- Every query recompiles the models it uses, rereading their files and
  re-describing their tables; nothing is cached yet.
- The session model belongs to one connection and ends with it.
- Not yet tried with MotherDuck beyond a simple query.
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
git clone --recurse-submodules https://github.com/lloydtabb/malloy-duckdb
cd malloy-duckdb
scripts/quick-build.sh
build/duckdb-cli/duckdb -unsigned
```

```sql
LOAD 'build/release/extension/malloy/malloy.duckdb_extension';
```

On macOS, give `LOAD` an absolute path, e.g.
`LOAD '/path/to/malloy-duckdb/build/release/extension/malloy/malloy.duckdb_extension';`.
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

To pick up a newer Malloy, rebuild the bundle from a Malloy checkout:

```sh
scripts/update-bundle.sh /path/to/malloy
```

## Layout

| Path | What |
|---|---|
| `src/malloy_extension.cpp` | Table functions, the session model, registration |
| `src/malloy_statements.cpp` | Recognizing and rewriting Malloy statements and `%{ }%` |
| `src/malloy_store.cpp` | Installed models: the `malloy.files` and `malloy.sources` tables |
| `src/malloy_engine.cpp` | QuickJS runtime and the C++ side of the compile loop |
| `js/driver.js` | The JavaScript side of the compile loop |
| `js/malloy-embedded.js` | The Malloy compiler bundle (generated; see `js/BUNDLE_SOURCE`) |
| `cmake/embed_js.cmake` | Compiles the JavaScript into the extension as byte arrays |
| `test/sql/malloy.test` | SQL tests; models in `test/models/` |
| `scripts/run-tests-cli.py` | Runs `test/sql/malloy.test` through the DuckDB CLI, without a full build |
| `duckdb`, `extension-ci-tools`, `third_party/quickjs` | Submodules |
