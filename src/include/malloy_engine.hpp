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
	//! nested data: (path, kind, type, grain, description, annotations,
	//! definition, usage, example).
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

	//! `name` described compactly, one row per line.
	static vector<vector<string>> DescribeText(ClientContext &context, const MalloyModel &model, const string &name);

	//! A query's result metadata as JSON: SQL, output schema with
	//! annotations, and the query's, source's and model's annotations.
	static string Compile(ClientContext &context, const MalloyModel &model, const string &query);

	//! Help topics (malloyyo's yo_help topics): (topic, about) rows.
	static vector<vector<string>> HelpTopics();
	//! One help topic, found by name, part of a name, or words in its text:
	//! its name, then one row per line.
	static vector<vector<string>> HelpTopic(const string &query);

	//! The Malloy version the bundle was built from.
	static string MalloyVersion();
};

//! Turns what a user typed for a model location into a URL the compiler can
//! resolve imports against: URLs pass through; paths become file:// URLs,
//! made absolute against the working directory.
string MalloyModelURL(ClientContext &context, const string &location);

} // namespace duckdb
