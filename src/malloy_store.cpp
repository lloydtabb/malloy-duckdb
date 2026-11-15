#include "malloy_store.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

namespace {

constexpr const char *FILE_SCHEME = "malloy-installed://";

string Ident(const string &name) {
	return KeywordHelper::WriteOptionallyQuoted(name);
}

string Table(const string &catalog, const char *table) {
	return Ident(catalog) + ".malloy." + table;
}

void Check(QueryResult &result) {
	if (result.HasError()) {
		result.ThrowError("malloy: ");
	}
}

unique_ptr<MaterializedQueryResult> Run(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	Check(*result);
	return result;
}

template <typename... ARGS>
unique_ptr<QueryResult> Execute(Connection &con, const string &sql, ARGS... args) {
	auto prepared = con.Prepare(sql);
	if (prepared->HasError()) {
		throw IOException("malloy: %s", prepared->GetError());
	}
	vector<Value> values {args...};
	// Materialized, not streamed: callers read the rows after this returns.
	auto result = prepared->Execute(values, false);
	Check(*result);
	return result;
}

vector<vector<string>> Rows(QueryResult &result) {
	vector<vector<string>> rows;
	auto &materialized = result.Cast<MaterializedQueryResult>();
	for (idx_t row = 0; row < materialized.RowCount(); row++) {
		vector<string> cells;
		for (idx_t col = 0; col < materialized.ColumnCount(); col++) {
			auto value = materialized.GetValue(col, row);
			cells.push_back(value.IsNull() ? string() : value.ToString());
		}
		rows.push_back(std::move(cells));
	}
	return rows;
}

//! Catalogs holding a malloy.sources table, the current database first,
//! then the others in attach order.
vector<string> StoreCatalogs(ClientContext &context, Connection &con) {
	auto current = MalloyStore::CurrentCatalog(context);
	auto result = Execute(con,
	                      "SELECT database_name FROM duckdb_tables() WHERE schema_name = 'malloy' AND "
	                      "table_name = 'sources' ORDER BY database_name <> ?, database_oid",
	                      Value(current));
	vector<string> catalogs;
	for (auto &row : Rows(*result)) {
		catalogs.push_back(row[0]);
	}
	return catalogs;
}

bool IsUnreserved(char c) {
	return StringUtil::CharacterIsAlphaNumeric(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

string Encode(const string &text) {
	string out;
	for (unsigned char c : text) {
		if (IsUnreserved(c)) {
			out += (char)c;
		} else {
			out += StringUtil::Format("%%%02X", (int)c);
		}
	}
	return out;
}

string Decode(const string &text) {
	string out;
	for (idx_t i = 0; i < text.size(); i++) {
		if (text[i] == '%' && i + 2 < text.size() && StringUtil::CharacterIsHex(text[i + 1]) &&
		    StringUtil::CharacterIsHex(text[i + 2])) {
			out += (char)std::stoi(text.substr(i + 1, 2), nullptr, 16);
			i += 2;
		} else {
			out += text[i];
		}
	}
	return out;
}

void CreateTables(Connection &con, const string &catalog) {
	Run(con, "CREATE SCHEMA IF NOT EXISTS " + Ident(catalog) + ".malloy");
	Run(con, "CREATE TABLE IF NOT EXISTS " + Table(catalog, "files") +
	             " (model VARCHAR, path VARCHAR, contents VARCHAR, installed_from VARCHAR, installed_at TIMESTAMP,"
	             " digest VARCHAR)");
	Run(con, "CREATE TABLE IF NOT EXISTS " + Table(catalog, "sources") + " (name VARCHAR, kind VARCHAR, model VARCHAR)");
	Run(con, "CREATE TABLE IF NOT EXISTS " + Table(catalog, "tables") + " (model VARCHAR, sql_from VARCHAR)");
}

//! (catalog, model, path) of a FileURL.
struct FileAddress {
	string catalog;
	string model;
	string path;
};

bool HasTables(Connection &con, const string &catalog) {
	auto result = Execute(con,
	                      "SELECT count(*) FROM duckdb_tables() WHERE database_name = ? AND schema_name = 'malloy' "
	                      "AND table_name IN ('files', 'sources')",
	                      Value(catalog));
	return Rows(*result)[0][0] == "2";
}

void DeleteModelFilesIfUnused(Connection &con, const string &catalog, const string &model) {
	auto unused = " WHERE model = ? AND NOT EXISTS (SELECT 1 FROM " + Table(catalog, "sources") + " WHERE model = ?)";
	Execute(con, "DELETE FROM " + Table(catalog, "tables") + unused, Value(model), Value(model));
	Execute(con, "DELETE FROM " + Table(catalog, "files") + unused, Value(model), Value(model));
}

} // namespace

string MalloyStore::CurrentCatalog(ClientContext &context) {
	return DatabaseManager::GetDefaultDatabase(context);
}

vector<InstalledSource> MalloyStore::List(ClientContext &context) {
	Connection con(*context.db);
	vector<InstalledSource> sources;
	unordered_set<string> seen;
	for (auto &catalog : StoreCatalogs(context, con)) {
		auto result = Run(con, "SELECT name, kind, model FROM " + Table(catalog, "sources") + " ORDER BY name");
		for (auto &row : Rows(*result)) {
			if (seen.insert(row[0]).second) {
				sources.push_back({row[0], row[1], catalog, row[2]});
			}
		}
	}
	return sources;
}

string MalloyStore::FileURL(const string &catalog, const string &model, const string &path) {
	return FILE_SCHEME + Encode(catalog) + "/" + Encode(model) + "/" + path;
}

bool MalloyStore::IsFileURL(const string &url) {
	return StringUtil::StartsWith(url, FILE_SCHEME);
}

static FileAddress Address(const string &url) {
	auto rest = url.substr(strlen(FILE_SCHEME));
	auto first = rest.find('/');
	auto second = first == string::npos ? string::npos : rest.find('/', first + 1);
	if (second == string::npos) {
		throw IOException("malloy: not an installed model file: %s", url);
	}
	return {Decode(rest.substr(0, first)), Decode(rest.substr(first + 1, second - first - 1)), rest.substr(second + 1)};
}

InstalledKeys MalloyStore::Keys(ClientContext &context, const string &url) {
	auto address = Address(url);
	InstalledKeys keys;
	Connection con(*context.db);
	// One query: this runs on every compile against an installed model.
	auto result = Execute(con,
	                      "SELECT 'file', path, digest FROM " + Table(address.catalog, "files") +
	                          " WHERE model = ? UNION ALL SELECT 'table', sql_from, NULL FROM " +
	                          Table(address.catalog, "tables") + " WHERE model = ?",
	                      Value(address.model), Value(address.model));
	for (auto &row : Rows(*result)) {
		if (row[0] == "file") {
			keys.files.emplace_back(FileURL(address.catalog, address.model, row[1]), row[2]);
		} else {
			keys.tables.push_back(row[1]);
		}
	}
	return keys;
}

string MalloyStore::ReadFile(ClientContext &context, const string &url) {
	auto address = Address(url);
	auto &catalog = address.catalog;
	auto &model = address.model;
	auto &path = address.path;

	Connection con(*context.db);
	if (!HasTables(con, catalog)) {
		throw IOException("malloy: no models are installed in %s", catalog);
	}
	auto result = Execute(con, "SELECT contents FROM " + Table(catalog, "files") + " WHERE model = ? AND path = ?",
	                      Value(model), Value(path));
	auto rows = Rows(*result);
	if (rows.empty()) {
		throw IOException("malloy: %s is not installed in %s (model %s)", path, catalog, model);
	}
	return rows[0][0];
}

void MalloyStore::Install(ClientContext &context, const string &catalog, const InstallPlan &plan) {
	Connection con(*context.db);
	CreateTables(con, catalog);
	Run(con, "BEGIN TRANSACTION");
	try {
		for (auto &entry : plan.entries) {
			auto existing = Execute(con, "SELECT model FROM " + Table(catalog, "sources") + " WHERE name = ? AND model <> ?",
			                        Value(entry.second), Value(plan.model));
			auto rows = Rows(*existing);
			if (!rows.empty()) {
				throw InvalidInputException("malloy: '%s' is already installed from %s; uninstall it first",
				                            entry.second, rows[0][0]);
			}
		}
		Execute(con, "DELETE FROM " + Table(catalog, "sources") + " WHERE model = ?", Value(plan.model));
		Execute(con, "DELETE FROM " + Table(catalog, "files") + " WHERE model = ?", Value(plan.model));
		Execute(con, "DELETE FROM " + Table(catalog, "tables") + " WHERE model = ?", Value(plan.model));
		for (auto &file : plan.files) {
			Execute(con,
			        "INSERT INTO " + Table(catalog, "files") +
			            " (model, path, contents, installed_from, installed_at, digest)"
			            " VALUES (?, ?, ?, ?, now()::TIMESTAMP, md5(?))",
			        Value(plan.model), Value(file.path), Value(file.contents), Value(file.installed_from),
			        Value(file.contents));
		}
		for (auto &table : plan.tables) {
			Execute(con, "INSERT INTO " + Table(catalog, "tables") + " VALUES (?, ?)", Value(plan.model),
			        Value(table));
		}
		for (auto &entry : plan.entries) {
			Execute(con, "INSERT INTO " + Table(catalog, "sources") + " VALUES (?, ?, ?)", Value(entry.second),
			        Value(entry.first), Value(plan.model));
		}
		Run(con, "COMMIT");
	} catch (...) {
		con.Query("ROLLBACK");
		throw;
	}
}

vector<pair<string, string>> MalloyStore::Models(ClientContext &context, const string &catalog) {
	Connection con(*context.db);
	vector<pair<string, string>> models;
	if (!HasTables(con, catalog)) {
		return models;
	}
	auto result = Run(con, "SELECT model, installed_from FROM " + Table(catalog, "files") +
	                           " WHERE path = model ORDER BY model");
	for (auto &row : Rows(*result)) {
		models.emplace_back(row[0], row[1]);
	}
	return models;
}

vector<string> MalloyStore::UninstallModel(ClientContext &context, const string &catalog, const string &model) {
	Connection con(*context.db);
	if (!HasTables(con, catalog)) {
		throw InvalidInputException("malloy: no models are installed in %s", catalog);
	}
	auto match = Execute(con,
	                     "SELECT DISTINCT model FROM " + Table(catalog, "files") +
	                         " WHERE (model = ? OR installed_from = ?) AND path = model",
	                     Value(model), Value(model));
	auto rows = Rows(*match);
	if (rows.empty()) {
		throw InvalidInputException("malloy: no installed model %s; malloy sources lists what is installed", model);
	}
	auto name = rows[0][0];
	auto sources = Execute(con, "SELECT name FROM " + Table(catalog, "sources") + " WHERE model = ? ORDER BY name",
	                       Value(name));
	vector<string> removed;
	for (auto &row : Rows(*sources)) {
		removed.push_back(row[0]);
	}
	Run(con, "BEGIN TRANSACTION");
	try {
		Execute(con, "DELETE FROM " + Table(catalog, "sources") + " WHERE model = ?", Value(name));
		Execute(con, "DELETE FROM " + Table(catalog, "files") + " WHERE model = ?", Value(name));
		Execute(con, "DELETE FROM " + Table(catalog, "tables") + " WHERE model = ?", Value(name));
		Run(con, "COMMIT");
	} catch (...) {
		con.Query("ROLLBACK");
		throw;
	}
	return removed;
}

void MalloyStore::UninstallSource(ClientContext &context, const string &catalog, const string &name) {
	Connection con(*context.db);
	if (!HasTables(con, catalog)) {
		throw InvalidInputException("malloy: no models are installed in %s", catalog);
	}
	auto match =
	    Execute(con, "SELECT model FROM " + Table(catalog, "sources") + " WHERE name = ?", Value(name));
	auto rows = Rows(*match);
	if (rows.empty()) {
		throw InvalidInputException("malloy: '%s' is not installed in %s", name, catalog);
	}
	Run(con, "BEGIN TRANSACTION");
	try {
		Execute(con, "DELETE FROM " + Table(catalog, "sources") + " WHERE name = ?", Value(name));
		DeleteModelFilesIfUnused(con, catalog, rows[0][0]);
		Run(con, "COMMIT");
	} catch (...) {
		con.Query("ROLLBACK");
		throw;
	}
}

vector<vector<string>> MalloyStore::Files(ClientContext &context, const string &catalog) {
	Connection con(*context.db);
	if (!HasTables(con, catalog)) {
		return {};
	}
	auto result =
	    Run(con, "SELECT model, path, contents FROM " + Table(catalog, "files") + " ORDER BY model, path");
	return Rows(*result);
}

} // namespace duckdb
