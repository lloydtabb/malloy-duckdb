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
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Per-connection state: the session model
//===--------------------------------------------------------------------===//
//! Each connection has a Malloy document in memory, the session model, that
//! `import`, `source:` and `query:` statements append to and that queries
//! compile against. It stands in for a file in the working directory, so
//! relative imports resolve from there.
struct MalloyClientState : public ClientContextState {
	string url;
	vector<string> statements;

	MalloyModel Model() const {
		MalloyModel model;
		model.url = url;
		model.in_memory = true;
		model.text = StringUtil::Join(statements, "\n");
		return model;
	}
};

static constexpr const char *SESSION_MODEL_FILE = "duckdb-session.malloy";

static shared_ptr<MalloyClientState> GetMalloyState(ClientContext &context) {
	auto state = context.registered_state->GetOrCreate<MalloyClientState>("malloy");
	if (state->url.empty()) {
		state->url = MalloyModelURL(context, SESSION_MODEL_FILE);
	}
	return state;
}

//! The model a call compiles against: the file named by its `model :=`
//! argument if given, otherwise the session model.
static MalloyModel ModelFor(ClientContext &context, named_parameter_map_t &named_parameters) {
	auto entry = named_parameters.find("model");
	if (entry != named_parameters.end() && !entry->second.IsNull()) {
		MalloyModel model;
		model.url = MalloyModelURL(context, entry->second.ToString());
		return model;
	}
	return GetMalloyState(context)->Model();
}

static string StringArgument(TableFunctionBindInput &input, const char *function, const char *what) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("%s: %s cannot be NULL", function, what);
	}
	return input.inputs[0].ToString();
}

//===--------------------------------------------------------------------===//
// malloy(query [, model := location]): the query's rows
//===--------------------------------------------------------------------===//
static unique_ptr<TableRef> MalloyBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto query = StringArgument(input, "malloy", "the Malloy query");
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
	//! For the define functions: statements to append to the session model
	//! when the call executes (not when it binds, which can happen more than
	//! once).
	string append;

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<MalloyRowsData>();
		copy->rows = rows;
		copy->append = append;
		return std::move(copy);
	}
	bool Equals(const FunctionData &other) const override {
		auto &o = other.Cast<MalloyRowsData>();
		return rows == o.rows && append == o.append;
	}
};

struct MalloyRowsState : public GlobalTableFunctionState {
	idx_t next = 0;
};

static unique_ptr<GlobalTableFunctionState> MalloyRowsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<MalloyRowsData>();
	if (!bind.append.empty()) {
		GetMalloyState(context)->statements.push_back(bind.append);
	}
	return make_uniq<MalloyRowsState>();
}

static void MalloyRowsScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<MalloyRowsData>();
	auto &state = data.global_state->Cast<MalloyRowsState>();
	idx_t count = 0;
	while (state.next < bind.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = bind.rows[state.next++];
		for (idx_t col = 0; col < output.ColumnCount(); col++) {
			output.SetValue(col, count, Value(col < row.size() ? row[col] : string()));
		}
		count++;
	}
	output.SetCardinality(count);
}

static void VarcharColumns(vector<LogicalType> &return_types, vector<string> &names, vector<string> columns) {
	names = std::move(columns);
	return_types.assign(names.size(), LogicalType::VARCHAR);
}

//===--------------------------------------------------------------------===//
// malloy_sql(query [, model := location]): the SQL a query compiles to
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> MalloySQLBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	auto query = StringArgument(input, "malloy_sql", "the Malloy query");
	auto result = make_uniq<MalloyRowsData>();
	result->rows.push_back({MalloyEngine::ToSQL(context, ModelFor(context, input.named_parameters), query)});
	VarcharColumns(return_types, names, {"sql"});
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// malloy_define(statements): add import/source:/query: statements to the
// session model. malloy_load(location) imports a model file.
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> DefineBind(ClientContext &context, const string &statements,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<MalloyRowsData>();
	result->rows = MalloyEngine::Define(context, GetMalloyState(context)->Model(), statements);
	result->append = statements;
	VarcharColumns(return_types, names, {"kind", "name"});
	return std::move(result);
}

static unique_ptr<FunctionData> MalloyDefineBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	return DefineBind(context, StringArgument(input, "malloy_define", "the Malloy statements"), return_types, names);
}

static unique_ptr<FunctionData> MalloyLoadBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	auto url = MalloyModelURL(context, StringArgument(input, "malloy_load", "the model location"));
	auto import = "import " + KeywordHelper::WriteQuoted(url, '"');
	return DefineBind(context, import, return_types, names);
}

//===--------------------------------------------------------------------===//
// malloy_sources(): what the session model can see
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> MalloySourcesBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<MalloyRowsData>();
	for (auto &row : MalloyEngine::ListContents(context, ModelFor(context, input.named_parameters))) {
		// The driver lists kind first; show the name first.
		result->rows.push_back({row.size() > 1 ? row[1] : "", row.empty() ? "" : row[0]});
	}
	VarcharColumns(return_types, names, {"name", "kind"});
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// malloy_describe(name): the fields of a source or named query
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> MalloyDescribeBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
	auto name = StringArgument(input, "malloy_describe", "the source name");
	auto result = make_uniq<MalloyRowsData>();
	result->rows = MalloyEngine::Describe(context, ModelFor(context, input.named_parameters), name);
	VarcharColumns(return_types, names, {"name", "kind", "type"});
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// malloy_session(): the session model's Malloy source
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> MalloySessionBind(ClientContext &context, TableFunctionBindInput &,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	auto state = GetMalloyState(context);
	auto result = make_uniq<MalloyRowsData>();
	for (auto &statement : state->statements) {
		result->rows.push_back({statement});
	}
	VarcharColumns(return_types, names, {"statement"});
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// malloy_version()
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> MalloyVersionBind(ClientContext &, TableFunctionBindInput &,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<MalloyRowsData>();
	result->rows.push_back({MalloyEngine::MalloyVersion()});
	VarcharColumns(return_types, names, {"malloy_version"});
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Malloy statements typed directly
//===--------------------------------------------------------------------===//
//! A cursor over statement text, for recognizing Malloy statements: enough
//! of a tokenizer to skip whitespace, comments and annotations and read
//! words, not a parser.
class StatementScanner {
public:
	explicit StatementScanner(const string &text_p) : text(text_p) {
	}

	//! Skips whitespace, `--` and `//` line comments, and `#` annotation lines
	//! (which SQL never starts with).
	void SkipNoise() {
		while (pos < text.size()) {
			if (StringUtil::CharacterIsSpace(text[pos])) {
				pos++;
			} else if (text.compare(pos, 2, "--") == 0 || text.compare(pos, 2, "//") == 0 || text[pos] == '#') {
				while (pos < text.size() && text[pos] != '\n') {
					pos++;
				}
			} else {
				break;
			}
		}
	}

	//! Reads an identifier-like word (letters, digits, underscore), lowercased.
	string Word() {
		SkipNoise();
		idx_t start = pos;
		while (pos < text.size() && (StringUtil::CharacterIsAlphaNumeric(text[pos]) || text[pos] == '_')) {
			pos++;
		}
		return StringUtil::Lower(text.substr(start, pos - start));
	}

	//! Reads a name: a word, or a `backquoted` name, case preserved.
	string Name() {
		SkipNoise();
		if (pos < text.size() && text[pos] == '`') {
			auto close = text.find('`', pos + 1);
			if (close == string::npos) {
				return "";
			}
			auto name = text.substr(pos + 1, close - pos - 1);
			pos = close + 1;
			return name;
		}
		idx_t start = pos;
		while (pos < text.size() && (StringUtil::CharacterIsAlphaNumeric(text[pos]) || text[pos] == '_')) {
			pos++;
		}
		return text.substr(start, pos - start);
	}

	bool Peek(char c) {
		SkipNoise();
		return pos < text.size() && text[pos] == c;
	}

	//! Consumes `c` if it is next.
	bool Accept(char c) {
		if (!Peek(c)) {
			return false;
		}
		pos++;
		return true;
	}

	//! True if only whitespace, comments and semicolons remain.
	bool AtEnd() {
		while (true) {
			SkipNoise();
			if (pos < text.size() && text[pos] == ';') {
				pos++;
				continue;
			}
			return pos >= text.size();
		}
	}

private:
	const string &text;
	idx_t pos = 0;
};

//! The text with trailing whitespace and semicolons removed.
static string StripTrailingSemicolons(const string &text) {
	idx_t end = text.size();
	while (end > 0 && (StringUtil::CharacterIsSpace(text[end - 1]) || text[end - 1] == ';')) {
		end--;
	}
	return text.substr(0, end);
}

//! If `query` is a Malloy statement, returns the SQL that runs it; otherwise
//! an empty string. Recognized:
//!
//!   run: ...                             SELECT * FROM malloy('...')
//!   import ..., source: ..., query: ...  SELECT * FROM malloy_define('...')
//!   show sources | show queries          SELECT ... FROM malloy_sources()
//!   describe source|query <name>         SELECT * FROM malloy_describe('<name>')
//!   show session                         SELECT * FROM malloy_session()
//!
//! `show` may also be written `show:`. `show sources` is valid SQL (SHOW of
//! a table named sources), so where another parser override runs before
//! this one and accepts all SQL (the DuckDB CLI loads one, the PEG parser
//! in its autocomplete extension), only `show: sources` reaches here.
static string MalloyStatementToSQL(const string &query) {
	StatementScanner scan(query);
	auto word = scan.Word();
	auto quoted = [&]() {
		return KeywordHelper::WriteQuoted(StripTrailingSemicolons(query), '\'');
	};
	if ((word == "run" || word == "source" || word == "query") && scan.Peek(':')) {
		auto function = word == "run" ? "malloy" : "malloy_define";
		return StringUtil::Format("SELECT * FROM %s(%s)", function, quoted());
	}
	// SQL has IMPORT DATABASE '...'; Malloy's import is followed by a string
	// or a brace list.
	if (word == "import" && (scan.Peek('\'') || scan.Peek('"') || scan.Peek('{'))) {
		return StringUtil::Format("SELECT * FROM malloy_define(%s)", quoted());
	}
	if (word == "show") {
		scan.Accept(':');
		auto what = scan.Word();
		if (!scan.AtEnd()) {
			return "";
		}
		if (what == "sources" || what == "queries") {
			return StringUtil::Format("SELECT name FROM malloy_sources() WHERE kind = '%s' ORDER BY name",
			                          what == "sources" ? "source" : "query");
		}
		if (what == "session") {
			return "SELECT * FROM malloy_session()";
		}
		return "";
	}
	if (word == "describe") {
		auto what = scan.Word();
		if (what != "source" && what != "query") {
			return "";
		}
		auto name = scan.Name();
		if (name.empty() || !scan.AtEnd()) {
			return "";
		}
		return StringUtil::Format("SELECT * FROM malloy_describe(%s)", KeywordHelper::WriteQuoted(name, '\''));
	}
	return "";
}

//! DuckDB offers every query here first, unsplit, when
//! allow_parser_override_extension is not 'default' (loading the extension
//! sets it to 'fallback'). A Malloy statement becomes the SQL that runs it;
//! anything else is declined and parsed as SQL.
static ParserOverrideResult MalloyParserOverride(ParserExtensionInfo *, const string &query, ParserOptions &options) {
	auto sql = MalloyStatementToSQL(query);
	if (sql.empty()) {
		return ParserOverrideResult();
	}
	try {
		ParserOptions sql_options = options;
		sql_options.extensions = decltype(sql_options.extensions)();
		Parser parser(sql_options);
		parser.ParseQuery(sql);
		return ParserOverrideResult(std::move(parser.statements));
	} catch (std::exception &ex) {
		return ParserOverrideResult(ex);
	}
}

//! With the override turned off, DuckDB only offers statements it failed to
//! parse, already split at every semicolon, including those inside Malloy's
//! braces. That cannot be parsed reliably, so explain how to turn it on.
static ParserExtensionParseResult MalloyParse(ParserExtensionInfo *, const string &query) {
	if (MalloyStatementToSQL(query).empty()) {
		return ParserExtensionParseResult();
	}
	return ParserExtensionParseResult(
	    "Malloy statements need the parser override: SET allow_parser_override_extension = 'fallback';\n"
	    "Or call the functions directly, e.g. SELECT * FROM malloy('run: ...');");
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

	auto rows_function = [](const char *name, vector<LogicalType> arguments, table_function_bind_t bind) {
		return TableFunction(name, std::move(arguments), MalloyRowsScan, bind, MalloyRowsInit);
	};

	auto malloy_sql = rows_function("malloy_sql", {LogicalType::VARCHAR}, MalloySQLBind);
	malloy_sql.named_parameters["model"] = LogicalType::VARCHAR;
	loader.RegisterFunction(malloy_sql);

	auto malloy_sources = rows_function("malloy_sources", {}, MalloySourcesBind);
	malloy_sources.named_parameters["model"] = LogicalType::VARCHAR;
	loader.RegisterFunction(malloy_sources);

	auto malloy_describe = rows_function("malloy_describe", {LogicalType::VARCHAR}, MalloyDescribeBind);
	malloy_describe.named_parameters["model"] = LogicalType::VARCHAR;
	loader.RegisterFunction(malloy_describe);

	loader.RegisterFunction(rows_function("malloy_define", {LogicalType::VARCHAR}, MalloyDefineBind));
	loader.RegisterFunction(rows_function("malloy_load", {LogicalType::VARCHAR}, MalloyLoadBind));
	loader.RegisterFunction(rows_function("malloy_session", {}, MalloySessionBind));
	loader.RegisterFunction(rows_function("malloy_version", {}, MalloyVersionBind));

	ParserExtension parser;
	parser.parse_function = MalloyParse;
	parser.plan_function = MalloyPlan;
	parser.parser_override = MalloyParserOverride;
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	ParserExtension::Register(config, parser);

	// Malloy statements typed directly need the parser override. Turn it on,
	// in its 'fallback' mode (SQL that isn't Malloy parses as usual), unless
	// the user has already chosen a setting.
	if (Settings::Get<AllowParserOverrideExtensionSetting>(config) == AllowParserOverride::DEFAULT_OVERRIDE) {
		config.SetOptionByName(AllowParserOverrideExtensionSetting::Name, Value("fallback"));
	}
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
