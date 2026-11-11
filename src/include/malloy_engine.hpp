#pragma once

#include "duckdb.hpp"

namespace duckdb {

//! Runs the Malloy compiler (bundled JavaScript) in an embedded QuickJS
//! runtime. There is one runtime per process, created on first use and
//! serialized by a recursive mutex: compiles are short, and a compile can
//! re-enter (binding a view that itself uses malloy()).
//! The model a compile runs against: a Malloy file at `url`, or, when
//! `in_memory` is set, the source `text` standing in for that file (the
//! connection's session model). Imports resolve relative to `url`.
struct MalloyModel {
	string url;
	bool in_memory = false;
	string text;
};

//! Runs the Malloy compiler (bundled JavaScript) in an embedded QuickJS
//! runtime. There is one runtime per process, created on first use and
//! serialized by a recursive mutex: compiles are short, and a compile can
//! re-enter (binding a view that itself uses malloy()).
class MalloyEngine {
public:
	//! Malloy query text -> DuckDB SQL. Throws a DuckDB exception on a Malloy
	//! error.
	static string ToSQL(ClientContext &context, const MalloyModel &model, const string &query);

	//! Every source and named query the model can see, imports included:
	//! (kind, name, description, primary_key, filters, joins, fields) rows.
	static vector<vector<string>> ListSources(ClientContext &context, const MalloyModel &model);

	//! The fields of the source or named query `name`, through its joins and
	//! nested data: (path, kind, type, grain, description, definition, usage,
	//! example).
	static vector<vector<string>> Describe(ClientContext &context, const MalloyModel &model, const string &name);

	//! Checks that `statements` compile when added to the session model
	//! (`prefix`, imports of the installed sources it needs, then `session`),
	//! and returns (kind, name) rows for what they newly define. Changes
	//! nothing; the caller appends them.
	static vector<vector<string>> Define(ClientContext &context, const string &url, const string &prefix,
	                                     const string &session, const string &statements);

	//! Compiles the model file at `url`. Rows: (file, url, contents) for every
	//! file read, (entry, kind, name) for every export, (warning, message).
	static vector<vector<string>> Collect(ClientContext &context, const string &url);

	//! The Malloy version the bundle was built from.
	static string MalloyVersion();
};

//! Turns what a user typed for a model location into a URL the compiler can
//! resolve imports against: URLs pass through; paths become file:// URLs,
//! made absolute against the working directory.
string MalloyModelURL(ClientContext &context, const string &location);

} // namespace duckdb
