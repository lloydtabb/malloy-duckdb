// Runs inside QuickJS, after malloy-embedded.js and catalog.js. It is a
// Malloy host: one long-lived Runtime, asking the C++ side
// (globalThis.__malloyHost) for files and table schemas:
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
  const {malloy, duckdb} = globalThis.malloyEmbedded;
  const CONNECTION = 'duckdb';

  // A small, fast string hash: invalidation keys for model text.
  function hash(text) {
    let h = 0x811c9dc5;
    for (let i = 0; i < text.length; i++) {
      h ^= text.charCodeAt(i);
      h = Math.imul(h, 0x01000193);
    }
    return `${text.length}:${(h >>> 0).toString(16)}`;
  }

  // What one compile touched, for the model cache and for `collect`.
  let recording = null; // {files: Map<url, contents>, tables: Map<from, describeJSON>}
  let inMemory = new Map(); // url -> model text the C++ side holds in memory

  // The connection: everything Malloy does with DuckDB (DESCRIBE, the type
  // mapping) is DuckDBCommon's. The only SQL Malloy sends while compiling is
  // `DESCRIBE SELECT * FROM <x>`, which the host answers by binding <x> in
  // the caller's own connection and transaction.
  const DESCRIBE = /^DESCRIBE SELECT \* FROM ([\s\S]*)$/;
  const connection = new duckdb.HostDuckDBConnection(CONNECTION, {
    // Names the database for Malloy's persist manifests, which this
    // extension doesn't use; every database answers to the same digest.
    digest: 'malloy-duckdb',
    query(sql) {
      const m = DESCRIBE.exec(sql.trim());
      if (!m) throw new Error(`malloy-duckdb runs no SQL while compiling: ${sql}`);
      const json = host.describe(m[1]);
      if (recording) recording.tables.set(m[1], json);
      return JSON.parse(json).map(([column_name, column_type]) => ({column_name, column_type}));
    },
  });

  const runtime = new malloy.Runtime({
    connection,
    urlReader: {
      async readURL(url) {
        const u = url.toString();
        const contents = inMemory.has(u) ? inMemory.get(u) : host.readURL(u);
        if (recording && !inMemory.has(u)) recording.files.set(u, contents);
        return {contents, invalidationKey: hash(contents)};
      },
    },
  });

  // Compiled models, kept across calls. A model depends on the files it
  // read and on the schemas of the tables and SQL sources it described;
  // both can change between statements (CREATE OR REPLACE, ATTACH, another
  // connection's temporary table), so a hit re-reads and re-describes them,
  // which is cheap next to compiling, and recompiles if anything differs.
  const models = new Map(); // key -> {materializer, files, tables}
  const MAX_MODELS = 32;

  function stillValid(entry) {
    for (const [url, contents] of entry.files) {
      let now;
      try {
        now = host.readURL(url);
      } catch (e) {
        return false;
      }
      if (now !== contents) return false;
    }
    for (const [from, json] of entry.tables) {
      let now;
      try {
        now = host.describe(from);
      } catch (e) {
        return false;
      }
      if (now !== json) return false;
    }
    return true;
  }

  // Every compile describes its tables again, in the caller's context, so
  // that what it records is current. `refreshSchemaCache` takes a time; an
  // increasing one refreshes even two compiles in the same millisecond.
  let lastRefresh = 0;
  function refresh() {
    lastRefresh = Math.max(lastRefresh + 1, Date.now());
    return {refreshSchemaCache: lastRefresh};
  }

  // Runs `fn` with `modelText` (if any) serving `modelURL`.
  async function withModel(modelURL, modelText, fn) {
    const saved = inMemory;
    inMemory = new Map(saved);
    if (typeof modelText === 'string') inMemory.set(modelURL, modelText);
    try {
      return await fn();
    } finally {
      inMemory = saved;
    }
  }

  // The compiled model at `modelURL`, from the cache when nothing it
  // depends on has changed.
  async function loadModel(modelURL, modelText) {
    const key = `${modelURL}\u0000${typeof modelText === 'string' ? hash(modelText) : ''}`;
    const hit = models.get(key);
    if (hit && stillValid(hit)) {
      models.delete(key);
      models.set(key, hit); // most recently used last
      return hit.materializer;
    }
    models.delete(key);
    const materializer = runtime.loadModel(new URL(modelURL), refresh());
    const seen = (recording = {files: new Map(), tables: new Map()});
    try {
      await withModel(modelURL, modelText, () => materializer.getModel());
    } catch (e) {
      fail(e); // not cached: the next call compiles again
    } finally {
      recording = null;
    }
    models.set(key, {materializer, files: seen.files, tables: seen.tables});
    while (models.size > MAX_MODELS) models.delete(models.keys().next().value);
    return materializer;
  }

  // How a compile error says where it is. `session` describes the session
  // model, so errors in it can point at the statement being added.
  function where(problem) {
    const url = (problem.at && problem.at.url) || '';
    const start = problem.at && problem.at.range ? problem.at.range.start : undefined;
    const line = start ? start.line + 1 : 0;
    const col = start ? start.character + 1 : 0;
    if (url.startsWith('internal://')) {
      // A query, or a statement being added to the session (extendModel).
      return line ? `line ${line}, column ${col}` : 'query';
    }
    return line ? `${url}:${line}:${col}` : url;
  }

  function fail(e) {
    const problems = e && Array.isArray(e.problems) ? e.problems.filter(p => p.severity === 'error') : [];
    if (!problems.length) throw e instanceof Error ? e : new Error(String(e));
    throw new Error(problems.map(p => `${p.message} (${where(p)})`).join('\n'));
  }

  async function prepared(modelURL, queryText, modelText) {
    const model = await loadModel(modelURL, modelText);
    try {
      return await withModel(modelURL, modelText, () => model.loadQuery(queryText, refresh()).getPreparedResult());
    } catch (e) {
      fail(e);
    }
  }

  // Every source and named query the model can see, described: see
  // js/catalog.js.
  async function catalog(modelURL, modelText) {
    const model = await (await loadModel(modelURL, modelText)).getModel();
    return globalThis.malloyCatalog.catalogOf(model._modelDef).entries;
  }

  // Everything a model can see: sources and named queries, imports included.
  function contentsOf(model) {
    return [
      ...model.explores.map(e => ({kind: 'source', name: e.name})),
      ...model.queries().named.map(name => ({kind: 'query', name})),
    ];
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
    async toSQL(modelURL, queryText, modelText) {
      return (await prepared(modelURL, queryText, modelText)).sql;
    },

    // Rows of kind, name, description, primary_key, filters, joins, fields:
    // every source and named query the model can see.
    async listSources(modelURL, modelText) {
      return listing(
        (await catalog(modelURL, modelText)).map(e => {
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
    async describe(modelURL, name, modelText) {
      const entries = await catalog(modelURL, modelText);
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
    async describeText(modelURL, name, modelText) {
      const entries = await catalog(modelURL, modelText);
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
    async compile(modelURL, queryText, modelText) {
      const {data, ...result} = (await prepared(modelURL, queryText, modelText)).toStableResult();
      return JSON.stringify(result);
    },

    // Checks that `newText` (import, source:, query: statements) compiles
    // when added to the session model, and returns rows of kind, name for
    // what it makes newly visible. The session model is `prefix` (imports of
    // installed sources it needs) followed by `sessionText`. Changes nothing:
    // the caller appends `newText` to its session model.
    async define(modelURL, prefix, sessionText, newText) {
      const text = [prefix, sessionText].filter(p => p).join('\n');
      const before = await loadModel(modelURL, text);
      const known = new Set(contentsOf(await before.getModel()).map(e => `${e.kind}\t${e.name}`));
      // The new statements compile against the session model as it is; error
      // positions are relative to `newText` itself.
      const after = before.extendModel(newText, {importBaseURL: new URL(modelURL), ...refresh()});
      let afterModel;
      try {
        afterModel = await withModel(modelURL, text, () => after.getModel());
      } catch (e) {
        fail(e);
      }
      return listing(contentsOf(afterModel).filter(e => !known.has(`${e.kind}\t${e.name}`)).map(e => [e.kind, e.name]));
    },

    // Compiles the model file at `modelURL` and lists, one row each:
    //   file    <url> <contents>   every file the compile read
    //   entry   <kind> <name>      every source and named query it exports
    //   warning <message>          a source reading a file by a relative path
    // For `malloy install` and `malloy check`.
    async collect(modelURL) {
      // Always a fresh compile: what matters is every file and table it reads.
      recording = {files: new Map(), tables: new Map()};
      let modelDef;
      try {
        modelDef = (await runtime.loadModel(new URL(modelURL), refresh()).getModel())._modelDef;
      } catch (e) {
        fail(e);
      } finally {
        var seen = recording;
        recording = null;
      }
      const files = [...seen.files].map(([url, contents]) => ['file', url, contents]);
      const warnings = [...seen.tables.keys()]
        .filter(looksLikeRelativeFile)
        .map(table => ['warning', `reads the file ${table} by a relative path, which only works where DuckDB runs in the same directory; for a shared database, load it into a table or use an absolute, s3:// or https:// location`]);
      const entries = (modelDef.exports || []).map(name => {
        const def = modelDef.contents[name];
        return ['entry', def && def.type === 'query' ? 'query' : 'source', name];
      });
      return listing([...files, ...entries, ...warnings]);
    },
  };
})();
