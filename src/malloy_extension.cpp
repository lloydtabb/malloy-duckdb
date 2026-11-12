#define DUCKDB_EXTENSION_MAIN

#include "malloy_extension.hpp"
#include "malloy_engine.hpp"
#include "malloy_statements.hpp"
#include "malloy_store.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
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

#include <algorithm>
#include <functional>
#include <map>

namespace duckdb {

//===--------------------------------------------------------------------===//
// The session model
//===--------------------------------------------------------------------===//
//! Each connection has a Malloy document in memory, the session model, that
//! `malloy import`, `source:` and `query:` add to. It stands in for a file
//! in the working directory, so relative imports resolve from there.
struct MalloyClientState : public ClientContextState {
	string url;
	vector<string> statements;
	//! What the session defines or imports: name -> kind. These shadow
	//! installed sources of the same name.
	std::map<string, string> names;

	string Text() const {
		return StringUtil::Join(statements, "\n");
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

static string MalloyName(const string &name) {
	bool plain = !name.empty() && (StringUtil::CharacterIsAlpha(name[0]) || name[0] == '_');
	for (char c : name) {
		plain = plain && (StringUtil::CharacterIsAlphaNumeric(c) || c == '_');
	}
	return plain ? name : "`" + name + "`";
}

//! Imports of the installed models that `text` (with the session) uses: one
//! `import { ... } from` per model, naming every source the model installed
//! that the session does not define. All of them, not only the ones named,
//! because a named query needs the source it is built on.
static string InstalledImports(ClientContext &context, const MalloyClientState &state, const string &text) {
	auto installed = MalloyStore::List(context);
	if (installed.empty()) {
		return "";
	}
	auto names = MalloyIdentifiers(state.Text() + "\n" + text);
	auto url = [](const InstalledSource &source) {
		return MalloyStore::FileURL(source.catalog, source.model, source.model);
	};
	unordered_set<string> used; // file URLs of models the text uses
	for (auto &source : installed) {
		if (names.count(source.name) && !state.names.count(source.name)) {
			used.insert(url(source));
		}
	}
	std::map<string, vector<string>> by_model; // file URL -> names
	for (auto &source : installed) {
		if (used.count(url(source)) && !state.names.count(source.name)) {
			by_model[url(source)].push_back(MalloyName(source.name));
		}
	}
	vector<string> imports;
	for (auto &entry : by_model) {
		imports.push_back("import { " + StringUtil::Join(entry.second, ", ") + " } from " +
		                  KeywordHelper::WriteQuoted(entry.first, '"'));
	}
	return StringUtil::Join(imports, "\n");
}

//! The model a query compiles against: the installed sources it needs, then
//! the session's statements.
static MalloyModel SessionModel(ClientContext &context, const string &text) {
	auto state = GetMalloyState(context);
	MalloyModel model;
	model.url = state->url;
	model.in_memory = true;
	auto prefix = InstalledImports(context, *state, text);
	auto session = state->Text();
	model.text = prefix.empty() ? session : session.empty() ? prefix : prefix + "\n" + session;
	return model;
}

static string StringArgument(TableFunctionBindInput &input, const char *function, const char *what) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("%s: %s cannot be NULL", function, what);
	}
	return input.inputs[0].ToString();
}

//===--------------------------------------------------------------------===//
// malloy('run: ...'): a query's rows, as a subquery
//===--------------------------------------------------------------------===//
static unique_ptr<TableRef> MalloyBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto query = StringArgument(input, "malloy", "the Malloy query");
	auto sql = MalloyEngine::ToSQL(context, SessionModel(context, query), query);

	Parser parser(context.GetParserOptions());
	parser.ParseQuery(sql);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw BinderException("malloy: the generated SQL is not a single SELECT statement:\n%s", sql);
	}
	auto select = unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
	return make_uniq<SubqueryRef>(std::move(select));
}

//===--------------------------------------------------------------------===//
// Functions that return rows of strings, and may act when they run
//===--------------------------------------------------------------------===//
using MalloyAction = std::function<vector<vector<string>>(ClientContext &)>;

struct MalloyRowsData : public TableFunctionData {
	//! Rows computed at bind.
	vector<vector<string>> rows;
	//! Runs once when the call executes (not at bind, which can happen more
	//! than once), and returns more rows. Changes happen here.
	MalloyAction action;

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<MalloyRowsData>();
		copy->rows = rows;
		copy->action = action;
		return std::move(copy);
	}
	bool Equals(const FunctionData &) const override {
		return false; // actions are not comparable; never reuse a bind
	}
};

struct MalloyRowsState : public GlobalTableFunctionState {
	vector<vector<string>> rows;
	idx_t next = 0;
};

static unique_ptr<GlobalTableFunctionState> MalloyRowsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<MalloyRowsData>();
	auto state = make_uniq<MalloyRowsState>();
	state->rows = bind.rows;
	if (bind.action) {
		for (auto &row : bind.action(context)) {
			state->rows.push_back(row);
		}
	}
	return std::move(state);
}

static void MalloyRowsScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<MalloyRowsState>();
	idx_t count = 0;
	while (state.next < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = state.rows[state.next++];
		for (idx_t col = 0; col < output.ColumnCount(); col++) {
			output.SetValue(col, count, col < row.size() ? Value(row[col]) : Value());
		}
		count++;
	}
	output.SetCardinality(count);
}

static unique_ptr<MalloyRowsData> Columns(vector<LogicalType> &return_types, vector<string> &names,
                                          vector<string> columns) {
	names = std::move(columns);
	return_types.assign(names.size(), LogicalType::VARCHAR);
	return make_uniq<MalloyRowsData>();
}

#define MALLOY_BIND(NAME)                                                                                              \
	static unique_ptr<FunctionData> NAME(ClientContext &context, TableFunctionBindInput &input,                       \
	                                     vector<LogicalType> &return_types, vector<string> &names)

//===--------------------------------------------------------------------===//
// malloy (help)
//===--------------------------------------------------------------------===//
MALLOY_BIND(MalloyHelpBind) {
	auto result = Columns(return_types, names, {"command", "does"});
	result->rows = {
	    {"run: <query>", "Runs a Malloy query"},
	    {"%{ <query> }%", "A Malloy query inside SQL, anywhere a table can go"},
	    {"source: ..., query: ...", "Defines a source or named query in this session"},
	    {"malloy help [<topic>]", "Lists Malloy help topics, or shows one: malloy help language/joins"},
	    {"malloy sources", "Lists the sources and named queries you can use, with descriptions"},
	    {"malloy describe <name>", "Lists a source's fields, through its joins, and how to use each"},
	    {"malloy sql <statement>", "Shows a statement with its Malloy compiled to SQL"},
	    {"malloy import '<location>'", "Loads a model into this session only"},
	    {"malloy install '<location>'", "Copies a model into this database and installs its sources"},
	    {"malloy update", "Installs every model again, from where it was installed from"},
	    {"malloy uninstall '<model>'", "Removes an installed model and its sources"},
	    {"malloy uninstall <name>", "Removes one installed source"},
	    {"malloy export '<directory>'", "Writes the installed model files to a directory"},
	    {"malloy check '<location>'", "Compiles a model and reports errors, changing nothing"},
	    {"malloy session", "Lists what this session has imported and defined"},
	    {"malloy reset", "Clears this session's imports and definitions"},
	};
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// malloy help [<topic>]: malloyyo's yo_help topics
//===--------------------------------------------------------------------===//
MALLOY_BIND(MalloyHelpTopicsBind) {
	auto result = Columns(return_types, names, {"topic", "about"});
	result->rows = MalloyEngine::HelpTopics();
	return std::move(result);
}

MALLOY_BIND(MalloyHelpTopicBind) {
	auto result = Columns(return_types, names, {"help"});
	result->rows = MalloyEngine::HelpTopic(StringArgument(input, "malloy_help_topic", "the topic"));
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// malloy sql <statement>
//===--------------------------------------------------------------------===//
MALLOY_BIND(MalloySQLBind) {
	auto statement = StringArgument(input, "malloy_sql", "the statement");
	string sql;
	if (MalloyIsRunStatement(statement)) {
		sql = MalloyEngine::ToSQL(context, SessionModel(context, statement), statement);
	} else {
		sql = MalloyExpandEmbedded(statement, [&](const string &query, bool parenthesized) {
			auto run = "run: " + query;
			auto compiled = MalloyEngine::ToSQL(context, SessionModel(context, run), run);
			return parenthesized ? compiled : "(" + compiled + ")";
		});
	}
	auto result = Columns(return_types, names, {"sql"});
	result->rows.push_back({sql});
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// source: ..., query: ..., malloy import: add to the session model
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> DefineBind(ClientContext &context, const string &statements,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	auto state = GetMalloyState(context);
	auto prefix = InstalledImports(context, *state, statements);
	auto defined = MalloyEngine::Define(context, state->url, prefix, state->Text(), statements);
	auto result = Columns(return_types, names, {"kind", "name"});
	result->rows = defined;
	result->action = [statements, defined](ClientContext &context) {
		auto state = GetMalloyState(context);
		state->statements.push_back(statements);
		for (auto &row : defined) {
			if (row.size() >= 2) {
				state->names[row[1]] = row[0];
			}
		}
		return vector<vector<string>>();
	};
	return std::move(result);
}

MALLOY_BIND(MalloyDefineBind) {
	return DefineBind(context, StringArgument(input, "malloy_define", "the Malloy statements"), return_types, names);
}

MALLOY_BIND(MalloyImportBind) {
	auto url = MalloyModelURL(context, StringArgument(input, "malloy_import", "the location"));
	return DefineBind(context, "import " + KeywordHelper::WriteQuoted(url, '"'), return_types, names);
}

//===--------------------------------------------------------------------===//
// malloy session, malloy reset
//===--------------------------------------------------------------------===//
MALLOY_BIND(MalloySessionBind) {
	auto result = Columns(return_types, names, {"statement"});
	for (auto &statement : GetMalloyState(context)->statements) {
		result->rows.push_back({statement});
	}
	return std::move(result);
}

MALLOY_BIND(MalloyResetBind) {
	auto result = Columns(return_types, names, {"reset"});
	result->action = [](ClientContext &context) {
		auto state = GetMalloyState(context);
		auto count = state->statements.size();
		state->statements.clear();
		state->names.clear();
		return vector<vector<string>> {
		    {StringUtil::Format("cleared %llu session statement%s", count, count == 1 ? "" : "s")}};
	};
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// malloy sources, malloy describe
//===--------------------------------------------------------------------===//
MALLOY_BIND(MalloySourcesBind) {
	auto state = GetMalloyState(context);
	auto installed = MalloyStore::List(context);
	std::map<string, string> from; // name -> where it comes from
	for (auto &source : installed) {
		if (!state->names.count(source.name)) {
			from[source.name] = source.catalog + ": " + source.model;
		}
	}
	for (auto &entry : state->names) {
		from[entry.first] = "session";
	}
	auto result = Columns(return_types, names,
	                      {"name", "kind", "from", "description", "primary_key", "filters", "joins", "fields"});
	if (from.empty()) {
		return std::move(result);
	}
	// One compile of the session with every installed model imported.
	string every_name;
	for (auto &entry : from) {
		every_name += MalloyName(entry.first) + "\n";
	}
	try {
		for (auto &row : MalloyEngine::ListSources(context, SessionModel(context, every_name))) {
			if (row.size() < 7 || !from.count(row[1])) {
				continue;
			}
			result->rows.push_back({row[1], row[0], from[row[1]], row[2], row[3], row[4], row[5], row[6]});
		}
	} catch (std::exception &ex) {
		// Still list what is there, and say why there is no more.
		ErrorData error(ex);
		for (auto &entry : from) {
			auto kind = state->names.count(entry.first) ? state->names[entry.first] : string();
			for (auto &source : installed) {
				if (source.name == entry.first && kind.empty()) {
					kind = source.kind;
				}
			}
			result->rows.push_back({entry.first, kind, entry.second, "(not compiled: " + error.RawMessage() + ")"});
		}
	}
	std::sort(result->rows.begin(), result->rows.end());
	return std::move(result);
}

MALLOY_BIND(MalloyDescribeBind) {
	auto name = StringArgument(input, "malloy_describe", "the source name");
	auto state = GetMalloyState(context);
	bool known = state->names.count(name) > 0;
	if (!known) {
		for (auto &source : MalloyStore::List(context)) {
			known = known || source.name == name;
		}
	}
	if (!known) {
		throw BinderException("Malloy: no source or query named '%s'; malloy sources lists them", name);
	}
	auto result = Columns(return_types, names, {"name", "kind", "type", "grain", "description", "definition", "usage", "example"});
	result->rows = MalloyEngine::Describe(context, SessionModel(context, MalloyName(name)), name);
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// malloy install, update, uninstall, export, check
//===--------------------------------------------------------------------===//
//! Compiles the model at `url` and works out what installing it stores.
//! Its files are stored by their paths relative to the directory that holds
//! them all, so imports between them resolve as they did.
static InstallPlan PlanInstall(ClientContext &context, const string &url, vector<string> &warnings) {
	InstallPlan plan;
	vector<pair<string, string>> files; // url, contents
	for (auto &row : MalloyEngine::Collect(context, url)) {
		if (row[0] == "file" && row.size() >= 3) {
			files.emplace_back(row[1], row[2]);
		} else if (row[0] == "entry" && row.size() >= 3) {
			plan.entries.emplace_back(row[1], row[2]);
		} else if (row[0] == "warning" && row.size() >= 2) {
			warnings.push_back(row[1]);
		}
	}
	if (files.empty()) {
		throw InvalidInputException("malloy: reading %s gave no model files", url);
	}
	auto directory = [](const string &u) {
		return u.substr(0, u.rfind('/') + 1);
	};
	auto root = directory(files[0].first);
	for (auto &file : files) {
		auto dir = directory(file.first);
		idx_t common = 0;
		while (common < root.size() && common < dir.size() && root[common] == dir[common]) {
			common++;
		}
		auto slash = common == 0 ? string::npos : root.rfind('/', common - 1);
		root = slash == string::npos ? "" : root.substr(0, slash + 1);
	}
	auto scheme_end = root.find("://");
	if (scheme_end == string::npos || root.find('/', scheme_end + 3) == string::npos) {
		throw InvalidInputException(
		    "malloy: the files of %s are in different places; install needs them all under one location", url);
	}
	for (auto &file : files) {
		plan.files.push_back({file.first.substr(root.size()), file.second, file.first});
	}
	plan.model = plan.files[0].path; // the model file is read first
	if (plan.entries.empty()) {
		throw InvalidInputException("malloy: %s exports no sources or queries, so there is nothing to install", url);
	}
	return plan;
}

static vector<vector<string>> InstallRows(const InstallPlan &plan, const vector<string> &warnings) {
	vector<vector<string>> rows;
	for (auto &entry : plan.entries) {
		rows.push_back({entry.second, entry.first, plan.model});
	}
	for (auto &warning : warnings) {
		rows.push_back({warning, "warning", plan.model});
	}
	return rows;
}

MALLOY_BIND(MalloyInstallBind) {
	auto url = MalloyModelURL(context, StringArgument(input, "malloy_install", "the location"));
	vector<string> warnings;
	auto plan = PlanInstall(context, url, warnings);
	auto catalog = MalloyStore::CurrentCatalog(context);
	auto result = Columns(return_types, names, {"name", "kind", "model"});
	result->action = [catalog, plan, warnings](ClientContext &context) {
		MalloyStore::Install(context, catalog, plan);
		return InstallRows(plan, warnings);
	};
	return std::move(result);
}

MALLOY_BIND(MalloyUpdateBind) {
	auto catalog = MalloyStore::CurrentCatalog(context);
	auto models = MalloyStore::Models(context, catalog);
	if (models.empty()) {
		throw InvalidInputException("malloy: no models are installed in %s", catalog);
	}
	vector<pair<InstallPlan, vector<string>>> plans;
	for (auto &model : models) {
		vector<string> warnings;
		auto plan = PlanInstall(context, model.second, warnings);
		if (plan.model != model.first) {
			throw InvalidInputException("malloy: %s, installed as %s, now lays out its files as %s; uninstall "
			                            "it and install it again",
			                            model.second, model.first, plan.model);
		}
		plans.emplace_back(std::move(plan), std::move(warnings));
	}
	auto result = Columns(return_types, names, {"name", "kind", "model"});
	result->action = [catalog, plans](ClientContext &context) {
		vector<vector<string>> rows;
		for (auto &plan : plans) {
			MalloyStore::Install(context, catalog, plan.first);
			for (auto &row : InstallRows(plan.first, plan.second)) {
				rows.push_back(row);
			}
		}
		return rows;
	};
	return std::move(result);
}

MALLOY_BIND(MalloyUninstallBind) {
	auto target = StringArgument(input, "malloy_uninstall", "what to uninstall");
	bool is_model = input.inputs.size() > 1 && !input.inputs[1].IsNull() && input.inputs[1].GetValue<bool>();
	auto catalog = MalloyStore::CurrentCatalog(context);
	// A model may be named as installed ('imdb.malloy') or by the location
	// it was installed from.
	auto location = is_model && target.find("://") == string::npos ? MalloyModelURL(context, target) : target;
	auto result = Columns(return_types, names, {"uninstalled"});
	result->action = [catalog, target, location, is_model](ClientContext &context) {
		vector<vector<string>> rows;
		if (is_model) {
			vector<string> removed;
			try {
				removed = MalloyStore::UninstallModel(context, catalog, target);
			} catch (InvalidInputException &) {
				removed = MalloyStore::UninstallModel(context, catalog, location);
			}
			for (auto &name : removed) {
				rows.push_back({name});
			}
		} else {
			MalloyStore::UninstallSource(context, catalog, target);
			rows.push_back({target});
		}
		return rows;
	};
	return std::move(result);
}

MALLOY_BIND(MalloyExportBind) {
	auto directory = StringArgument(input, "malloy_export", "the directory");
	auto catalog = MalloyStore::CurrentCatalog(context);
	auto result = Columns(return_types, names, {"file", "model"});
	result->action = [catalog, directory](ClientContext &context) {
		auto files = MalloyStore::Files(context, catalog);
		if (files.empty()) {
			throw InvalidInputException("malloy: no models are installed in %s", catalog);
		}
		// Two models may share a path only if it holds the same file.
		std::map<string, string> contents;
		for (auto &file : files) {
			auto existing = contents.find(file[1]);
			if (existing != contents.end() && existing->second != file[2]) {
				throw InvalidInputException(
				    "malloy: installed models have different versions of %s; export cannot write both", file[1]);
			}
			contents[file[1]] = file[2];
		}
		auto &fs = FileSystem::GetFileSystem(context);
		vector<vector<string>> rows;
		for (auto &file : files) {
			auto path = fs.JoinPath(directory, file[1]);
			// Create each missing directory along the way.
			auto separator = fs.PathSeparator(path);
			for (auto pos = path.find(separator, 1); pos != string::npos; pos = path.find(separator, pos + 1)) {
				auto dir = path.substr(0, pos);
				if (!fs.DirectoryExists(dir)) {
					fs.CreateDirectory(dir);
				}
			}
			auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
			auto &text = file[2];
			handle->Write((void *)text.data(), text.size());
			handle->Sync();
			rows.push_back({path, file[0]});
		}
		return rows;
	};
	return std::move(result);
}

MALLOY_BIND(MalloyCheckBind) {
	auto url = MalloyModelURL(context, StringArgument(input, "malloy_check", "the location"));
	auto result = Columns(return_types, names, {"name", "kind"});
	for (auto &row : MalloyEngine::Collect(context, url)) {
		if (row[0] == "entry" && row.size() >= 3) {
			result->rows.push_back({row[2], row[1]});
		} else if (row[0] == "warning" && row.size() >= 2) {
			result->rows.push_back({row[1], "warning"});
		}
	}
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// malloy_error, malloy_version
//===--------------------------------------------------------------------===//
//! What a mistyped `malloy` command becomes, so its message reaches the user
//! (an error from the parser hook itself would be replaced by DuckDB's own).
MALLOY_BIND(MalloyErrorBind) {
	throw InvalidInputException("Malloy: %s", StringArgument(input, "malloy_error", "the message"));
}

MALLOY_BIND(MalloyVersionBind) {
	auto result = Columns(return_types, names, {"malloy_version"});
	result->rows.push_back({MalloyEngine::MalloyVersion()});
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Recognizing Malloy in statements
//===--------------------------------------------------------------------===//
//! DuckDB offers every script here first, unsplit, when
//! allow_parser_override_extension is not 'default' (LOAD sets it to
//! 'fallback'). A script with Malloy in it becomes SQL calling the
//! functions above; anything else is declined and parsed as usual.
static ParserOverrideResult MalloyParserOverride(ParserExtensionInfo *, const string &query, ParserOptions &options) {
	auto sql = MalloyRewriteScript(query);
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
	if (MalloyRewriteScript(query).empty()) {
		return ParserExtensionParseResult();
	}
	return ParserExtensionParseResult(
	    "Malloy statements need the parser override: SET allow_parser_override_extension = 'fallback';");
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
	loader.RegisterFunction(malloy);

	auto rows = [&](const char *name, vector<LogicalType> arguments, table_function_bind_t bind) {
		loader.RegisterFunction(TableFunction(name, std::move(arguments), MalloyRowsScan, bind, MalloyRowsInit));
	};
	auto text = LogicalType::VARCHAR;
	rows("malloy_help", {}, MalloyHelpBind);
	rows("malloy_help_topics", {}, MalloyHelpTopicsBind);
	rows("malloy_help_topic", {text}, MalloyHelpTopicBind);
	rows("malloy_sql", {text}, MalloySQLBind);
	rows("malloy_define", {text}, MalloyDefineBind);
	rows("malloy_import", {text}, MalloyImportBind);
	rows("malloy_session", {}, MalloySessionBind);
	rows("malloy_reset", {}, MalloyResetBind);
	rows("malloy_sources", {}, MalloySourcesBind);
	rows("malloy_describe", {text}, MalloyDescribeBind);
	rows("malloy_install", {text}, MalloyInstallBind);
	rows("malloy_update", {}, MalloyUpdateBind);
	rows("malloy_uninstall", {text, LogicalType::BOOLEAN}, MalloyUninstallBind);
	rows("malloy_export", {text}, MalloyExportBind);
	rows("malloy_check", {text}, MalloyCheckBind);
	rows("malloy_error", {text}, MalloyErrorBind);
	rows("malloy_version", {}, MalloyVersionBind);

	ParserExtension parser;
	parser.parse_function = MalloyParse;
	parser.plan_function = MalloyPlan;
	parser.parser_override = MalloyParserOverride;
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	ParserExtension::Register(config, parser);

	// Malloy statements need the parser override. Turn it on, in its
	// 'fallback' mode (SQL that isn't Malloy parses as usual), unless the
	// user has already chosen a setting.
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
