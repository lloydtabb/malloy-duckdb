#include "malloy_statements.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

namespace {

bool IsSpace(char c) {
	return StringUtil::CharacterIsSpace(c);
}

bool IsWordStart(char c) {
	return StringUtil::CharacterIsAlpha(c) || c == '_';
}

bool IsWordChar(char c) {
	return StringUtil::CharacterIsAlphaNumeric(c) || c == '_';
}

bool At(const string &text, idx_t pos, const char *token) {
	return text.compare(pos, strlen(token), token) == 0;
}

//! Index just past the end of the line containing `pos`.
idx_t LineEnd(const string &text, idx_t pos) {
	auto end = text.find('\n', pos);
	return end == string::npos ? text.size() : end + 1;
}

//! Index just past `close`, searching from `pos`; the end of `text` if it
//! never appears.
idx_t PastToken(const string &text, idx_t pos, const char *close) {
	auto end = text.find(close, pos);
	return end == string::npos ? text.size() : end + strlen(close);
}

//! Index just past a quoted string starting at `pos`. `doubled` quotes
//! (SQL's 'it''s') or `backslash` escapes (Malloy's 'it\'s') stay inside.
idx_t PastQuoted(const string &text, idx_t pos, bool backslash) {
	char quote = text[pos];
	for (idx_t i = pos + 1; i < text.size(); i++) {
		if (backslash && text[i] == '\\') {
			i++;
		} else if (text[i] == quote) {
			if (!backslash && i + 1 < text.size() && text[i + 1] == quote) {
				i++;
			} else {
				return i + 1;
			}
		}
	}
	return text.size();
}

//! Is `pos` at the first non-blank character of its line?
bool AtLineStart(const string &text, idx_t pos) {
	for (idx_t i = pos; i > 0; i--) {
		char c = text[i - 1];
		if (c == '\n') {
			return true;
		}
		if (c != ' ' && c != '\t' && c != '\r') {
			return false;
		}
	}
	return true;
}

//! Skips whitespace, `--`, `//` and `/* */` comments, and `#` annotation
//! lines, from `pos`.
idx_t SkipNoise(const string &text, idx_t pos) {
	while (pos < text.size()) {
		if (IsSpace(text[pos])) {
			pos++;
		} else if (At(text, pos, "--") || At(text, pos, "//") || (text[pos] == '#' && AtLineStart(text, pos))) {
			pos = LineEnd(text, pos);
		} else if (At(text, pos, "/*")) {
			pos = PastToken(text, pos + 2, "*/");
		} else {
			break;
		}
	}
	return pos;
}

//! The word at `pos` (after noise), lowercased, and the index after it.
string WordAt(const string &text, idx_t &pos) {
	pos = SkipNoise(text, pos);
	auto start = pos;
	while (pos < text.size() && IsWordChar(text[pos])) {
		pos++;
	}
	return StringUtil::Lower(text.substr(start, pos - start));
}

enum class StatementKind { SQL, MALLOY, COMMAND };

//! What a statement starting at `pos` is, by its first words.
StatementKind Classify(const string &text, idx_t pos, bool &command_takes_sql) {
	command_takes_sql = false;
	auto word = WordAt(text, pos);
	auto next = SkipNoise(text, pos);
	if ((word == "run" || word == "source" || word == "query") && next < text.size() && text[next] == ':') {
		return StatementKind::MALLOY;
	}
	if (word == "malloy" && (next >= text.size() || IsWordStart(text[next]) || text[next] == ';')) {
		auto sub = WordAt(text, pos);
		command_takes_sql = sub == "sql";
		return StatementKind::COMMAND;
	}
	return StatementKind::SQL;
}

//! Index of the `;` ending a SQL statement that starts at `pos` (or the end
//! of `text`), skipping strings, comments and embedded `%{ }%`.
idx_t SQLStatementEnd(const string &text, idx_t pos) {
	while (pos < text.size()) {
		char c = text[pos];
		if (c == ';') {
			return pos;
		} else if (c == '\'' || c == '"') {
			pos = PastQuoted(text, pos, false);
		} else if (At(text, pos, "$$")) {
			pos = PastToken(text, pos + 2, "$$");
		} else if (At(text, pos, "--")) {
			pos = LineEnd(text, pos);
		} else if (At(text, pos, "/*")) {
			pos = PastToken(text, pos + 2, "*/");
		} else if (At(text, pos, "%{")) {
			pos = PastToken(text, pos + 2, "}%");
		} else {
			pos++;
		}
	}
	return text.size();
}

//! Index of the `;` ending a Malloy statement that starts at `pos` (or the
//! end of `text`): outside braces, strings and comments.
idx_t MalloyStatementEnd(const string &text, idx_t pos) {
	idx_t depth = 0;
	while (pos < text.size()) {
		char c = text[pos];
		if (c == ';' && depth == 0) {
			return pos;
		} else if (c == '{') {
			depth++;
			pos++;
		} else if (c == '}') {
			depth = depth > 0 ? depth - 1 : 0;
			pos++;
		} else if (At(text, pos, "\"\"\"")) {
			pos = PastToken(text, pos + 3, "\"\"\"");
		} else if (c == '\'' || c == '"' || c == '`') {
			pos = PastQuoted(text, pos, true);
		} else if (At(text, pos, "--") || At(text, pos, "//") || (c == '#' && AtLineStart(text, pos))) {
			pos = LineEnd(text, pos);
		} else if (At(text, pos, "/*")) {
			pos = PastToken(text, pos + 2, "*/");
		} else {
			pos++;
		}
	}
	return text.size();
}

string Quote(const string &text) {
	return KeywordHelper::WriteQuoted(text, '\'');
}

string Trim(string text) {
	StringUtil::Trim(text);
	return text;
}

string ErrorSQL(const string &message) {
	return "SELECT * FROM malloy_error(" + Quote(message) + ")";
}

//! A command argument: a 'quoted' or "quoted" string, a `backquoted` name,
//! or a bare name. Sets `quoted` for strings.
bool ReadArgument(const string &text, idx_t &pos, string &value, bool &quoted) {
	pos = SkipNoise(text, pos);
	quoted = false;
	if (pos >= text.size()) {
		return false;
	}
	char c = text[pos];
	if (c == '\'' || c == '"') {
		auto end = PastQuoted(text, pos, false);
		auto inner = text.substr(pos + 1, end - pos - 2);
		string doubled(2, c);
		value = StringUtil::Replace(inner, doubled, string(1, c));
		quoted = true;
		pos = end;
		return true;
	}
	if (c == '`') {
		auto end = PastQuoted(text, pos, true);
		value = text.substr(pos + 1, end - pos - 2);
		pos = end;
		return true;
	}
	auto start = pos;
	while (pos < text.size() && IsWordChar(text[pos])) {
		pos++;
	}
	value = text.substr(start, pos - start);
	return !value.empty();
}

bool OnlyNoiseLeft(const string &text, idx_t pos) {
	return SkipNoise(text, pos) >= text.size();
}

//! The SQL for a `malloy <command> ...` statement.
string CommandSQL(const string &statement) {
	idx_t pos = 0;
	WordAt(statement, pos); // malloy
	auto command = WordAt(statement, pos);
	auto usage = [](const string &problem, const string &form) {
		return ErrorSQL(problem + "; usage: " + form);
	};

	// Commands without arguments
	struct Plain {
		const char *name;
		const char *function;
	};
	static const Plain plain[] = {{"", "malloy_help"},
	                              {"help", "malloy_help"},
	                              {"sources", "malloy_sources"},
	                              {"update", "malloy_update"},
	                              {"session", "malloy_session"},
	                              {"reset", "malloy_reset"}};
	for (auto &p : plain) {
		if (command == p.name) {
			if (!OnlyNoiseLeft(statement, pos)) {
				return usage("malloy " + command + " takes no arguments", "malloy " + command);
			}
			return StringUtil::Format("SELECT * FROM %s()", p.function);
		}
	}

	if (command == "sql") {
		auto rest = Trim(statement.substr(pos));
		if (rest.empty()) {
			return usage("malloy sql needs a statement", "malloy sql <statement>");
		}
		return "SELECT * FROM malloy_sql(" + Quote(rest) + ")";
	}

	string value;
	bool quoted;
	bool has_argument = ReadArgument(statement, pos, value, quoted);
	bool extra = !OnlyNoiseLeft(statement, pos);

	if (command == "describe") {
		if (!has_argument || extra) {
			return usage("malloy describe needs one source name", "malloy describe <name>");
		}
		return "SELECT * FROM malloy_describe(" + Quote(value) + ")";
	}
	if (command == "uninstall") {
		if (!has_argument || extra) {
			return usage("malloy uninstall needs a model or a source name",
			             "malloy uninstall '<model>' or malloy uninstall <name>");
		}
		return "SELECT * FROM malloy_uninstall(" + Quote(value) + ", " + (quoted ? "true" : "false") + ")";
	}

	// Commands taking a location
	struct Located {
		const char *name;
		const char *function;
		const char *what;
	};
	static const Located located[] = {{"import", "malloy_import", "'<location>'"},
	                                  {"install", "malloy_install", "'<location>'"},
	                                  {"check", "malloy_check", "'<location>'"},
	                                  {"export", "malloy_export", "'<directory>'"}};
	for (auto &l : located) {
		if (command == l.name) {
			if (!has_argument || !quoted || extra) {
				return usage("malloy " + command + " needs a quoted location", string("malloy ") + l.name + " " + l.what);
			}
			return StringUtil::Format("SELECT * FROM %s(%s)", l.function, Quote(value));
		}
	}

	return ErrorSQL("unknown command 'malloy " + command + "'; malloy lists the commands");
}

//! The SQL that runs one embedded query: `%{ q }%` is `run: q`.
string EmbeddedSQL(const string &query, bool parenthesized) {
	auto sql = "SELECT * FROM malloy(" + Quote("run: " + Trim(query)) + ")";
	return parenthesized ? sql : "(" + sql + ")";
}

//! Rewrites one statement, or returns it unchanged. Sets `changed`.
string RewriteStatement(const string &statement, StatementKind kind, bool &changed) {
	changed = true;
	if (kind == StatementKind::COMMAND) {
		return CommandSQL(statement);
	}
	if (kind == StatementKind::MALLOY) {
		idx_t pos = 0;
		auto word = WordAt(statement, pos);
		auto function = word == "run" ? "malloy" : "malloy_define";
		return StringUtil::Format("SELECT * FROM %s(%s)", function, Quote(Trim(statement)));
	}
	// SQL: a statement that is only `%{ q }%` runs q; otherwise expand each.
	auto trimmed = Trim(statement);
	auto start = SkipNoise(trimmed, 0);
	if (At(trimmed, start, "%{") && StringUtil::EndsWith(trimmed, "}%") &&
	    trimmed.find("}%", start + 2) == trimmed.size() - 2) {
		return "SELECT * FROM malloy(" + Quote("run: " + Trim(trimmed.substr(start + 2, trimmed.size() - start - 4))) + ")";
	}
	bool found = false;
	auto expanded = MalloyExpandEmbedded(statement, [&](const string &query, bool parenthesized) {
		found = true;
		return EmbeddedSQL(query, parenthesized);
	});
	changed = found;
	return expanded;
}

} // namespace

string MalloyExpandEmbedded(const string &sql,
                            const std::function<string(const string &query, bool parenthesized)> &replace) {
	string out;
	idx_t pos = 0;
	idx_t copied = 0;
	while (pos < sql.size()) {
		char c = sql[pos];
		if (c == '\'' || c == '"') {
			pos = PastQuoted(sql, pos, false);
		} else if (At(sql, pos, "--")) {
			pos = LineEnd(sql, pos);
		} else if (At(sql, pos, "/*")) {
			pos = PastToken(sql, pos + 2, "*/");
		} else if (At(sql, pos, "%{")) {
			auto close = sql.find("}%", pos + 2);
			if (close == string::npos) {
				break; // unterminated: left for DuckDB to reject
			}
			auto query = sql.substr(pos + 2, close - pos - 2);
			auto start = pos;
			auto end = close + 2;
			// (%{ q }%): replace the parentheses too, and keep one pair.
			idx_t before = start;
			while (before > copied && IsSpace(sql[before - 1])) {
				before--;
			}
			idx_t after = end;
			while (after < sql.size() && IsSpace(sql[after])) {
				after++;
			}
			bool parenthesized = before > copied && sql[before - 1] == '(' && after < sql.size() && sql[after] == ')';
			if (parenthesized) {
				start = before - 1;
				end = after + 1;
			}
			out += sql.substr(copied, start - copied);
			auto replacement = replace(query, parenthesized);
			out += parenthesized ? "(" + replacement + ")" : replacement;
			copied = pos = end;
		} else {
			pos++;
		}
	}
	out += sql.substr(copied);
	return out;
}

string MalloyRewriteScript(const string &script) {
	vector<string> statements;
	bool any_changed = false;
	idx_t pos = 0;
	while (pos < script.size()) {
		auto start = pos;
		if (OnlyNoiseLeft(script, pos)) {
			break;
		}
		bool command_takes_sql;
		auto kind = Classify(script, pos, command_takes_sql);
		auto end = kind == StatementKind::SQL || command_takes_sql ? SQLStatementEnd(script, pos)
		                                                           : MalloyStatementEnd(script, pos);
		bool changed;
		statements.push_back(RewriteStatement(script.substr(start, end - start), kind, changed));
		any_changed = any_changed || changed;
		pos = end < script.size() ? end + 1 : end;
	}
	if (!any_changed) {
		return "";
	}
	return StringUtil::Join(statements, ";\n");
}

bool MalloyIsRunStatement(const string &text) {
	bool command_takes_sql;
	idx_t pos = 0;
	if (Classify(text, pos, command_takes_sql) != StatementKind::MALLOY) {
		return false;
	}
	return WordAt(text, pos) == "run";
}

unordered_set<string> MalloyIdentifiers(const string &text) {
	unordered_set<string> names;
	idx_t pos = 0;
	while (pos < text.size()) {
		char c = text[pos];
		if (At(text, pos, "\"\"\"")) {
			pos = PastToken(text, pos + 3, "\"\"\"");
		} else if (c == '\'' || c == '"') {
			pos = PastQuoted(text, pos, true);
		} else if (c == '`') {
			auto end = PastQuoted(text, pos, true);
			names.insert(text.substr(pos + 1, end - pos - 2));
			pos = end;
		} else if (At(text, pos, "--") || At(text, pos, "//") || (c == '#' && AtLineStart(text, pos))) {
			pos = LineEnd(text, pos);
		} else if (At(text, pos, "/*")) {
			pos = PastToken(text, pos + 2, "*/");
		} else if (IsWordStart(c)) {
			auto start = pos;
			while (pos < text.size() && IsWordChar(text[pos])) {
				pos++;
			}
			names.insert(text.substr(start, pos - start));
		} else {
			pos++;
		}
	}
	return names;
}

} // namespace duckdb
