/*
 * Copyright Contributors to the Malloy project
 * SPDX-License-Identifier: MIT
 */

// The script the extension evaluates in QuickJS: Malloy, a DuckDB connection
// whose database is the extension's own, and malloyyo's MCP engine core, so
// `malloy sources` and `malloy describe` answer as malloyyo's list_sources and
// describe_source do. It sets globalThis.malloyEmbedded.

// The globals must exist before the modules that read them load, and ES
// module evaluation follows import order.
import './globals';
import './url';
import * as malloy from '@malloydata/malloy';
import * as duckdb from '@malloydata/db-duckdb/host';
import * as engine from '@malloyyo/mcp-engine/core';

declare const __MALLOY_VERSION__: string;
declare const __SOURCES__: string;

Object.assign(globalThis, {
  malloyEmbedded: {
    /** `@malloydata/malloy`'s version. */
    version: __MALLOY_VERSION__,
    /** The Malloy and malloyyo commits the bundle was built from. */
    sources: __SOURCES__,
    malloy,
    duckdb,
    engine,
  },
});
