#pragma once

#include "duckdb.hpp"

#include <functional>

namespace duckdb {

//! Recognizing Malloy in what is typed to DuckDB: enough of a tokenizer to
//! find statement boundaries, embedded `%{ }%` queries and identifiers,
//! skipping strings and comments. Not a parser.

//! If `script` contains any Malloy (statements starting with `run:`,
//! `source:`, `query:` or `malloy`, or SQL with `%{ }%` in it), returns the
//! SQL that runs the whole script, every statement rewritten into calls to
//! the extension's table functions. Returns an empty string for a script
//! with no Malloy in it, which DuckDB should parse as it is.
string MalloyRewriteScript(const string &script);

//! Replaces each `%{ query }%` in a SQL statement with `replace(query,
//! parenthesized)`. `parenthesized` is true for `(%{ query }%)`, whose
//! parentheses are kept. MalloySQL's embedding syntax.
string MalloyExpandEmbedded(const string &sql, const std::function<string(const string &query, bool parenthesized)> &replace);

//! True if `text` is a Malloy `run:` statement.
bool MalloyIsRunStatement(const string &text);

//! Every name-like token in Malloy text: identifiers and `backquoted`
//! names, outside strings and comments.
unordered_set<string> MalloyIdentifiers(const string &text);

} // namespace duckdb
