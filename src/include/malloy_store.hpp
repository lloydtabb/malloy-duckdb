#pragma once

#include "duckdb.hpp"

namespace duckdb {

//! A source or named query installed in a database.
struct InstalledSource {
	string name;
	string kind;
	string catalog;
	string model;
};

//! One file of a model being installed.
struct InstallFile {
	string path;
	string contents;
	string installed_from;
};

//! Everything `malloy install` writes for one model.
struct InstallPlan {
	string model;
	vector<InstallFile> files;
	vector<pair<string, string>> entries; // (kind, name)
};

//! Installed models live in two tables in a `malloy` schema of a database:
//!
//!   malloy.files   (model, path, contents, installed_from, installed_at)
//!   malloy.sources (name, kind, model)
//!
//! Each model's files are stored under the model's own name, so imports
//! between them resolve as they did on disk. The tables are read and written
//! through a second connection to the same database, because a table
//! function cannot run queries on the connection that is running it. That
//! connection sees committed data, and names every table with its catalog,
//! since it does not share the caller's USE.
class MalloyStore {
public:
	//! The catalog `malloy install` writes to: the caller's current database.
	static string CurrentCatalog(ClientContext &context);

	//! Every installed source visible to the caller, first match first: the
	//! current database's, then each other attached database's, in attach
	//! order. A name installed in more than one database appears once.
	static vector<InstalledSource> List(ClientContext &context);

	//! The URL a compile uses for an installed model file.
	static string FileURL(const string &catalog, const string &model, const string &path);
	static bool IsFileURL(const string &url);

	//! The contents of an installed model file, by its FileURL.
	static string ReadFile(ClientContext &context, const string &url);

	//! Replaces `plan.model` in `catalog` with `plan`, creating the tables if
	//! needed. Refuses names already installed from another model.
	static void Install(ClientContext &context, const string &catalog, const InstallPlan &plan);

	//! Installed models in `catalog`: (model, where its model file was installed from).
	static vector<pair<string, string>> Models(ClientContext &context, const string &catalog);

	//! Removes a model and its sources, matched by model name or by where it
	//! was installed from. Returns the removed sources' names.
	static vector<string> UninstallModel(ClientContext &context, const string &catalog, const string &model);

	//! Removes one source; its model's files go too if it was the last one.
	static void UninstallSource(ClientContext &context, const string &catalog, const string &name);

	//! Every installed file in `catalog`: (model, path, contents).
	static vector<vector<string>> Files(ClientContext &context, const string &catalog);
};

} // namespace duckdb
