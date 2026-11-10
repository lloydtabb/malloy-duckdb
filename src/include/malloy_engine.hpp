#pragma once

#include "duckdb.hpp"

namespace duckdb {

//! Runs the Malloy compiler (bundled JavaScript) in an embedded QuickJS
//! runtime. There is one runtime per process, created on first use and
//! serialized by a recursive mutex: compiles are short, and a compile can
//! re-enter (binding a view that itself uses malloy()).
class MalloyEngine {
public:
	//! Malloy query text -> DuckDB SQL. Throws a DuckDB exception on a Malloy
	//! error. `model_url` may be empty (no model loaded).
	static string ToSQL(ClientContext &context, const string &model_url, const string &query);

	//! Compiles the model at `model_url` and returns its exported entries as
	//! (kind, name) pairs.
	static vector<pair<string, string>> DescribeModel(ClientContext &context, const string &model_url);

	//! The Malloy version the bundle was built from.
	static string MalloyVersion();
};

//! Turns what a user typed for a model location into a URL the compiler can
//! resolve imports against: URLs pass through; paths become file:// URLs,
//! made absolute against the working directory.
string MalloyModelURL(ClientContext &context, const string &location);

} // namespace duckdb
