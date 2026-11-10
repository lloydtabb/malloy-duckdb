#define DUCKDB_EXTENSION_MAIN

#include "malloy_extension.hpp"
#include "malloy_engine.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Per-connection state: the loaded model
//===--------------------------------------------------------------------===//
struct MalloyClientState : public ClientContextState {
	string model_url;
};

static shared_ptr<MalloyClientState> GetMalloyState(ClientContext &context) {
	return context.registered_state->GetOrCreate<MalloyClientState>("malloy");
}

//! The model a call compiles against: its `model :=` argument if given,
//! otherwise the one malloy_load() set on this connection (possibly none).
static string ModelFor(ClientContext &context, named_parameter_map_t &named_parameters) {
	auto entry = named_parameters.find("model");
	if (entry != named_parameters.end() && !entry->second.IsNull()) {
		return MalloyModelURL(context, entry->second.ToString());
	}
	return GetMalloyState(context)->model_url;
}

static string QueryArgument(TableFunctionBindInput &input) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("malloy: the Malloy query cannot be NULL");
	}
	return input.inputs[0].ToString();
}

//===--------------------------------------------------------------------===//
// malloy(query [, model := url]): the query's rows
//===--------------------------------------------------------------------===//
static unique_ptr<TableRef> MalloyBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto query = QueryArgument(input);
	auto sql = MalloyEngine::ToSQL(context, ModelFor(context, input.named_parameters), query);

	Parser parser(context.GetParserOptions());
	parser.ParseQuery(sql);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw BinderException("malloy: the generated SQL is not a single SELECT statement:\n%s", sql);
	}
	auto select = unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
	return make_uniq<SubqueryRef>(std::move(select));
}

//===--------------------------------------------------------------------===//
// Functions that return a few rows of strings
//===--------------------------------------------------------------------===//
struct MalloyRowsData : public TableFunctionData {
	vector<vector<string>> rows;

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<MalloyRowsData>();
		copy->rows = rows;
		return std::move(copy);
	}
	bool Equals(const FunctionData &other) const override {
		return rows == other.Cast<MalloyRowsData>().rows;
	}
};

struct MalloyRowsState : public GlobalTableFunctionState {
	idx_t next = 0;
};

static unique_ptr<GlobalTableFunctionState> MalloyRowsInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<MalloyRowsState>();
}

static void MalloyRowsScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<MalloyRowsData>();
	auto &state = data.global_state->Cast<MalloyRowsState>();
	idx_t count = 0;
	while (state.next < bind.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = bind.rows[state.next++];
		for (idx_t col = 0; col < row.size(); col++) {
			output.SetValue(col, count, Value(row[col]));
		}
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// malloy_sql(query [, model := url]): the SQL a query compiles to
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> MalloySQLBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	auto query = QueryArgument(input);
	auto result = make_uniq<MalloyRowsData>();
	result->rows.push_back({MalloyEngine::ToSQL(context, ModelFor(context, input.named_parameters), query)});
	names.emplace_back("sql");
	return_types.emplace_back(LogicalType::VARCHAR);
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// malloy_load(location): set this connection's model, list what it exports
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> MalloyLoadBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("malloy_load: the model location cannot be NULL");
	}
	auto url = MalloyModelURL(context, input.inputs[0].ToString());
	// Compile it now, so a bad model fails here rather than at the first query.
	auto entries = MalloyEngine::DescribeModel(context, url);
	GetMalloyState(context)->model_url = url;

	auto result = make_uniq<MalloyRowsData>();
	for (auto &entry : entries) {
		result->rows.push_back({entry.first, entry.second, url});
	}
	if (entries.empty()) {
		result->rows.push_back({"model", "(no exported sources or queries)", url});
	}
	names = {"kind", "name", "model"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// malloy_version()
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> MalloyVersionBind(ClientContext &, TableFunctionBindInput &,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<MalloyRowsData>();
	result->rows.push_back({MalloyEngine::MalloyVersion()});
	names.emplace_back("malloy_version");
	return_types.emplace_back(LogicalType::VARCHAR);
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Bare Malloy statements: `run: flights -> by_carrier`
//===--------------------------------------------------------------------===//
//! If `query` is a Malloy statement, sets `malloy` to its text (without a
//! trailing semicolon) and returns true.
static bool IsMalloyStatement(const string &query, string &malloy) {
	idx_t start = 0;
	while (start < query.size() && StringUtil::CharacterIsSpace(query[start])) {
		start++;
	}
	if (query.compare(start, 4, "run:") != 0) {
		return false;
	}
	idx_t end = query.size();
	while (end > start && (StringUtil::CharacterIsSpace(query[end - 1]) || query[end - 1] == ';')) {
		end--;
	}
	malloy = query.substr(start, end - start);
	return true;
}

//! With `SET allow_parser_override_extension = 'fallback'`, DuckDB offers
//! every query here first, unsplit, so Malloy's own semicolons survive. A
//! Malloy statement becomes `SELECT * FROM malloy('...')`; anything else is
//! declined and parsed as SQL.
static ParserOverrideResult MalloyParserOverride(ParserExtensionInfo *, const string &query, ParserOptions &options) {
	string malloy;
	if (!IsMalloyStatement(query, malloy)) {
		return ParserOverrideResult();
	}
	try {
		ParserOptions sql_options = options;
		sql_options.extensions = decltype(sql_options.extensions)();
		Parser parser(sql_options);
		parser.ParseQuery("SELECT * FROM malloy(" + KeywordHelper::WriteQuoted(malloy, '\'') + ")");
		return ParserOverrideResult(std::move(parser.statements));
	} catch (std::exception &ex) {
		return ParserOverrideResult(ex);
	}
}

//! Without the override setting, DuckDB only offers statements it failed to
//! parse, already split at every semicolon, including those inside Malloy's
//! braces. That cannot be parsed reliably, so explain how to turn it on.
static ParserExtensionParseResult MalloyParse(ParserExtensionInfo *, const string &query) {
	string malloy;
	if (!IsMalloyStatement(query, malloy)) {
		return ParserExtensionParseResult();
	}
	return ParserExtensionParseResult(
	    "To run Malloy statements directly, first run: SET allow_parser_override_extension = 'fallback';\n"
	    "Or wrap the query: SELECT * FROM malloy('run: ...');");
}

static ParserExtensionPlanResult MalloyPlan(ParserExtensionInfo *, ClientContext &,
                                            unique_ptr<ParserExtensionParseData>) {
	throw InternalException("malloy: MalloyParse never returns a statement to plan");
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//
static void LoadInternal(ExtensionLoader &loader) {
	TableFunction malloy("malloy", {LogicalType::VARCHAR}, nullptr, nullptr);
	malloy.bind_replace = MalloyBindReplace;
	malloy.named_parameters["model"] = LogicalType::VARCHAR;
	loader.RegisterFunction(malloy);

	TableFunction malloy_sql("malloy_sql", {LogicalType::VARCHAR}, MalloyRowsScan, MalloySQLBind, MalloyRowsInit);
	malloy_sql.named_parameters["model"] = LogicalType::VARCHAR;
	loader.RegisterFunction(malloy_sql);

	TableFunction malloy_load("malloy_load", {LogicalType::VARCHAR}, MalloyRowsScan, MalloyLoadBind, MalloyRowsInit);
	loader.RegisterFunction(malloy_load);

	TableFunction malloy_version("malloy_version", {}, MalloyRowsScan, MalloyVersionBind, MalloyRowsInit);
	loader.RegisterFunction(malloy_version);

	ParserExtension parser;
	parser.parse_function = MalloyParse;
	parser.plan_function = MalloyPlan;
	parser.parser_override = MalloyParserOverride;
	ParserExtension::Register(DBConfig::GetConfig(loader.GetDatabaseInstance()), parser);
}

void MalloyExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string MalloyExtension::Name() {
	return "malloy";
}

std::string MalloyExtension::Version() const {
#ifdef EXT_VERSION_MALLOY
	return EXT_VERSION_MALLOY;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(malloy, loader) {
	duckdb::LoadInternal(loader);
}
}
