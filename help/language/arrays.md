---
description: Arrays and nested data — unnest with a path (genres.each, hits.page), or query each row's array in place with ->
---

# Arrays and nested data

DuckDB columns can hold arrays: `VARCHAR[]` (a list of values) or
`STRUCT(...)[]` (a list of records). `DESCRIBE` shows them. Malloy reads them
two ways.

## Unnesting: a path into the array

A path through an array unnests it, as if it were a `join_many`: the query
sees one row per element. The element of an array of values is `each`; the
fields of an array of records are reached by name.

    run: titles -> {
      group_by: genre is genres.each
      aggregate: title_count
      order_by: title_count desc
    }

    run: people -> {
      group_by: by_year.startYear
      aggregate: acting is by_year.year_ratings_acting.sum()
    }

Use this to ask about the elements across rows: titles per genre, hits by
page. Aggregates stay correct (`title_count` counts titles, not
title-genre pairs). But a `where:` on an element filters elements, not rows:
`where: genres.each = 'Comedy'` keeps only the Comedy element of each title,
so grouping by `genres.each` then shows only Comedy.

## Querying in place: the arrow

`array -> { ... }` runs a query on the current row's array, and its result is
a value of that row: a dimension, not a measure. Nothing is unnested and the
row isn't fanned out. Inside the braces, names are the element's own
(`each`, or `startYear`, not `by_year.startYear`). It is experimental, so the
file needs `##! experimental.array_queries` at the top.

    ##! experimental.array_queries
    source: titles is duckdb.table('titles') extend {
      measure: title_count is count()
      dimension:
        genre_count is genres -> { aggregate: n is count() }
        is_comedy is genres -> {
          aggregate: n is count() { where: each = 'Comedy' }
        } > 0
        is_drama is genres -> {
          aggregate: n is count() { where: each = 'Drama' }
        } > 0
    }

    source: people is duckdb.table('names') extend {
      measure: person_count is count()
      dimension:
        first_year is by_year -> { aggregate: n is startYear.min() }
        last_year is by_year -> { aggregate: n is startYear.max() }
        career_years is last_year - first_year
    }

A fact about each row becomes something to group by or filter on:

    run: titles -> { group_by: genre_count; aggregate: title_count }

    -- comedies that aren't dramas; a where: on genres.each couldn't say this
    run: titles -> { where: is_comedy and not is_drama; aggregate: title_count }

    -- every genre of the comedies, not only 'Comedy'
    run: titles -> { where: is_comedy; group_by: genres.each; aggregate: title_count }

    -- a dimension made by an arrow can be aggregated
    run: people -> { aggregate: avg_career is career_years.avg() }

Use the arrow when the question is about each row, from its array. Use a
path when it's about the elements across rows.

## What an arrow returns

- One `aggregate:` with one field: that value.
- `select:`, `group_by:`, or several stages: an array of records, which can
  be selected, or reached into with a path:

      dimension: best_year is by_year -> {
        select: startYear, year_ratings_acting
        order_by: year_ratings_acting desc
        limit: 1
      }
      run: people -> { group_by: best_year.startYear; aggregate: person_count }

- One `aggregate:` with several fields: an error.

Inside the arrow only the element's fields are visible, not the row's other
columns. Written inline in an aggregate, use the function form:
`sum(genres -> { aggregate: n is count() })`; `(genres -> {...}).sum()`
does not parse.

Arrays inside the elements can be aggregated too: for `hits` whose elements
have a `product` array, `hits -> { aggregate: n is product.quantity.sum() }`.

More, with worked examples:
https://docs.malloydata.dev/documentation/experiments/array_queries
