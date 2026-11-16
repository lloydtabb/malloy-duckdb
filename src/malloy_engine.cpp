#include "malloy_engine.hpp"
#include "malloy_store.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/binder.hpp"

#include <mutex>

extern "C" {
#include "quickjs.h"
}

// Generated at build time from js/*.js (see cmake/embed_js.cmake). Each is
// NUL-terminated; the length excludes the NUL, as JS_Eval wants.
extern const unsigned char MALLOY_EMBEDDED_JS[];
extern const size_t MALLOY_EMBEDDED_JS_LEN;
extern const unsigned char MALLOY_DRIVER_JS[];
extern const size_t MALLOY_DRIVER_JS_LEN;

namespace duckdb {

namespace {

// Malloy's parser is recursive; give it room. QuickJS measures from the stack
// top recorded by JS_UpdateStackTop, which is refreshed before every call
// because DuckDB may bind on any thread.
constexpr size_t MALLOY_JS_STACK_SIZE = 4 * 1024 * 1024;

// How deep Malloy calls may nest: describing a view defined with malloy()
// starts a call inside another.
constexpr int MAX_DEPTH = 8;

//! Calls into the runtime in progress on this thread, outermost first: a call
//! can start inside another, when binding a view defined with malloy().
thread_local int call_depth = 0;

const char *PRELUDE_JS = "globalThis.console = {log() {}, info() {}, warn() {}, error() {}, debug() {}};";

//! Per-call context handed to the host callbacks through the JSContext opaque.
struct HostCall {
	optional_ptr<ClientContext> context;
};

string JSStringArg(JSContext *ctx, JSValueConst value) {
	size_t len;
	const char *str = JS_ToCStringLen(ctx, &len, value);
	if (!str) {
		throw InternalException("malloy: expected a string from JavaScript");
	}
	string result(str, len);
	JS_FreeCString(ctx, str);
	return result;
}

JSValue ThrowToJS(JSContext *ctx, const std::exception &ex) {
	ErrorData error(ex);
	return JS_ThrowPlainError(ctx, "%s", error.RawMessage().c_str());
}

string JSONString(const string &value) {
	string out = "\"";
	for (unsigned char c : value) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				out += StringUtil::Format("\\u%04x", (int)c);
			} else {
				out += (char)c;
			}
		}
	}
	return out + "\"";
}

int HexValue(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

string PercentDecode(const string &input) {
	string out;
	for (idx_t i = 0; i < input.size(); i++) {
		if (input[i] == '%' && i + 2 < input.size() && HexValue(input[i + 1]) >= 0 && HexValue(input[i + 2]) >= 0) {
			out += (char)(HexValue(input[i + 1]) * 16 + HexValue(input[i + 2]));
			i += 2;
		} else {
			out += input[i];
		}
	}
	return out;
}

//! file:// URLs become local paths; everything else (https://, s3://, ...)
//! goes to DuckDB's file system as is, which serves it when an extension
//! such as httpfs is loaded.
string ReadURL(ClientContext &context, const string &url) {
	if (MalloyStore::IsFileURL(url)) {
		return MalloyStore::ReadFile(context, url);
	}
	string path = url;
	if (StringUtil::StartsWith(url, "file://")) {
		path = PercentDecode(url.substr(7));
#ifdef _WIN32
		// file:///C:/dir/x.malloy
		if (path.size() > 2 && path[0] == '/' && path[2] == ':') {
			path = path.substr(1);
		}
#endif
	}
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	string contents;
	char buffer[16384];
	while (true) {
		auto n = handle->Read(buffer, sizeof(buffer));
		if (n <= 0) {
			break;
		}
		contents.append(buffer, NumericCast<idx_t>(n));
	}
	return contents;
}

//! The schema of `SELECT * FROM <from>`, bound in the caller's own
//! connection and transaction, so temporary and uncommitted objects are
//! visible.
string Describe(ClientContext &context, const string &from) {
	Parser parser(context.GetParserOptions());
	parser.ParseQuery("SELECT * FROM " + from);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw BinderException("malloy: could not read the schema of %s", from);
	}
	auto binder = Binder::CreateBinder(context);
	auto bound = binder->Bind(*parser.statements[0]);
	string json = "[";
	for (idx_t i = 0; i < bound.names.size(); i++) {
		if (i > 0) {
			json += ",";
		}
		json += "[" + JSONString(bound.names[i]) + "," + JSONString(bound.types[i].ToString()) + "]";
	}
	return json + "]";
}

JSValue HostReadURL(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	auto call = static_cast<HostCall *>(JS_GetContextOpaque(ctx));
	try {
		if (!call || !call->context || argc < 1) {
			throw InternalException("malloy: readURL called outside a compile");
		}
		auto contents = ReadURL(*call->context, JSStringArg(ctx, argv[0]));
		return JS_NewStringLen(ctx, contents.c_str(), contents.size());
	} catch (std::exception &ex) {
		return ThrowToJS(ctx, ex);
	}
}

JSValue HostDescribe(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	auto call = static_cast<HostCall *>(JS_GetContextOpaque(ctx));
	try {
		if (!call || !call->context || argc < 1) {
			throw InternalException("malloy: describe called outside a compile");
		}
		auto json = Describe(*call->context, JSStringArg(ctx, argv[0]));
		return JS_NewStringLen(ctx, json.c_str(), json.size());
	} catch (std::exception &ex) {
		return ThrowToJS(ctx, ex);
	}
}

//! The cache keys install wrote for an installed model, as JSON:
//! {"files": {"<url>": "<md5>", ...}, "tables": ["<from>", ...]}.
JSValue HostModelKeys(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	auto call = static_cast<HostCall *>(JS_GetContextOpaque(ctx));
	try {
		if (!call || !call->context || argc < 1) {
			throw InternalException("malloy: modelKeys called outside a compile");
		}
		auto keys = MalloyStore::Keys(*call->context, JSStringArg(ctx, argv[0]));
		string json = "{\"files\":{";
		for (idx_t i = 0; i < keys.files.size(); i++) {
			json += (i ? "," : "") + JSONString(keys.files[i].first) + ":" + JSONString(keys.files[i].second);
		}
		json += "}}";
		return JS_NewStringLen(ctx, json.c_str(), json.size());
	} catch (std::exception &ex) {
		return ThrowToJS(ctx, ex);
	}
}

class Runtime {
public:
	Runtime() {
		rt = JS_NewRuntime();
		if (!rt) {
			throw InternalException("malloy: could not create a JavaScript runtime");
		}
		JS_SetMaxStackSize(rt, MALLOY_JS_STACK_SIZE);
		ctx = JS_NewContext(rt);
		if (!ctx) {
			throw InternalException("malloy: could not create a JavaScript context");
		}
		Eval(PRELUDE_JS, strlen(PRELUDE_JS), "prelude.js");

		JSValue global = JS_GetGlobalObject(ctx);
		JSValue host = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, host, "readURL", JS_NewCFunction(ctx, HostReadURL, "readURL", 1));
		JS_SetPropertyStr(ctx, host, "describe", JS_NewCFunction(ctx, HostDescribe, "describe", 1));
		JS_SetPropertyStr(ctx, host, "modelKeys", JS_NewCFunction(ctx, HostModelKeys, "modelKeys", 1));
		JS_SetPropertyStr(ctx, global, "__malloyHost", host);
		JS_FreeValue(ctx, global);

		Eval(reinterpret_cast<const char *>(MALLOY_EMBEDDED_JS), MALLOY_EMBEDDED_JS_LEN, "malloy-embedded.js");
		Eval(reinterpret_cast<const char *>(MALLOY_DRIVER_JS), MALLOY_DRIVER_JS_LEN, "driver.js");
	}

	// Never destroyed: the runtime lives as long as the process, and tearing
	// down QuickJS during static destruction buys nothing.

	//! Calls malloyDuckDB[method](args...) and returns its string result.
	//! `context` is what the host callbacks act on; null for calls that need none.
	string Call(optional_ptr<ClientContext> context, const char *method, const vector<string> &args) {
		// A call can start inside another: describing a view defined with
		// malloy() runs Malloy again. A view whose Malloy reads itself would
		// recurse until the stack overflowed.
		if (call_depth >= MAX_DEPTH) {
			throw BinderException("Malloy: statements nest more than %d deep; does a view defined with malloy() "
			                      "read itself?",
			                      MAX_DEPTH);
		}
		// QuickJS measures its stack limit from here, so only the outermost
		// call sets it; a nested call runs deeper on the same stack.
		if (call_depth == 0) {
			JS_UpdateStackTop(rt);
		}
		call_depth++;
		// When the call ends, however it ends, the driver gives the caller's
		// state back (malloyDuckDBEnd) and the depth drops.
		struct Leave {
			Runtime &runtime;
			~Leave() {
				runtime.EndCall(call_depth);
				call_depth--;
			}
		} leave {*this};
		HostCall call {context};
		auto previous = JS_GetContextOpaque(ctx);
		JS_SetContextOpaque(ctx, &call);

		JSValue global = JS_GetGlobalObject(ctx);
		JSValue api = JS_GetPropertyStr(ctx, global, "malloyDuckDB");
		JSValue fn = JS_GetPropertyStr(ctx, api, method);
		vector<JSValue> js_args;
		for (auto &arg : args) {
			js_args.push_back(JS_NewStringLen(ctx, arg.c_str(), arg.size()));
		}
		JSValue result = JS_Call(ctx, fn, api, (int)js_args.size(), js_args.data());
		for (auto &arg : js_args) {
			JS_FreeValue(ctx, arg);
		}
		JS_FreeValue(ctx, fn);
		JS_FreeValue(ctx, api);
		JS_FreeValue(ctx, global);

		// Malloy's Runtime API is asynchronous, but the host callbacks are
		// synchronous, so the promise settles once the job queue is drained.
		// The host callbacks still need the context while jobs run.
		if (!JS_IsException(result) && JS_IsPromise(result)) {
			result = Settle(result);
		}
		JS_SetContextOpaque(ctx, previous);

		if (JS_IsException(result)) {
			// A nested call's error arrives already marked as Malloy's.
			auto message = TakeException();
			throw BinderException(StringUtil::StartsWith(message, "Malloy: ") ? message : "Malloy: " + message);
		}
		string out;
		try {
			out = JSStringArg(ctx, result);
		} catch (...) {
			JS_FreeValue(ctx, result);
			throw;
		}
		JS_FreeValue(ctx, result);
		return out;
	}

	std::recursive_mutex lock;

private:
	//! Calls malloyDuckDBEnd(depth). Never throws: it runs as a call unwinds.
	void EndCall(int depth) {
		JSValue global = JS_GetGlobalObject(ctx);
		JSValue fn = JS_GetPropertyStr(ctx, global, "malloyDuckDBEnd");
		JSValue arg = JS_NewInt32(ctx, depth);
		JSValue result = JS_Call(ctx, fn, global, 1, &arg);
		if (JS_IsException(result)) {
			JS_FreeValue(ctx, JS_GetException(ctx));
		}
		JS_FreeValue(ctx, result);
		JS_FreeValue(ctx, fn);
		JS_FreeValue(ctx, global);
	}

	//! Runs pending jobs until `promise` settles; returns its value, or
	//! JS_EXCEPTION with the rejection pending. Takes ownership of `promise`.
	JSValue Settle(JSValue promise) {
		while (JS_PromiseState(ctx, promise) == JS_PROMISE_PENDING) {
			JSContext *job_ctx;
			int ran = JS_ExecutePendingJob(rt, &job_ctx);
			if (ran < 0) {
				// A job threw outside any promise; it may not be this call's.
				// Drop it and go on: this call's own failure rejects its promise.
				JS_FreeValue(job_ctx, JS_GetException(job_ctx));
				continue;
			}
			if (ran == 0 && JS_PromiseState(ctx, promise) == JS_PROMISE_PENDING) {
				JS_FreeValue(ctx, promise);
				return JS_ThrowInternalError(ctx, "the Malloy call did not finish: it is waiting on asynchronous work the host never runs (a timer or real IO)");
			}
		}
		JSValue value = JS_PromiseResult(ctx, promise);
		bool rejected = JS_PromiseState(ctx, promise) == JS_PROMISE_REJECTED;
		JS_FreeValue(ctx, promise);
		if (rejected) {
			return JS_Throw(ctx, value);
		}
		return value;
	}

	void Eval(const char *source, size_t len, const char *filename) {
		JSValue result = JS_Eval(ctx, source, len, filename, JS_EVAL_TYPE_GLOBAL);
		if (JS_IsException(result)) {
			throw InternalException("malloy: loading %s failed: %s", filename, TakeException());
		}
		JS_FreeValue(ctx, result);
	}

	//! The pending exception's message, without the "Error: " prefix.
	string TakeException() {
		JSValue exception = JS_GetException(ctx);
		string message;
		JSValue msg = JS_GetPropertyStr(ctx, exception, "message");
		if (JS_IsString(msg)) {
			message = JSStringArg(ctx, msg);
		} else {
			const char *str = JS_ToCString(ctx, exception);
			message = str ? str : "unknown JavaScript error";
			if (str) {
				JS_FreeCString(ctx, str);
			}
		}
		JS_FreeValue(ctx, msg);
		JS_FreeValue(ctx, exception);
		return message;
	}

	JSRuntime *rt = nullptr;
	JSContext *ctx = nullptr;
};

std::mutex creation_lock;
Runtime *runtime = nullptr;

Runtime &GetRuntime() {
	std::lock_guard<std::mutex> guard(creation_lock);
	if (!runtime) {
		runtime = new Runtime();
	}
	return *runtime;
}

} // namespace

//! Splits a driver listing: rows on newlines, cells on tabs, with
//! backslash escapes inside cells (see `cell` in driver.js).
static vector<vector<string>> ParseListing(const string &listing) {
	vector<vector<string>> rows;
	if (listing.empty()) {
		return rows;
	}
	vector<string> row;
	string cell;
	for (idx_t i = 0; i < listing.size(); i++) {
		char c = listing[i];
		if (c == '\\' && i + 1 < listing.size()) {
			char e = listing[++i];
			cell += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
		} else if (c == '\t') {
			row.push_back(std::move(cell));
			cell.clear();
		} else if (c == '\n') {
			row.push_back(std::move(cell));
			cell.clear();
			rows.push_back(std::move(row));
			row.clear();
		} else {
			cell += c;
		}
	}
	row.push_back(std::move(cell));
	rows.push_back(std::move(row));
	return rows;
}

//! Driver arguments: the fixed ones, then the model's text if it is in memory
//! (the driver reads the file at the model's URL when that is omitted).
static vector<string> WithModelText(vector<string> args, const MalloyModel &model) {
	if (model.in_memory) {
		args.push_back(model.text);
	}
	return args;
}

string MalloyEngine::ToSQL(ClientContext &context, const MalloyModel &model, const string &query) {
	auto &rt = GetRuntime();
	std::lock_guard<std::recursive_mutex> guard(rt.lock);
	return rt.Call(&context, "toSQL", WithModelText({model.url, query}, model));
}

vector<vector<string>> MalloyEngine::ListSources(ClientContext &context, const MalloyModel &model) {
	auto &rt = GetRuntime();
	std::lock_guard<std::recursive_mutex> guard(rt.lock);
	return ParseListing(rt.Call(&context, "listSources", WithModelText({model.url}, model)));
}

vector<vector<string>> MalloyEngine::Describe(ClientContext &context, const MalloyModel &model, const string &name) {
	auto &rt = GetRuntime();
	std::lock_guard<std::recursive_mutex> guard(rt.lock);
	return ParseListing(rt.Call(&context, "describe", WithModelText({model.url, name}, model)));
}

vector<vector<string>> MalloyEngine::Define(ClientContext &context, const string &url, const string &prefix,
                                           const string &session, const string &statements) {
	auto &rt = GetRuntime();
	std::lock_guard<std::recursive_mutex> guard(rt.lock);
	return ParseListing(rt.Call(&context, "define", {url, prefix, session, statements}));
}

vector<vector<string>> MalloyEngine::Collect(ClientContext &context, const string &url) {
	auto &rt = GetRuntime();
	std::lock_guard<std::recursive_mutex> guard(rt.lock);
	return ParseListing(rt.Call(&context, "collect", {url}));
}

vector<vector<string>> MalloyEngine::DescribeText(ClientContext &context, const MalloyModel &model,
                                                 const string &name) {
	auto &rt = GetRuntime();
	std::lock_guard<std::recursive_mutex> guard(rt.lock);
	return ParseListing(rt.Call(&context, "describeText", WithModelText({model.url, name}, model)));
}

string MalloyEngine::Compile(ClientContext &context, const MalloyModel &model, const string &query) {
	auto &rt = GetRuntime();
	std::lock_guard<std::recursive_mutex> guard(rt.lock);
	return rt.Call(&context, "compile", WithModelText({model.url, query}, model));
}

vector<vector<string>> MalloyEngine::HelpTopics() {
	auto &rt = GetRuntime();
	std::lock_guard<std::recursive_mutex> guard(rt.lock);
	return ParseListing(rt.Call(optional_ptr<ClientContext>(), "helpTopics", {}));
}

vector<vector<string>> MalloyEngine::HelpTopic(const string &query) {
	auto &rt = GetRuntime();
	std::lock_guard<std::recursive_mutex> guard(rt.lock);
	return ParseListing(rt.Call(optional_ptr<ClientContext>(), "helpTopic", {query}));
}

bool MalloyEngine::Nested() {
	return call_depth > 0;
}

string MalloyEngine::MalloyVersion() {
	auto &rt = GetRuntime();
	std::lock_guard<std::recursive_mutex> guard(rt.lock);
	return rt.Call(optional_ptr<ClientContext>(), "getVersion", {});
}

string MalloyModelURL(ClientContext &context, const string &location) {
	if (location.find("://") != string::npos) {
		return location;
	}
	auto &fs = FileSystem::GetFileSystem(context);
	string path = fs.ExpandPath(location);
	if (!fs.IsPathAbsolute(path)) {
		path = fs.JoinPath(FileSystem::GetWorkingDirectory(), path);
	}
	path = fs.ConvertSeparators(path);
#ifdef _WIN32
	StringUtil::ReplaceAll(path, "\\", "/");
	return "file:///" + path;
#else
	return "file://" + path;
#endif
}

} // namespace duckdb
