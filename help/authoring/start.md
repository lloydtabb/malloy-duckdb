---
description: Start here to write a Malloy model in DuckDB — look at the data, write sources, check them, test with malloy import, then malloy install
---

# Writing a Malloy model in DuckDB

A model is one or more `.malloy` files on disk. Edit them with your own file
tools; the extension compiles and tests them; `malloy install` copies the
finished model into the database, where anyone who opens it can query it.
Inside DuckDB the connection is always `duckdb`: `duckdb.table('flights')`,
`duckdb.sql("""SELECT ...""")`.

In the DuckDB CLI, end each statement with `;`, and use `.mode list` (or
`.mode jsonlines` for nested results) so long output isn't cut off.

## 1. Look at the data first

Use plain SQL. It's faster than guessing:

    SHOW TABLES;
    DESCRIBE flights;
    SUMMARIZE flights;          -- per column: distinct count, nulls, min, max
    SELECT * FROM flights LIMIT 5;

`SUMMARIZE` finds the keys (a column with as many distinct values as rows)
and the grain (what one row is). DuckDB reads files as tables too:
`DESCRIBE 'data/flights.parquet'`.

If you don't know what the model should answer, ask the person. They know
the data and which questions matter.

## 2. A base source per table, no joins

A base is what's in one table and what's computable from it alone:

    // flights_base.malloy
    source: flights_base is duckdb.table('flights') extend {
      primary_key: id
      measure: flight_count is count()
      dimension: route is concat(origin, '-', destination)
    }

Check each file as you go. `malloy check` compiles it and reports every
error with its file, line and column, without changing anything:

    malloy check 'flights_base.malloy';

Name a table, or a file by its path. For a database other people will open,
the data must travel with it: use tables in the database, or `s3://` and
`https://` files. A relative file path only works where DuckDB runs, and
`malloy install` warns about it. Use `duckdb.sql("""...""")` only when a
table or file can't express what you need.

## 3. Join the bases into the sources people query

Import the bases, join them, and export exactly what should be queried:

    // index.malloy
    import "flights_base.malloy"
    import "carriers_base.malloy"

    source: flights is flights_base extend {
      join_one: carriers is carriers_base with carrier
    }
    source: carriers is carriers_base extend {
      join_many: flights is flights_base on flights.carrier = code
    }

    export { flights, carriers }

- `join_one ... with` needs a `primary_key` on the joined source. Use
  `join_one` when each row matches at most one joined row, `join_many` when
  it can match several.
- Imported names are private. Base sources stay internal unless exported.
- Without an `export` statement, everything you define is public. With one,
  only the names listed are, and those are what `malloy install` installs.

## 4. Document what you export

Descriptions are what an agent reads first, in `malloy sources` and
`malloy describe`, so write them for someone choosing and querying a source:

    ##" Flights, carriers and airports, 2000-2005, from the FAA.

    #" One row per flight: delays, distance and routes by carrier and day.
    #(agent) Delays are in minutes. Cancelled flights have no arr_delay.
    source: flights is ...

    #" Flights that left more than 15 minutes late.
    measure: late_count is count() { where: dep_delay > 15 }

`##"` describes the model, `#"` a source or field in one sentence, and
`#(agent)` says what someone writing a query must know: grain, the measure to
prefer, a filter that is almost always wanted. More in
`malloy help develop/documenting-models`.

## 5. Test it

Load the model into your session and look at it:

    malloy import 'index.malloy';
    malloy sources;
    malloy describe flights;

Run the queries that matter, and check the numbers against SQL:

    run: flights -> { aggregate: flight_count };
    SELECT count(*) FROM flights;
    run: flights -> { group_by: carriers.nickname; aggregate: flight_count };

`malloy sql run: ...` shows the SQL a query compiles to, and
`malloy compile run: ...` its output fields and annotations. Files are read
again on every query, so after an edit, just run the query again; `malloy
reset` clears the session if you want to start over.

## 6. Install it

    malloy install 'index.malloy';

This compiles the model, refuses it if it has errors, and copies it and every
file it imports into the database. Check it from a new session:
`malloy sources`. After a change, install again; `malloy update` reinstalls
every model from where it was installed from.

## Writing Malloy

Don't guess syntax or field names: `malloy check` and `malloy describe` are
ground truth, and an error names what's wrong. `malloy help writing-malloy-with-mcp`
has the common errors, and `malloy help` lists the language reference topics
(`language/joins`, `language/aggregate-locality-symmetric-aggregates`...).
