// Runs inside QuickJS, after malloy-embedded.js. It drives the Malloy
// compiler's needs loop, asking the C++ side (globalThis.__malloyHost) for
// whatever the compiler needs:
//
//   __malloyHost.readURL(url)      -> the file's text
//   __malloyHost.describe(sqlFrom) -> JSON: [[column_name, duckdb_type], ...]
//                                     for `SELECT * FROM <sqlFrom>`
//
// Both throw on failure.
//
// Every entry point on globalThis.malloyDuckDB compiles against a model,
// named by `modelURL`. If `modelText` is a string, it is that model's source
// (the connection's session model, which lives in memory, not on disk);
// otherwise the model is read from `modelURL`. Imports resolve relative to
// `modelURL` either way.
//
// Entry points return strings, and throw Error on a Malloy error, with the
// messages formatted for a DuckDB error. Listings are one row per line,
// fields separated by tabs.
(function () {
  'use strict';
  const host = globalThis.__malloyHost;
  const malloy = globalThis.malloyEmbedded;
  const CONNECTION = 'duckdb';

  // DuckDB column types become Malloy types exactly as Malloy's own DuckDB
  // connection converts them, nested STRUCT and LIST types included.
  function schemaFor(sqlFrom) {
    return JSON.parse(malloy.duckdbSchema(host.describe(sqlFrom)));
  }

  // How a compile error says where it is. `session` describes the session
  // model, so errors in it can point at the statement being added.
  function where(log, session) {
    const url = log.url || '';
    const line = log.range ? log.range.start.line + 1 : 0;
    const col = log.range ? log.range.start.character + 1 : 0;
    if (url.startsWith('internal://query/')) {
      return line ? `query line ${line}, column ${col}` : 'query';
    }
    if (session && url === session.url) {
      if (line > session.newFromLine) {
        return `line ${line - session.newFromLine}, column ${col}`;
      }
      return line ? `session model line ${line}` : 'session model';
    }
    return line ? `${url}:${line}:${col}` : url;
  }

  function fail(logs, session) {
    const errors = logs.filter(l => l.severity === 'error');
    throw new Error(errors.map(e => `${e.message} (${where(e, session)})`).join('\n'));
  }

  // Runs one compile to completion. `compile` is a malloyEmbedded function.
  function drive(compile, request, modelText, session) {
    const needs = {
      connections: [{name: CONNECTION, dialect: 'duckdb'}],
      files: [],
      table_schemas: [],
      sql_schemas: [],
    };
    for (let round = 0; round < 100; round++) {
      const response = JSON.parse(compile(JSON.stringify({...request, compiler_needs: needs})));
      const logs = response.logs || [];
      if (logs.some(l => l.severity === 'error')) fail(logs, session);
      const asked = response.compiler_needs;
      if (!asked) return response;
      let progress = false;
      for (const file of asked.files || []) {
        const inMemory = typeof modelText === 'string' && file.url === request.model_url;
        needs.files.push({url: file.url, contents: inMemory ? modelText : host.readURL(file.url)});
        progress = true;
      }
      for (const table of asked.table_schemas || []) {
        needs.table_schemas.push({...table, schema: schemaFor(table.name)});
        progress = true;
      }
      for (const sql of asked.sql_schemas || []) {
        needs.sql_schemas.push({...sql, schema: schemaFor(`(${sql.sql})`)});
        progress = true;
      }
      for (const c of asked.connections || []) {
        if (!needs.connections.some(k => k.name === c.name)) {
          throw new Error(`Unknown connection '${c.name}'; inside DuckDB the only connection is '${CONNECTION}'`);
        }
      }
      if (!progress) {
        throw new Error('Malloy compiler asked for something malloy-duckdb cannot supply: ' + JSON.stringify(asked));
      }
    }
    throw new Error('Malloy compile did not finish');
  }

  // Everything the model can see: sources and named queries, imports included.
  function contents(modelURL, modelText, session) {
    const response = drive(malloy.compileModelContents, {model_url: modelURL}, modelText, session);
    return (response.model && response.model.entries) || [];
  }

  function typeName(type) {
    switch (type.kind) {
      case 'string_type':
        return 'string';
      case 'number_type':
        return 'number';
      case 'boolean_type':
        return 'boolean';
      case 'date_type':
        return 'date';
      case 'timestamp_type':
        return 'timestamp';
      case 'timestamptz_type':
        return 'timestamptz';
      case 'json_type':
        return 'json';
      case 'sql_native_type':
        return type.sql_type ? `sql native (${type.sql_type})` : 'sql native';
      case 'array_type':
        return typeName(type.element_type) + '[]';
      case 'record_type':
        return '{' + type.fields.map(f => `${f.name} :: ${typeName(f.type)}`).join(', ') + '}';
      default:
        return type.kind.replace(/_type$/, '').replace(/_/g, ' ');
    }
  }

  function fieldType(field) {
    switch (field.kind) {
      case 'join':
        return `join_${field.relationship}`;
      case 'view':
        return 'view';
      default:
        return field.type ? typeName(field.type) : '';
    }
  }

  // Tabs and newlines would break the line-and-tab listing format.
  function cell(text) {
    return String(text).replace(/[\t\r\n]+/g, ' ');
  }

  function listing(rows) {
    return rows.map(row => row.map(cell).join('\t')).join('\n');
  }

  globalThis.malloyDuckDB = {
    getVersion() {
      return malloy.version;
    },

    // Malloy query text -> DuckDB SQL.
    toSQL(modelURL, queryText, modelText) {
      const response = drive(malloy.compileQuery, {model_url: modelURL, query_malloy: queryText}, modelText);
      if (!response.result || typeof response.result.sql !== 'string') {
        throw new Error('Malloy produced no SQL for this query');
      }
      return response.result.sql;
    },

    // Rows of kind, name: every source and named query the model can see.
    listContents(modelURL, modelText) {
      return listing(contents(modelURL, modelText).map(e => [e.kind, e.name]));
    },

    // Rows of name, kind, type: the fields of one source or named query.
    describe(modelURL, name, modelText) {
      const entries = contents(modelURL, modelText);
      const entry = entries.find(e => e.name === name);
      if (!entry) {
        const known = entries.map(e => e.name).sort().join(', ');
        throw new Error(`No source or query named '${name}'` + (known ? `; defined: ${known}` : '; nothing is defined yet'));
      }
      return listing(entry.schema.fields.map(f => [f.name, f.kind, fieldType(f)]));
    },

    // Checks that `newText` (import, source:, query: statements) compiles
    // when appended to the session model `modelText`, and returns rows of
    // kind, name for what it makes newly visible. Does not change anything:
    // the caller appends `newText` to its session model.
    define(modelURL, modelText, newText) {
      const before = new Set(contents(modelURL, modelText).map(e => `${e.kind}\t${e.name}`));
      const combined = modelText ? `${modelText}\n${newText}` : newText;
      const session = {url: modelURL, newFromLine: modelText ? modelText.split('\n').length : 0};
      const after = contents(modelURL, combined, session);
      return listing(after.filter(e => !before.has(`${e.kind}\t${e.name}`)).map(e => [e.kind, e.name]));
    },
  };
})();
