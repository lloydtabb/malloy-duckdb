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

	//! Every source and named query the model can see, imports included, as
	//! (kind, name) rows.
	static vector<vector<string>> ListContents(ClientContext &context, const MalloyModel &model);

	//! The fields of the source or named query `name`, as (name, kind, type)
	//! rows.
	static vector<vector<string>> Describe(ClientContext &context, const MalloyModel &model, const string &name);

	//! Checks that `statements` compile when appended to the in-memory model,
	//! and returns (kind, name) rows for what they newly define or import.
	//! Changes nothing; the caller appends them.
	static vector<vector<string>> Define(ClientContext &context, const MalloyModel &model, const string &statements);

	//! The Malloy version the bundle was built from.
	static string MalloyVersion();
};

//! Turns what a user typed for a model location into a URL the compiler can
//! resolve imports against: URLs pass through; paths become file:// URLs,
//! made absolute against the working directory.
string MalloyModelURL(ClientContext &context, const string &location);

} // namespace duckdb
