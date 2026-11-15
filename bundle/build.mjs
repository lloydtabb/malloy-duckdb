// Builds js/malloy-embedded.js from a Malloy checkout and a malloyyo checkout:
//
//   node bundle/build.mjs /path/to/malloy /path/to/malloyyo
//
// Both checkouts must be built (Malloy: `npm run build`; malloyyo's engine:
// `npm run gen -w packages/mcp-engine`, which writes its help content). Every
// @malloydata package resolves inside the Malloy checkout, so the bundle holds
// one copy of Malloy, which the engine needs (it checks `instanceof
// MalloyError`). js/BUNDLE_SOURCE records both commits.
import * as esbuild from 'esbuild';
import {execSync} from 'child_process';
import {readFileSync, writeFileSync} from 'fs';
import path from 'path';

const here = path.dirname(new URL(import.meta.url).pathname);
const [malloyDir, malloyyoDir] = process.argv.slice(2).map(p => path.resolve(p ?? ''));
if (!process.argv[3]) {
  console.error('usage: node bundle/build.mjs /path/to/malloy /path/to/malloyyo');
  process.exit(2);
}
const engineCore = path.join(malloyyoDir, 'packages/mcp-engine/src/core.ts');

function commit(dir) {
  const git = cmd => execSync(`git ${cmd}`, {cwd: dir}).toString().trim();
  return git('rev-parse --short HEAD') + (git('status --porcelain') ? '-dirty' : '');
}

const fromCheckouts = {
  name: 'from-checkouts',
  setup(build) {
    build.onResolve({filter: /^@malloydata\//}, async args => {
      if (args.pluginData === 'from-checkouts') return undefined;
      return build.resolve(args.path, {
        kind: args.kind,
        resolveDir: path.join(malloyDir, 'packages/malloy'),
        pluginData: 'from-checkouts',
      });
    });
    build.onResolve({filter: /^@malloyyo\/mcp-engine\/core$/}, () => ({path: engineCore}));
  },
};

const malloyVersion = JSON.parse(
  readFileSync(path.join(malloyDir, 'packages/malloy/package.json'), 'utf8')
).version;
const sources = `malloydata/malloy ${commit(malloyDir)}, malloydata/malloyyo ${commit(malloyyoDir)}`;

await esbuild.build({
  entryPoints: [path.join(here, 'src/index.ts')],
  outfile: path.join(here, '../js/malloy-embedded.js'),
  bundle: true,
  format: 'iife',
  platform: 'neutral',
  mainFields: ['main', 'module'],
  target: 'es2020',
  minify: true,
  legalComments: 'eof',
  inject: [path.join(here, 'src/process-shim.ts')],
  define: {
    __MALLOY_VERSION__: JSON.stringify(malloyVersion),
    __SOURCES__: JSON.stringify(sources),
  },
  plugins: [fromCheckouts],
  logLevel: 'warning',
});
writeFileSync(
  path.join(here, '../js/BUNDLE_SOURCE'),
  `Built by bundle/build.mjs, Malloy ${malloyVersion}, from ${sources}.\n`
);
console.log(`js/malloy-embedded.js: Malloy ${malloyVersion}, ${sources}`);
