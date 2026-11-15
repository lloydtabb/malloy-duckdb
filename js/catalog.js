// What malloy describe, malloy describe text and malloy sources show: every
// source and named query a model can see, with every field reachable through
// joins and nested data. Sets globalThis.malloyCatalog.catalogOf(modelDef).
// Runs after malloy-embedded.js. Moved here from malloydata/malloy#3211.
(() => {
  var __defProp = Object.defineProperty;
  var __defNormalProp = (obj, key, value) => key in obj ? __defProp(obj, key, { enumerable: true, configurable: true, writable: true, value }) : obj[key] = value;
  var __publicField = (obj, key, value) => __defNormalProp(obj, typeof key !== "symbol" ? key + "" : key, value);
  const {
    Annotations,
    expressionIsAggregate,
    expressionIsAnalytic,
    isJoined,
    isSourceDef,
    annotationsForRoute,
    modelDefToModelInfo,
    payloadOf,
    routeOf
  } = globalThis.malloyEmbedded.malloy;
  const { maybeQuoteIdentifier } = globalThis.malloyEmbedded.interfaces;
  const MAX_JOIN_DEPTH = 4;
  function description(annotations) {
    const notes = new Annotations(annotations);
    for (const route of ['"', "docs"]) {
      const text = notes.forRoute(route).map((n) => n.content.trim()).filter((t) => t).join("\n");
      if (text) return text;
    }
    return void 0;
  }
  function tags(annotations) {
    return new Annotations(annotations).forRoute().filter((n) => n.route !== '"' && n.route !== "docs").map((n) => n.text.trim()).filter((t) => t);
  }
  function nameOf(field) {
    return field.as ?? field.name;
  }
  const quoteName = maybeQuoteIdentifier;
  function join(path, name) {
    return path ? `${path}.${quoteName(name)}` : quoteName(name);
  }
  function typeName(def) {
    switch (def.type) {
      case "sql native":
        return def.rawType ? `sql native (${def.rawType})` : "sql native";
      case "array":
        if (def.elementTypeDef?.type === "record_element") {
          return `${recordTypeName(def.fields)}[]`;
        }
        return `${typeName(def.elementTypeDef ?? { type: "sql native" })}[]`;
      case "record":
      case "record_element":
        return recordTypeName(def.fields);
      default:
        return String(def.type);
    }
  }
  function recordTypeName(fields) {
    const parts = (fields ?? []).map((f) => `${nameOf(f)} :: ${typeName(f)}`);
    return `{${parts.join(", ")}}`;
  }
  function isPublic(field) {
    return field.accessModifier === void 0;
  }
  class Walker {
    constructor(sourceName) {
      __publicField(this, "sourceName", sourceName);
      __publicField(this, "fields", []);
    }
    walk(struct, path, grain, depth, entered = /* @__PURE__ */ new Set()) {
      const joins = [];
      for (const field of struct.fields) {
        if (!isPublic(field)) continue;
        const def = field;
        const fieldPath = join(path, nameOf(def));
        if (def.type === "turtle") {
          this.view(def, fieldPath, path, grain);
        } else if (isJoined(def)) {
          this.joinField(def, fieldPath, grain);
          joins.push(def);
        } else {
          this.atomic(def, fieldPath, path, grain);
        }
      }
      for (const def of joins) {
        const fieldPath = join(path, nameOf(def));
        const fans = def.join !== "one";
        if (isPlainArray(def)) {
          const each = def.fields.find((f) => f.name === "each");
          if (each) {
            this.fields.push({
              path: `${fieldPath}.each`,
              kind: "dimension",
              type: typeName(each),
              grain: fieldPath,
              annotations: [],
              usage: `One value per element of ${fieldPath}; ${fieldPath}.value is the same`,
              examples: [`group_by: ${fieldPath}.each`]
            });
          }
          continue;
        }
        const identity = sourceIdentity(def);
        if (depth < MAX_JOIN_DEPTH && !(identity && entered.has(identity))) {
          const next = identity ? /* @__PURE__ */ new Set([...entered, identity]) : entered;
          this.walk(def, fieldPath, fans ? fieldPath : grain, depth + 1, next);
        }
      }
    }
    atomic(def, path, parent, grain) {
      const kind = expressionIsAnalytic(def.expressionType) ? "calculation" : expressionIsAggregate(def.expressionType) ? "measure" : "dimension";
      const type = typeName(def);
      this.fields.push({
        path,
        kind,
        type,
        grain,
        description: description(def.annotations),
        annotations: tags(def.annotations),
        definition: def.code,
        ...usage(kind, type, path, parent, grain)
      });
    }
    view(def, path, parent, grain) {
      this.fields.push({
        path,
        kind: "view",
        type: "view",
        grain,
        description: description(def.annotations),
        annotations: tags(def.annotations),
        usage: parent ? `A view of ${parent}` : `A query of ${quoteName(this.sourceName)}: run: ${quoteName(this.sourceName)} -> ${path}, or nest it`,
        examples: parent ? [] : [`nest: ${path}`]
      });
    }
    joinField(def, path, grain) {
      const column = firstField(def, () => true);
      const number = firstField(def, (f) => f.type === "number");
      const measure = def.fields.find(
        (f) => isPublic(f) && !isJoined(f) && expressionIsAggregate(f.expressionType)
      );
      const of = grain ? `${grain} row` : `${quoteName(this.sourceName)} row`;
      if (def.type === "array" || def.type === "record") {
        const many2 = def.type === "array";
        const element = isPlainArray(def) ? `${path}.each` : column && join(path, nameOf(column));
        this.fields.push({
          path,
          kind: "dimension",
          type: typeName(def),
          grain,
          description: description(def.annotations),
          annotations: tags(def.annotations),
          usage: many2 ? `Nested data, many elements per ${of}: grouping by an element field gives a row per element; ${path}.count() counts elements` : `Nested data, one per ${of}: its fields are ${path}.<field>`,
          examples: [
            ...element ? [`group_by: ${element}`] : [],
            ...many2 ? [`aggregate: n is ${path}.count()`] : []
          ]
        });
        return;
      }
      const relationship = String(def.join);
      const many = relationship !== "one";
      this.fields.push({
        path,
        kind: "join",
        type: `join_${relationship}`,
        grain,
        description: description(def.annotations),
        annotations: tags(def.annotations),
        usage: many ? `Many ${path} per ${of}: grouping by a ${path} field gives a row per ${path}; aggregates of ${path} fields run over ${path} rows, each counted once` : `One ${path} per ${of}: its fields are ${path}.<field>; aggregates of them run over ${path} rows, each counted once`,
        examples: [
          ...many ? [`aggregate: n is ${path}.count()`] : [],
          ...column ? [`group_by: ${join(path, nameOf(column))}`] : [],
          ...number ? [`aggregate: total is ${join(path, nameOf(number))}.sum()`] : [],
          ...measure ? [`aggregate: ${join(path, nameOf(measure))}`] : []
        ]
      });
    }
  }
  function sourceIdentity(def) {
    if (def.type === "array" || def.type === "record") return void 0;
    return def.sourceID ?? def.name;
  }
  function isPlainArray(def) {
    return def.type === "array" && def.elementTypeDef?.type !== "record_element";
  }
  function firstField(struct, test) {
    return struct.fields.find(
      (f) => isPublic(f) && f.type !== "turtle" && !isJoined(f) && !expressionIsAggregate(f.expressionType) && !expressionIsAnalytic(f.expressionType) && test(f)
    );
  }
  function usage(kind, type, path, parent, grain) {
    const over = parent ? `, over ${parent} rows, each counted once` : "";
    switch (kind) {
      case "measure":
        return {
          usage: parent ? `A measure of ${parent}, computed over ${parent} rows, each counted once` : "A measure",
          examples: [`aggregate: ${path}`]
        };
      case "calculation":
        return { usage: "A calculation (window function)", examples: [] };
      default: {
        const per = grain ? `; grouping by it gives a row per ${grain}` : "";
        if (type === "number") {
          return {
            usage: `A number${per}; .sum(), .avg(), .min() and .max() aggregate it${over}`,
            examples: [`group_by: ${path}`, `aggregate: total is ${path}.sum()`]
          };
        }
        return {
          usage: `A ${type}${per}; count(${path}) counts its distinct values`,
          examples: [`group_by: ${path}`, `aggregate: n is count(${path})`]
        };
      }
    }
  }
  function stableTypeName(type) {
    switch (type.kind) {
      case "sql_native_type":
        return type.sql_type ? `sql native (${type.sql_type})` : "sql native";
      case "array_type":
        return `${stableTypeName(type.element_type)}[]`;
      case "record_type":
        return `{${type.fields.map((f) => `${f.name} :: ${stableTypeName(f.type)}`).join(", ")}}`;
      default:
        return type.kind.replace(/_type$/, "");
    }
  }
  function stableDescription(annotations) {
    for (const route of ['"', "docs"]) {
      const text = annotationsForRoute(annotations, route).map((a) => payloadOf(a).trim()).filter((t) => t).join("\n");
      if (text) return text;
    }
    return void 0;
  }
  function queryFields(modelDef, name) {
    const info = modelDefToModelInfo({ ...modelDef, exports: [name] });
    const fields = info.entries.find((e) => e.name === name)?.schema.fields ?? [];
    return fields.map((f) => ({
      path: quoteName(f.name),
      kind: f.kind === "calculate" ? "calculation" : f.kind,
      type: "type" in f ? stableTypeName(f.type) : f.kind,
      grain: "",
      description: stableDescription(f.annotations),
      annotations: (f.annotations ?? []).filter((a) => !['"', "docs"].includes(routeOf(a) ?? "")).map((a) => a.value.trim()),
      usage: `A column of ${quoteName(name)}'s output`,
      examples: []
    }));
  }
  function catalogOf(modelDef) {
    const entries = [];
    for (const [name, def] of Object.entries(modelDef.contents)) {
      if (isSourceDef(def)) {
        const walker = new Walker(name);
        walker.walk(
          def,
          "",
          "",
          0,
          /* @__PURE__ */ new Set([String(def.sourceID ?? name)])
        );
        const source = def;
        entries.push({
          kind: "source",
          name,
          description: description(source.annotations),
          annotations: tags(source.annotations),
          primary_key: source.primaryKey,
          filters: (source.filterList ?? []).map((f) => f.code).filter((c) => typeof c === "string"),
          parameters: Object.values(source.parameters ?? {}).map(
            (p) => `${p.name} :: ${typeName(p)}`
          ),
          fields: walker.fields
        });
      } else if (def.type === "query") {
        const entry = {
          kind: "query",
          name,
          description: description(def.annotations),
          annotations: tags(def.annotations),
          filters: [],
          parameters: [],
          fields: []
        };
        try {
          entry.fields = queryFields(modelDef, name);
        } catch (e) {
          entry.problem = e instanceof Error ? e.message : String(e);
        }
        entries.push(entry);
      }
    }
    const own = modelDef.modelAnnotations?.[modelDef.modelID];
    return { description: description(own?.ownNotes), entries };
  }
  globalThis.malloyCatalog = { catalogOf };
})();
