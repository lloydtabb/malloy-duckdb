// Runs inside QuickJS, after malloy-embedded.js and help-content.js. It is a
// Malloy host: one long-lived Runtime, asking the C++ side
// (globalThis.__malloyHost) for files and table schemas:
//
//   __malloyHost.readURL(url)      -> the file's text
//   __malloyHost.describe(sqlFrom) -> JSON: [[column_name, duckdb_type], ...]
//                                     for `SELECT * FROM <sqlFrom>`
//   __malloyHost.modelKeys(url)    -> JSON: the cache keys install wrote for
//                                     the installed model `url` belongs to
//
// Each throws on failure.
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
  const {malloy, duckdb, engine} = globalThis.malloyEmbedded;
  const CONNECTION = 'duckdb';

  // What one compile touched, for the model cache and for `collect`.
  let recording = null; // {files: Map<url, contents>, tables: Map<from, describeJSON>}
  let inMemory = new Map(); // url -> model text the C++ side holds in memory

  // The connection: everything Malloy does with DuckDB (DESCRIBE, the type
  // mapping) is DuckDBCommon's. The only SQL Malloy sends while compiling is
  // `DESCRIBE SELECT * FROM <x>`, which the host answers by binding <x> in
  // the caller's own connection and transaction.
  const DESCRIBE = /^DESCRIBE SELECT \* FROM ([\s\S]*)$/;
  const connectionHost = {
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
  };
  const connection = new duckdb.HostDuckDBConnection(CONNECTION, connectionHost);

  // Install wrote cache keys for each installed model: its files' digests
  // and the tables its compile described (malloy.files.digest,
  // malloy.tables). They are fetched once per call to the driver, one host
  // round trip per model, as are table schemas; both are dropped when the
  // call ends, so every call sees the database as it is.
  const STORE_SCHEME = 'malloy-installed://';
  let storeKeys = new Map(); // FileURL -> {files: {url: md5}, tables: [from]}
  let schemas = new Map(); // from -> DESCRIBE JSON, or a marker if it fails
  function forgetCallState() {
    storeKeys = new Map();
    schemas = new Map();
  }
  function installedKeys(url) {
    let keys = storeKeys.get(url);
    if (!keys) {
      keys = JSON.parse(host.modelKeys(url));
      storeKeys.set(url, keys);
      for (const file of Object.keys(keys.files)) storeKeys.set(file, keys);
    }
    return keys;
  }
  function schemaOf(from) {
    if (!schemas.has(from)) {
      let json;
      try {
        json = host.describe(from);
      } catch (e) {
        json = `(cannot describe: ${e.message})`;
      }
      schemas.set(from, json);
    }
    return schemas.get(from);
  }

  // The tables and SQL sources a compile described, by the URL of every file
  // it read, for models that were not installed (the session model, files on
  // disk): an imported file's key must change with the tables its sources
  // read, or Malloy serves the import from its cache with the old schema.
  const modelTables = new Map();
  function tablesFor(url) {
    return url.startsWith(STORE_SCHEME) ? installedKeys(url).tables : modelTables.get(url) || [];
  }

  // A file's invalidation key for Malloy's CacheManager: what the file says
  // (install's digest, else its text), then what the tables of the model at
  // that URL look like now, bound in the caller's connection. A cached model
  // is reused only while every file's key is unchanged, so a reinstalled
  // model, CREATE OR REPLACE TABLE, or another connection's temporary table
  // by the same name all mean a recompile. The key holds the strings
  // themselves: CacheManager only compares them.
  function invalidationKey(url, contents) {
    let what;
    if (inMemory.has(url)) what = inMemory.get(url);
    else if (url.startsWith(STORE_SCHEME)) what = installedKeys(url).files[url];
    else what = contents ?? host.readURL(url);
    return JSON.stringify([what, tablesFor(url).map(schemaOf)]);
  }

  const urlReader = {
    async readURL(url) {
      const u = url.toString();
      const contents = inMemory.has(u) ? inMemory.get(u) : host.readURL(u);
      if (recording) {
        if (u === recording.url) recording.read = true;
        if (!inMemory.has(u)) recording.files.set(u, contents);
      }
      return {contents, invalidationKey: invalidationKey(u, contents)};
    },
    async getInvalidationKey(url) {
      return invalidationKey(url.toString());
    },
  };

  const runtime = new malloy.Runtime({
    connection,
    urlReader,
    cacheManager: new malloy.CacheManager(new malloy.InMemoryModelCache()),
  });

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

  // The compiled model at `modelURL`. CacheManager returns it from the
  // cache while every file's invalidation key is unchanged; when it
  // compiles instead, the tables the compile described are recorded for the
  // model's next key.
  async function loadModel(modelURL, modelText) {
    const materializer = runtime.loadModel(new URL(modelURL), refresh());
    const seen = (recording = {url: modelURL, read: false, files: new Map(), tables: new Map()});
    try {
      await withModel(modelURL, modelText, () => materializer.getModel());
    } catch (e) {
      fail(e);
    } finally {
      recording = null;
    }
    if (seen.read) {
      const tables = [...seen.tables.keys()];
      for (const url of [modelURL, ...seen.files.keys()]) modelTables.set(url, tables);
    }
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

  // malloyyo's MCP engine answers `malloy sources` and `malloy describe`, as
  // its list_sources and describe_source do, through this host. Each call
  // gets its own connection: the engine compiles through the Runtime it is
  // handed, and a fresh schema cache describes every table in the caller's
  // current context.
  function engineHost(modelURL, modelText) {
    const readSource = href =>
      inMemory.has(href) ? inMemory.get(href) : typeof modelText === 'string' && href === modelURL ? modelText : host.readURL(href);
    return {
      withModel: (ref, fn) =>
        withModel(modelURL, modelText, () =>
          fn({
            runtime: new malloy.Runtime({
              connection: new duckdb.HostDuckDBConnection(CONNECTION, connectionHost),
              urlReader: runtime.urlReader,
            }),
            entry: new URL(modelURL),
            readSource,
          })
        ),
    };
  }

  // The engine's tool, called as an MCP client would call it.
  async function engineTool(name, modelURL, modelText, args) {
    const tool = engine.exploreSurface(engineHost(modelURL, modelText)).tools.find(t => t.name === name);
    const result = await tool.handler({...args, model_ref: modelURL});
    if (!result.ok) {
      const errors = (result.problems || []).filter(p => p.severity !== 'warning');
      throw new Error(errors.map(p => p.message).join('\n') || `${name} failed`);
    }
    return result;
  }

  // The model as the engine compiles it: sources and named queries.
  async function engineModel(modelURL, modelText) {
    return engineHost(modelURL, modelText).withModel(modelURL, async m => {
      const compiled = await engine.compile(m.runtime, m.entry, {readSource: m.readSource});
      if (!compiled.ok) fail({problems: compiled.problems.map(p => ({...p, severity: p.severity ?? 'error'}))});
      return compiled.model;
    });
  }

  // A describe_source type as one line: a scalar's name, or a record's fields.
  function typeText(type) {
    if (typeof type === 'string') return type;
    if (type && typeof type === 'object') {
      return `{${Object.entries(type).map(([n, d]) => `${n} :: ${d.type === undefined ? 'array' : typeText(d.type)}`).join(', ')}}`;
    }
    return '';
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
      return globalThis.malloyEmbedded.version;
    },

    // Malloy query text -> DuckDB SQL.
    async toSQL(modelURL, queryText, modelText) {
      return (await prepared(modelURL, queryText, modelText)).sql;
    },

    // Rows of kind, name, description, primary_key, joins, fields: every
    // source and named query the model can see, as the engine compiles it.
    async listSources(modelURL, modelText) {
      const model = await engineModel(modelURL, modelText);
      const counts = groups =>
        [
          [groups.dimensions.length, 'dimension'],
          [groups.measures.length, 'measure'],
          [groups.views.length, 'view'],
        ]
          .filter(([n]) => n > 0)
          .map(([n, word]) => `${n} ${word}${n === 1 ? '' : 's'}`)
          .join(', ');
      return listing([
        ...Object.values(model.sources).map(s => [
          'source',
          s.name,
          s.description || '',
          s.primary_key || '',
          s.joins.map(j => `${j.name} (${j.relationship})`).join(', '),
          counts(s),
        ]),
        ...model.queries.map(q => ['query', q.name, q.description || '', '', '', '']),
      ]);
    },

    // Rows of name, kind, type, fans_out, description, instructions,
    // expression: describe_source's answer, one row per field, join and view.
    async describe(modelURL, name, modelText) {
      const r = await engineTool('describe_source', modelURL, modelText, {source: name});
      const rows = [];
      const fields = (schema, prefix, fans) => {
        for (const [n, d] of Object.entries(schema.dimensions || {})) {
          if (d.type === undefined) continue; // an array: its joins entry describes it
          rows.push([prefix + n, 'dimension', typeText(d.type), fans, d.description || '', d.instructions || '', d.expression || '']);
        }
        for (const [n, d] of Object.entries(schema.measures || {})) {
          rows.push([prefix + n, 'measure', typeText(d.type), fans, d.description || '', d.instructions || '', d.expression || '']);
        }
      };
      const root = r.described_source;
      fields(root, '', 'false');
      for (const [n, description] of Object.entries(root.views || {})) {
        rows.push([n, 'view', 'view', 'false', description || '', '', '']);
      }
      for (const [path, j] of Object.entries(r.joins || {})) {
        const schema = j.source_def || (j.source && r.join_source_map[j.source]) || {};
        const fans = j.fans_out ? 'true' : 'false';
        rows.push([j.quoted_path || path, j.is_array ? 'array' : 'join', j.is_array ? 'array' : j.source || '', fans, schema.description || '', schema.instructions || '', j.code || '']);
        fields(schema, `${j.quoted_path || path}.`, fans);
      }
      return listing(rows);
    },

    // describe_source's answer as an MCP client receives it, one row per
    // line: the JSON, then the source's Malloy text.
    async describeText(modelURL, name, modelText) {
      // Without `guidance`: it tells an MCP client how to use the query tool and
      // share links, which have no counterpart inside DuckDB.
      const {guidance, ...r} = await engineTool('describe_source', modelURL, modelText, {source: name});
      const blocks = engine.toContent(r).content.map(c => c.text);
      return listing(blocks.join('\n\n').split('\n').map(line => [line]));
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
    //   table   <from>             every table and SQL source it described
    // For `malloy install` and `malloy check`.
    async collect(modelURL) {
      // Always a fresh compile, without the cache: what matters is every file
      // and table it reads, which install records as the model's cache keys.
      recording = {url: modelURL, read: false, files: new Map(), tables: new Map()};
      let modelDef;
      try {
        const fresh = new malloy.Runtime({
          connection: new duckdb.HostDuckDBConnection(CONNECTION, connectionHost),
          urlReader,
        });
        modelDef = (await fresh.loadModel(new URL(modelURL)).getModel())._modelDef;
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
      const tables = [...seen.tables.keys()].map(table => ['table', table]);
      return listing([...files, ...entries, ...warnings, ...tables]);
    },
  };

  // Each call sees the database as it is: install's keys and table schemas
  // are fetched afresh for it. A call can start inside another: describing a
  // view defined with malloy() runs Malloy again. So each call keeps its own
  // state, and gives the caller's back when it ends.
  for (const [name, fn] of Object.entries(globalThis.malloyDuckDB)) {
    globalThis.malloyDuckDB[name] = (...args) => {
      const caller = {recording, inMemory, storeKeys, schemas};
      const restore = () => ({recording, inMemory, storeKeys, schemas} = caller);
      forgetCallState();
      recording = null;
      inMemory = new Map();
      try {
        return Promise.resolve(fn(...args)).finally(restore);
      } catch (e) {
        restore();
        throw e;
      }
    };
  }
})();
