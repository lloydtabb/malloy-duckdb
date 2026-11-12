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
  // `observe`, if given, is told of every file read ({url, contents}) and
  // every table described ({table}).
  function drive(compile, request, modelText, session, observe) {
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
        const text = inMemory ? modelText : host.readURL(file.url);
        if (observe && !inMemory) observe({url: file.url, contents: text});
        needs.files.push({url: file.url, contents: text});
        progress = true;
      }
      for (const table of asked.table_schemas || []) {
        if (observe) observe({table: table.name});
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

  // Every source and named query the model can see, described: see
  // compileModelCatalog in malloy-embedded's catalog.ts.
  function catalog(modelURL, modelText) {
    const response = drive(malloy.compileModelCatalog, {model_url: modelURL}, modelText);
    return (response.catalog && response.catalog.entries) || [];
  }

  // Everything the model can see: sources and named queries, imports included.
  function contents(modelURL, modelText, session) {
    const response = drive(malloy.compileModelContents, {model_url: modelURL}, modelText, session);
    return (response.model && response.model.entries) || [];
  }

  // A table path naming a file relative to the working directory, such as
  // data/movies.parquet or 'movies.csv', rather than a table or a URL.
  function looksLikeRelativeFile(path) {
    const p = String(path).replace(/^['"]|['"]$/g, '');
    if (p.startsWith('/') || /^[a-z][a-z0-9+.-]*:\/\//i.test(p) || /^[A-Za-z]:[\\/]/.test(p)) return false;
    return p.includes('/') || /\.(parquet|csv|tsv|json|jsonl|ndjson|xlsx)$/i.test(p);
  }

  // Listings are rows on lines, cells separated by tabs; backslash escapes
  // keep tabs, newlines and backslashes inside a cell (file contents).
  function cell(text) {
    return String(text).replace(/[\\\t\r\n]/g, c => ({'\\': '\\\\', '\t': '\\t', '\r': '\\r', '\n': '\\n'})[c]);
  }

  function listing(rows) {
    return rows.map(row => row.map(cell).join('\t')).join('\n');
  }

  // ── help topics: malloyyo's yo_help topics (js/help-content.js) ─────────
  // Ported from malloyyo packages/mcp-engine/src/help.ts, so topic names and
  // lookup match yo_help: a topic's name is its path, and the language
  // reference is split on its `## ` headings into language/<heading>.
  const LANGUAGE_REFERENCE = 'language/malloy-language-reference.md';

  function slugify(text) {
    return text.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
  }

  function stripHeader(raw) {
    const text = raw.replace(/^<!--[\s\S]*?-->\s*\n/, '');
    const m = /^---\n([\s\S]*?)\n---\n([\s\S]*)$/.exec(text);
    if (!m) return {description: '', body: text};
    const d = /^description:\s*(.+)$/m.exec(m[1]);
    return {description: d ? d[1].trim() : '', body: m[2]};
  }

  let helpTopics = null;
  function topics() {
    if (helpTopics) return helpTopics;
    helpTopics = [];
    const files = globalThis.malloyHelpContent || {};
    for (const key of Object.keys(files)) {
      const name = key.replace(/\.md$/, '').split('/').map(slugify).join('/');
      if (key !== LANGUAGE_REFERENCE) {
        const {description, body} = stripHeader(files[key]);
        helpTopics.push({name, description, body: body.trim()});
        continue;
      }
      // Split the reference into sections; the preamble is language/overview.
      let section = 'language/overview';
      let title = 'Malloy language reference: overview';
      let lines = [];
      const flush = () => {
        const body = lines.join('\n').trim();
        if (body) helpTopics.push({name: section, description: title, body});
        lines = [];
      };
      for (const line of stripHeader(files[key]).body.split('\n')) {
        const m = /^## (?!# )(.+)$/.exec(line);
        if (m) {
          flush();
          section = 'language/' + slugify(m[1].trim());
          title = m[1].trim();
          continue;
        }
        lines.push(line);
      }
      flush();
    }
    return helpTopics;
  }

  // Exact name, then a name containing it (`joins` finds language/joins),
  // then a topic whose text contains every word.
  function findTopic(query) {
    const q = query.toLowerCase().trim();
    const all = topics();
    const direct = all.find(t => t.name === q) || all.find(t => t.name.includes(q));
    if (direct) return direct;
    const words = q.split(/\s+/).filter(Boolean);
    return words.length ? all.find(t => words.every(w => t.body.toLowerCase().includes(w))) : undefined;
  }

  globalThis.malloyDuckDB = {
    // Rows of topic, about: every help topic.
    helpTopics() {
      return listing(topics().map(t => [t.name, t.description]));
    },

    // Rows of one line each: the topic's text, its name first.
    helpTopic(query) {
      const topic = findTopic(query);
      if (!topic) {
        throw new Error(`No help topic matches '${query}'; malloy help lists them`);
      }
      return listing([[topic.name], ...topic.body.split('\n').map(line => [line])]);
    },


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

    // Rows of kind, name, description, primary_key, filters, joins, fields:
    // every source and named query the model can see.
    listSources(modelURL, modelText) {
      return listing(
        catalog(modelURL, modelText).map(e => {
          const joins = e.fields
            .filter(f => f.kind === 'join' && !f.path.includes('.'))
            .map(f => `${f.path} (${f.type.replace(/^join_/, '')})`);
          const own = e.fields.filter(f => !f.path.includes('.') && f.kind !== 'join');
          const count = kind => own.filter(f => f.kind === kind).length;
          const counts = [
            [count('dimension'), 'dimension'],
            [count('measure'), 'measure'],
            [count('calculation'), 'calculation'],
            [count('view'), 'view'],
          ]
            .filter(([n]) => n > 0)
            .map(([n, word]) => `${n} ${word}${n === 1 ? '' : 's'}`);
          return [
            e.kind,
            e.name,
            e.description || '',
            e.primary_key || '',
            e.filters.join('; '),
            joins.join(', '),
            counts.join(', '),
          ];
        })
      );
    },

    // Rows of path, kind, type, grain, description, definition, usage,
    // example: every field of one source or named query, through its joins
    // and nested data.
    describe(modelURL, name, modelText) {
      const entries = catalog(modelURL, modelText);
      const entry = entries.find(e => e.name === name);
      if (!entry) {
        const known = entries.map(e => e.name).sort().join(', ');
        throw new Error(`No source or query named '${name}'` + (known ? `; defined: ${known}` : '; nothing is defined yet'));
      }
      return listing(
        entry.fields.map(f => [
          f.path,
          f.kind,
          f.type,
          f.grain,
          f.description || '',
          (f.annotations || []).join(' '),
          f.definition || '',
          f.usage,
          (f.examples || []).join('; '),
        ])
      );
    },

    // One row per line: a source or named query, compactly. Its own fields
    // grouped by kind, then each join and nested array with its fields.
    describeText(modelURL, name, modelText) {
      const entries = catalog(modelURL, modelText);
      const entry = entries.find(e => e.name === name);
      if (!entry) {
        const known = entries.map(e => e.name).sort().join(', ');
        throw new Error(`No source or query named '${name}'` + (known ? `; defined: ${known}` : '; nothing is defined yet'));
      }
      // One-line annotations go on the field's line; a multi-line one (a
      // block like #|(agent) ... |#) goes on the lines after it, indented.
      const oneLine = text => !text.includes('\n');
      const notes = f => {
        const parts = [];
        if (f.description) parts.push(`-- ${f.description}`);
        for (const a of f.annotations || []) if (oneLine(a)) parts.push(a);
        return parts.length ? '  ' + parts.join('  ') : '';
      };
      const blocks = (f, pad) =>
        (f.annotations || [])
          .filter(a => !oneLine(a))
          .map(a => a.split('\n').map(l => `${pad}  ${l}`).join('\n'));
      const line = f => {
        const name = f.path.includes('.') ? f.path.slice(f.path.lastIndexOf('.') + 1) : f.path;
        let text = name;
        if (f.kind === 'dimension' || f.kind === 'calculation') text += ` :: ${f.type}`;
        if (f.definition) text += ` = ${f.definition.replace(/\s+/g, ' ')}`;
        return text + notes(f);
      };
      const lines = [];
      lines.push(`${entry.kind} ${entry.name}${entry.description ? ' -- ' + entry.description : ''}`);
      for (const a of entry.annotations || []) {
        for (const l of a.split('\n')) lines.push(`  ${l}`);
      }
      if (entry.primary_key) lines.push(`  primary_key: ${entry.primary_key}`);
      for (const f of entry.filters || []) lines.push(`  where: ${f}`);
      for (const p of entry.parameters || []) lines.push(`  parameter: ${p}`);

      const own = entry.fields.filter(f => !f.path.includes('.'));
      const sections = entry.kind === 'query'
        ? [['columns', own]]
        : [
            ['dimensions', own.filter(f => f.kind === 'dimension')],
            ['measures', own.filter(f => f.kind === 'measure')],
            ['calculations', own.filter(f => f.kind === 'calculation')],
            ['views', own.filter(f => f.kind === 'view')],
          ];
      for (const [title, fields] of sections) {
        if (!fields.length) continue;
        lines.push(`${title}:`);
        for (const f of fields) lines.push(`  ${line(f)}`, ...blocks(f, '  '));
      }

      // Joins and nested arrays as a tree: each with its fields, then the
      // joins and arrays inside it.
      const isNested = f => f.kind === 'join' || /\[\]$/.test(f.type);
      const parentOf = path => (path.includes('.') ? path.slice(0, path.lastIndexOf('.')) : '');
      const nested = entry.fields.filter(isNested);
      const emit = (parent, depth) => {
        for (const n of nested.filter(f => parentOf(f.path) === parent)) {
          const pad = '  '.repeat(depth);
          const kind = n.kind === 'join' ? n.type : `${n.type} (nested)`;
          const example = (n.examples || [])[0];
          lines.push(`${pad}${n.path} :: ${kind}${notes(n)}${example ? `  e.g. ${example}` : ''}`, ...blocks(n, pad));
          for (const f of entry.fields) {
            if (parentOf(f.path) !== n.path || isNested(f)) continue;
            lines.push(`${pad}  ${line(f)}`, ...blocks(f, pad + '  '));
          }
          emit(n.path, depth + 1);
        }
      };
      if (nested.length) {
        lines.push('joins and nested data:');
        emit('', 1);
      }
      return listing(lines.map(l => [l]));
    },

    // A query's result metadata, as JSON, without running it: its SQL, its
    // output schema (nested fields included, each with its annotations), and
    // the query's, source's and model's annotations. Malloy's own Result
    // shape, without the data.
    compile(modelURL, queryText, modelText) {
      const response = drive(malloy.compileQuery, {model_url: modelURL, query_malloy: queryText}, modelText);
      if (!response.result) {
        throw new Error('Malloy produced no result for this query');
      }
      const {data, ...result} = response.result;
      return JSON.stringify(result);
    },

    // Checks that `newText` (import, source:, query: statements) compiles
    // when added to the session model, and returns rows of kind, name for
    // what it makes newly visible. The session model is `prefix` (imports of
    // installed sources it needs) followed by `sessionText`. Changes nothing:
    // the caller appends `newText` to its session model.
    define(modelURL, prefix, sessionText, newText) {
      const join = parts => parts.filter(p => p).join('\n');
      const before = new Set(contents(modelURL, join([prefix, sessionText])).map(e => `${e.kind}\t${e.name}`));
      const lines = text => (text ? text.split('\n').length : 0);
      const session = {url: modelURL, newFromLine: lines(prefix) + lines(sessionText)};
      const after = contents(modelURL, join([prefix, sessionText, newText]), session);
      return listing(after.filter(e => !before.has(`${e.kind}\t${e.name}`)).map(e => [e.kind, e.name]));
    },

    // Compiles the model file at `modelURL` and lists, one row each:
    //   file    <url> <contents>   every file the compile read
    //   entry   <kind> <name>      every source and named query it exports
    //   warning <message>          a source reading a file by a relative path
    // For `malloy install` and `malloy check`.
    collect(modelURL) {
      const files = [];
      const warnings = [];
      const response = drive(malloy.compileModel, {model_url: modelURL}, undefined, undefined, seen => {
        if (seen.url !== undefined) {
          files.push(['file', seen.url, seen.contents]);
        } else if (looksLikeRelativeFile(seen.table)) {
          warnings.push(['warning', `reads the file ${seen.table} by a relative path, which only works where DuckDB runs in the same directory; for a shared database, load it into a table or use an absolute, s3:// or https:// location`]);
        }
      });
      const translation = (response.translations || []).find(t => t.url === modelURL);
      const modelDef = translation ? JSON.parse(translation.compiled_model_json) : {contents: {}};
      const entries = ((response.model && response.model.entries) || []).map(e => {
        const def = modelDef.contents[e.name];
        return ['entry', def && def.type === 'query' ? 'query' : e.kind, e.name];
      });
      return listing([...files, ...entries, ...warnings]);
    },
  };
})();
