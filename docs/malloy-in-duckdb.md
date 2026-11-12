# Malloy in DuckDB

> **Status: early prototype.** Everything here is implemented and tested
> against DuckDB v2.0.0 on macOS. Not yet tried with MotherDuck
> beyond a simple query. Building and loading the extension are in the
> [README](../README.md).

Use [Malloy](https://www.malloydata.dev) inside DuckDB SQL. Install your
Malloy model into a database, and anyone who opens that database can query
its sources: no files to find, nothing to host, nothing to import.

```sql
SELECT genre, movie_count
FROM %{ movies -> by_genre }%
WHERE movie_count > 100;
```

SQL stays in charge. Malloy goes wherever a table could, between `%{` and
`}%`, the same embedding as [MalloySQL](https://docs.malloydata.dev/documentation/malloy_sql/).

## Getting started

### Load the extension

Start DuckDB (v2.0.0) and load the extension:

```sql
LOAD malloy;
```

Until the extension is published, load your own build instead. Start DuckDB
with `-unsigned` and give an absolute path (macOS refuses relative ones):

```sql
LOAD '/path/to/malloy-duckdb/build/release/extension/malloy/malloy.duckdb_extension';
```

### Ask what's there

```sql
malloy sources;
```

This lists every source you can query, with its description: those
installed in this database, and any you defined in this session. A new
database has none.

### Embed a Malloy query

Inside `%{ }%`, write a Malloy query: a source, `->`, and what to compute.
With no model at all, the source can be a table:

```sql
SELECT * FROM %{
  duckdb.table('flights') -> {
    group_by: carrier
    aggregate: flight_count is count()
  }
}%;
```

The extension compiles the Malloy and puts the SQL it compiles to in its
place, as a subquery. Everything around it is ordinary DuckDB SQL, so a
Malloy query can be filtered, joined, used in a `CREATE TABLE ... AS`, or
embedded more than once in the same statement:

```sql
CREATE TABLE carrier_summary AS
SELECT c.nickname, f.flight_count
FROM %{ flights -> by_carrier }% AS f
JOIN carriers AS c ON c.code = f.carrier;
```

To just run a query, write it as you would in a `.malloy` file:

```sql
run: flights -> by_carrier;
```

Statements end with `;` in the DuckDB CLI, as in SQL. Malloy's own
semicolons inside `{ }` are fine.

Nested results (from `nest:`) are DuckDB lists of structs, which the CLI's
table view prints in one wide column. `.mode jsonlines` prints each row as
one JSON object instead, nests included. `malloy compile` gives the result's
shape and annotations, to read alongside it.

### Get help

```sql
malloy;
```

This lists the `malloy` commands. `malloy help` lists Malloy help topics,
the same ones as malloyyo's `yo_help`, and `malloy help <topic>` shows one.

## Installing your Malloy model

Installing copies a model into the database. After that, everyone who opens
the database can use its sources by name, and the model files don't need to
be anywhere but the database.

### 1. Install it from your files

Start DuckDB in the directory you work in, and install the model file:

```sql
malloy install 'models/imdb.malloy';
```

This reads the model from your disk and compiles it, so a model with errors
is refused and nothing changes. It then stores a copy of the model, and of
every file it imports, in the database. It lists the sources and named
queries it installed:

```
┌────────┬─────────┬─────────────┐
│  name  │  kind   │    model    │
├────────┼─────────┼─────────────┤
│ movies │ source  │ imdb.malloy │
│ people │ source  │ imdb.malloy │
│ top_10 │ query   │ imdb.malloy │
└────────┴─────────┴─────────────┘
```

Only what the model exports is installed. A source the model imports from
another file is installed only if the model exports it.

The model is named by its path among the files it was installed with:
`imdb.malloy`, or `models/imdb.malloy` if it imports from `../shared/`.

Imports keep working inside the database, wherever the imported files were
on your disk, including `../shared/common.malloy`.

You can also install from a URL, such as
`https://raw.githubusercontent.com/acme/models/main/imdb.malloy`, or from
`s3://`. The model is copied the same way; the URL isn't read again.

### 2. Explore it

```sql
malloy sources;
```

```
┌────────┬────────┬─────────────────────┬──────────────────────────────┬─────────────┬─────────────────┬────────────────────────────┬─────────────────────────────────┐
│  name  │  kind  │        from         │         description          │ primary_key │     filters     │           joins            │             fields              │
├────────┼────────┼─────────────────────┼──────────────────────────────┼─────────────┼─────────────────┼────────────────────────────┼─────────────────────────────────┤
│ movies │ source │ memory: imdb.malloy │ Movies, one row per title    │ id          │ released > 1900 │ credits (many), lead (one) │ 6 dimensions, 1 measure, 1 view │
│ people │ source │ memory: imdb.malloy │ People who work in film      │ id          │                 │                            │ 2 dimensions, 1 measure         │
└────────┴────────┴─────────────────────┴──────────────────────────────┴─────────────┴─────────────────┴────────────────────────────┴─────────────────────────────────┘
```

Each source and named query, with where it comes from (`session`, or the
database and model it's installed from), its description, primary key,
source-level filters, joins with their relationships, and a count of its
own fields.

```sql
malloy describe movies;
```

describes a source compactly, one line per field: its own dimensions,
measures and views, then each join and nested array with the fields it adds.
Each line has the field's type, its definition if it isn't a column, its
description after `--`, and its other annotations:

```
source movies -- Movies, one row per title
  primary_key: id
dimensions:
  decade :: number = floor(released / 10) * 10  -- The decade the movie was released in  # number="####"
measures:
  movie_count = count()  -- How many movies
views:
  by_genre  -- Movies per genre  # bar_chart
joins and nested data:
  credits :: join_many  -- Everyone credited on the movie  e.g. aggregate: n is credits.count()
    role :: string
  lead :: join_one  e.g. group_by: lead.id
    name :: string
```

Use `.mode list` in the DuckDB CLI so long lines aren't cut off. For
everything in columns, including a usage sentence and example clauses for
every field:

```sql
SELECT * FROM malloy_describe('movies');
```

| Column | Holds |
|---|---|
| `name` | The field's path, as a query writes it: `decade`, `lead.name`, `credits.role`, `genres.each` |
| `kind` | `dimension`, `measure`, `calculation`, `view` or `join` |
| `type` | `string`, `number`, `string[]`, `{name :: string}[]`, `join_one`, `join_many`... |
| `grain` | Empty for one value per row of the source. Otherwise the `join_many` or nested array the field is per: `credits` for `credits.role` |
| `description` | The field's description |
| `annotations` | Its other annotations, as written: `# number="####"`, `#(agent) ...` |
| `definition` | The Malloy expression that defines it: `count()`, `floor(released / 10) * 10` |
| `usage` | How a query uses it, in a sentence |
| `example` | Clauses that use it, ready to paste inside `run: movies -> { ... }` |

For example, through a `join_many`:

| name | kind | grain | example |
|---|---|---|---|
| `credits` | join | | `aggregate: n is credits.count(); group_by: credits.movie_id; aggregate: total is credits.movie_id.sum()` |
| `credits.role` | dimension | `credits` | `group_by: credits.role; aggregate: n is count(credits.role)` |
| `lead.person_count` | measure | | `aggregate: lead.person_count` |
| `genres` | dimension | | `group_by: genres.each; aggregate: n is genres.count()` |
| `directors.name` | dimension | `directors` | `group_by: directors.name; aggregate: n is count(directors.name)` |

A join's examples use its first column; every field of the join has its own
row with its own examples.

The grain is what to watch in calculations:

- **Grouping by a field with a grain gives a row per row of the grain.**
  `group_by: credits.role` gives a row per credit role. Measures of the
  source, such as `movie_count`, still count each movie once.
- **Aggregating a joined field runs over the rows of the source it comes
  from, each counted once**, through a `join_one` or a `join_many`:
  `lead.person_count` counts distinct lead people, not movies.
- **Count rows of a join or array with `.count()`**: `credits.count()`,
  `genres.count()`. An aggregate expression needs a name:
  `aggregate: n is credits.count()`.

Every example in the `example` column is checked by compiling it, so it
works as written.

To query it:

```sql
SELECT * FROM %{ movies -> by_genre }%;
run: movies -> { group_by: credits.role; aggregate: movie_count };
```

When a query names a source that isn't defined in the session, the
extension finds it among the installed sources and loads its model from the
database, and only the models the query needs.

### Describe your model

Descriptions come from the model. Write one with `#"` above a source, a
join, a field or a query (Malloy's description annotation; `#(docs)` also
works):

```malloy
#" Movies, one row per title
source: movies is duckdb.table('movies') extend {
  #" The decade the movie was released in
  dimension: decade is floor(released / 10) * 10
  #" Everyone credited on the movie, one row per credit
  join_many: credits is duckdb.table('credits') on credits.movie_id = id
}
```

A table's own columns can't be annotated. To describe one, define a
dimension from it with a new name.

### Make the data travel too

Installing copies the model, not its data. A model that reads
`duckdb.table('data/movies.parquet')` names a file relative to wherever
DuckDB is running, so it works on your machine and nowhere else.

For a database that other people will open, the model's sources should be
tables in that database, or files at an `s3://` or `https://` location:

```sql
CREATE TABLE movies AS SELECT * FROM 'data/movies.parquet';
```

and in the model, `duckdb.table('movies')`. `malloy install` warns when a
model reads a file by a relative path: its output has a row of kind
`warning` saying which file.

### In scripts

Each `duckdb` process is a new session, so a script that `malloy import`s a
model compiles it again every time. Install it into a database file once
instead, and every later run can use it:

```sh
duckdb movies.duckdb -c "LOAD malloy; malloy install 'models/imdb.malloy';"
duckdb movies.duckdb -cmd "LOAD malloy" -c "run: movies -> by_genre;"
```

### Sharing with MotherDuck

Installed models are stored in the database, so they travel with it.
Install a model into a MotherDuck database, from your own files, and
everyone who attaches that database can query its sources:

```sql
ATTACH 'md:analytics';
USE analytics;
malloy install 'models/imdb.malloy';
```

### Updating and removing

The database keeps its copy until you install again. Editing your files
changes nothing for anyone until then:

```sql
malloy install 'models/imdb.malloy';
```

Installing a model again replaces its stored copy, its imports and its list
of sources. To reinstall every model from where it was installed from,
without retyping paths:

```sql
malloy update;
```

That reads the original files, so it works on a machine that has them.

To remove a model and its sources, or one source:

```sql
malloy uninstall 'imdb.malloy';
malloy uninstall movies;
```

To get the installed files back out, to recover them or to see exactly
what's installed:

```sql
malloy export 'installed-models/';
```

## Testing changes

Your files and the installed copy are separate, so you can change your
files and test them before anyone else sees anything. Import to test;
install to publish:

| | `malloy import` | `malloy install` |
|---|---|---|
| Reads | Your files (or a URL), on every query | Your files (or a URL), once |
| Stored | Nowhere; this connection remembers where the files are | In the database, as a copy |
| Seen by | This connection only | Everyone who opens the database |
| Lasts | Until `malloy reset` or you disconnect | Until you install again or uninstall |
| Edits to your files | Show up on your next query | Show up only when you install again |

The loop is: edit, `malloy import`, try queries, repeat, then
`malloy install` when it's right.

### Try the change in your session

Edit your model files, then import the model into your session:

```sql
malloy import 'models/imdb.malloy';
```

`malloy import` loads a model for this connection only, without installing
it, and says in one line what it added:
`imported 2 sources (movies, people), 1 query (top_10)`. Sources in your session take precedence over installed ones with the
same name, so from here on `movies` means the version on your disk, and
nobody else sees it.

Run the queries that matter, and compare the SQL if you need to:

```sql
run: movies -> by_genre;
malloy sql SELECT * FROM %{ movies -> by_genre }%;
```

`malloy sql` shows a statement with its Malloy compiled to SQL, without
running it.

You can also try a small change without editing a file, by defining a new
source in the session. A statement that starts with `source:`, `query:` or
`run:` is Malloy, like a Malloy cell in a MalloySQL notebook:

```sql
source: movies_by_decade is movies extend {
  dimension: decade is floor(year / 10) * 10
};
run: movies_by_decade -> { group_by: decade; aggregate: movie_count };
```

A session can't redefine a name it can already see, installed or not:
`source: movies is movies extend {...}` is an error. To change an installed
source, change the model file and `malloy import` it.

### Check the whole model

```sql
malloy check 'models/imdb.malloy';
```

This compiles the model and reports every error with its file, line and
column, without installing anything or changing your session. It also
lists the sources the model exports.

### Go back to the installed version

```sql
malloy session;   -- what your session has imported and defined
malloy reset;     -- forget it all; installed sources apply again
```

### Publish the change

```sql
malloy install 'models/imdb.malloy';
```

The next query anywhere uses it.

## Reference

### Embedded Malloy

| Form | Means |
|---|---|
| `%{ <query> }%` | A Malloy query, anywhere SQL takes a table or subquery. Replaced by the SQL it compiles to, in parentheses |
| `(%{ <query> }%)` | The same; the parentheses are kept, not doubled |
| A statement that is only `%{ <query> }%` | Runs the query, as `run:` does |

`<query>` is a Malloy query, as after `run:` in a `.malloy` file:
`flights -> by_carrier`, `flights -> { group_by: carrier }`, or the name of
a named query.

Embedded Malloy may contain comments and newlines. It may not contain `}%`.

### Malloy statements

A statement that starts with one of these is Malloy, not SQL:

| Statement | Does |
|---|---|
| `run: ...` | Runs a query |
| `source: ...`, `query: ...` | Defines a source or named query in this session |

### `malloy` commands

| Command | Does |
|---|---|
| `malloy` | Lists the commands |
| `malloy help [<topic>]` | Lists the Malloy help topics, or shows one: `malloy help language/joins`, or just `malloy help joins`. The topics are malloyyo's `yo_help` topics, with the same names. Use `.mode list` in the DuckDB CLI so long lines aren't cut off |
| `malloy sources` | Lists the sources and named queries you can use: where each comes from, description, primary key, filters, joins, fields |
| `malloy describe <name>` | Describes a source compactly: its fields grouped by kind, then its joins and nested data, with definitions, descriptions and annotations. `malloy_describe('<name>')` has it all in columns, with usage and examples |
| `malloy sql <statement>` | Shows a statement with its Malloy compiled to SQL |
| `malloy compile <query>` | A query's result metadata as JSON, without running it: its SQL, its output fields (nested ones included) with their annotations, and the query's, source's and model's annotations. Malloy's own result format, without the data. Takes a `run:` statement or a single `%{ }%` |
| `malloy import '<location>'` | Loads a model into this session only, from your files or a URL |
| `malloy install '<location>'` | Copies a model and its imports into this database and installs its exported sources |
| `malloy update` | Installs every model again, from where it was installed from |
| `malloy uninstall '<model>'`, `malloy uninstall <name>` | Removes a model and its sources, or one source |
| `malloy export '<directory>'` | Writes the installed model files to a directory |
| `malloy check '<location>'` | Compiles a model and reports errors, changing nothing |
| `malloy session` | Lists what this session has imported and defined |
| `malloy reset` | Clears this session's imports and definitions |

Each command also exists as a table function for use from SQL, such as
`malloy_sources()` and `malloy_describe('movies')`.

### Where a name is found

When a query names a source, the extension looks in order:

1. this session's definitions and imports;
2. the sources installed in the current database;
3. the sources installed in each other attached database, in the order they
   were attached.

The first match wins.

### How installed models are stored

Installed models are rows in two ordinary tables in a `malloy` schema, which
`malloy install` creates if it doesn't exist:

`malloy.files`, one row per model file:

| Column | Type | Holds |
|---|---|---|
| `model` | `VARCHAR` | The model it was installed with, such as `imdb.malloy` |
| `path` | `VARCHAR` | The file's path among that model's files, such as `imdb.malloy` or `shared/common.malloy` |
| `contents` | `VARCHAR` | The file's Malloy source |
| `installed_from` | `VARCHAR` | Where it was read from at install, for `malloy update` |
| `installed_at` | `TIMESTAMP` | When |

Each model's files are kept apart, so two models can import different
versions of a file with the same path.

`malloy.sources`, one row per installed source or named query:

| Column | Type | Holds |
|---|---|---|
| `name` | `VARCHAR` | The name, as queries use it |
| `kind` | `VARCHAR` | `source` or `query` |
| `model` | `VARCHAR` | The model that exports it |

A name can be installed from only one model per database; installing a
second model that exports the same name is refused.

You can read these with SQL, and copy them between databases to copy the
installed models. Change them through `malloy install` and
`malloy uninstall`, which keep the two consistent.

### Other databases

The two tables are a convention, not a DuckDB feature: string columns in a
`malloy` schema (a dataset, in BigQuery). Any database can hold them, and
`%{ }%` is already how MalloySQL embeds Malloy in any dialect's SQL, so
other Malloy tools can find installed models and expand embedded queries
the same way. A model only runs against the kind of database its sources
were written for: a model whose sources are `duckdb.table(...)` runs in
DuckDB and MotherDuck.

### Trust

Whoever can write the `malloy` tables decides what installed models
contain. A model can only produce SQL that runs with your own permissions,
the same as SQL you type yourself, but limit who can write those tables in
a shared database as you would any other table.

## Not yet supported

- Timestamp literals and timezones in Malloy (`@2003-01-15`, `timezone:`).
- Caching: each query recompiles the models it uses, which takes tens of
  milliseconds, more for large models.
