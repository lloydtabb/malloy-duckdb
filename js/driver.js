// Runs inside QuickJS, after malloy-embedded.js. It drives the Malloy
// compiler's needs loop, asking the C++ side (globalThis.__malloyHost) for
// whatever the compiler needs:
//
//   __malloyHost.readURL(url)      -> the file's text
//   __malloyHost.describe(sqlFrom) -> JSON: [[column_name, duckdb_type], ...]
//                                     for `SELECT * FROM <sqlFrom>`
//
// Both throw on failure. The entry points, on globalThis.malloyDuckDB, return
// strings and throw Error on a Malloy error, with the messages formatted for
// a DuckDB error.
(function () {
  'use strict';
  const host = globalThis.__malloyHost;
  const malloy = globalThis.malloyEmbedded;
  const CONNECTION = 'duckdb';

  // Used when no model is loaded: queries may still name tables directly,
  // as in `run: duckdb.table('flights') -> { ... }`.
  const EMPTY_MODEL = 'internal://malloy-duckdb/empty.malloy';

  function malloyType(duckType) {
    const t = duckType.toUpperCase();
    if (t === 'VARCHAR' || t.startsWith('VARCHAR(') || t === 'TEXT' || t === 'STRING') {
      return {kind: 'string_type'};
    }
    if (/^(TINYINT|SMALLINT|INTEGER|BIGINT|HUGEINT|UTINYINT|USMALLINT|UINTEGER|UBIGINT|UHUGEINT)$/.test(t)) {
      return {kind: 'number_type', subtype: 'integer'};
    }
    if (/^(FLOAT|REAL|DOUBLE)$/.test(t) || t.startsWith('DECIMAL')) {
      return {kind: 'number_type', subtype: 'decimal'};
    }
    if (t === 'BOOLEAN') return {kind: 'boolean_type'};
    if (t === 'DATE') return {kind: 'date_type'};
    if (t === 'TIMESTAMP' || t === 'TIMESTAMP_S' || t === 'TIMESTAMP_MS' || t === 'TIMESTAMP_NS') {
      return {kind: 'timestamp_type'};
    }
    if (t === 'TIMESTAMP WITH TIME ZONE') return {kind: 'timestamptz_type'};
    if (t === 'JSON') return {kind: 'json_type'};
    // TODO: STRUCT and LIST map to Malloy records and arrays.
    return {kind: 'sql_native_type', sql_type: duckType};
  }

  function schemaFor(sqlFrom) {
    const columns = JSON.parse(host.describe(sqlFrom));
    return {
      fields: columns.map(([name, type]) => ({kind: 'dimension', name, type: malloyType(type)})),
    };
  }

  function where(log) {
    const url = log.url || '';
    const line = log.range ? log.range.start.line + 1 : 0;
    const col = log.range ? log.range.start.character + 1 : 0;
    const file = url.startsWith('internal://query/') ? 'query' : url;
    return line ? `${file}:${line}:${col}` : file;
  }

  function fail(logs) {
    const errors = logs.filter(l => l.severity === 'error');
    throw new Error(errors.map(e => `${e.message} (${where(e)})`).join('\n'));
  }

  // Runs one compile to completion. `compile` is a malloyEmbedded function.
  function drive(compile, request) {
    const needs = {
      connections: [{name: CONNECTION, dialect: 'duckdb'}],
      files: [],
      table_schemas: [],
      sql_schemas: [],
    };
    for (let round = 0; round < 100; round++) {
      const response = JSON.parse(compile(JSON.stringify({...request, compiler_needs: needs})));
      const logs = response.logs || [];
      if (logs.some(l => l.severity === 'error')) fail(logs);
      const asked = response.compiler_needs;
      if (!asked) return response;
      let progress = false;
      for (const file of asked.files || []) {
        const contents = file.url === EMPTY_MODEL ? '' : host.readURL(file.url);
        needs.files.push({url: file.url, contents});
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

  globalThis.malloyDuckDB = {
    getVersion() {
      return malloy.version;
    },

    // Malloy query text -> DuckDB SQL.
    toSQL(modelURL, queryText) {
      const response = drive(malloy.compileQuery, {
        model_url: modelURL || EMPTY_MODEL,
        query_malloy: queryText,
      });
      if (!response.result || typeof response.result.sql !== 'string') {
        throw new Error('Malloy produced no SQL for this query');
      }
      return response.result.sql;
    },

    // Compiles a model and lists what it exports, one "kind\tname" per line.
    describeModel(modelURL) {
      const response = drive(malloy.compileModel, {model_url: modelURL});
      const entries = (response.model && response.model.entries) || [];
      return entries.map(e => `${e.kind}\t${e.name}`).join('\n');
    },
  };
})();
