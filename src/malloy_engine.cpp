#include "malloy_engine.hpp"

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
		JS_UpdateStackTop(rt);
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
		JS_SetContextOpaque(ctx, previous);

		if (JS_IsException(result)) {
			throw BinderException("Malloy: %s", TakeException());
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

//! Splits a driver listing: rows on newlines, cells on tabs.
static vector<vector<string>> ParseListing(const string &listing) {
	vector<vector<string>> rows;
	if (listing.empty()) {
		return rows;
	}
	for (auto &line : StringUtil::Split(listing, '\n')) {
		rows.push_back(StringUtil::Split(line, '\t'));
	}
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

vector<vector<string>> MalloyEngine::ListContents(ClientContext &context, const MalloyModel &model) {
	auto &rt = GetRuntime();
	std::lock_guard<std::recursive_mutex> guard(rt.lock);
	return ParseListing(rt.Call(&context, "listContents", WithModelText({model.url}, model)));
}

vector<vector<string>> MalloyEngine::Describe(ClientContext &context, const MalloyModel &model, const string &name) {
	auto &rt = GetRuntime();
	std::lock_guard<std::recursive_mutex> guard(rt.lock);
	return ParseListing(rt.Call(&context, "describe", WithModelText({model.url, name}, model)));
}

vector<vector<string>> MalloyEngine::Define(ClientContext &context, const MalloyModel &model,
                                           const string &statements) {
	auto &rt = GetRuntime();
	std::lock_guard<std::recursive_mutex> guard(rt.lock);
	return ParseListing(rt.Call(&context, "define", {model.url, model.text, statements}));
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
