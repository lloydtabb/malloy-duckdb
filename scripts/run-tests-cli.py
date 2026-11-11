#!/usr/bin/env python3
"""Replays test/sql/malloy.test through a DuckDB CLI, in one session.

DuckDB's own test runner needs a full DuckDB build; this checks the same
file against the CLI that scripts/quick-build.sh downloads:

    scripts/run-tests-cli.py [cli] [extension]

Understands what the test file uses: statement ok / statement error (with
an expected substring) and query (with expected rows, tab-separated).
"""
import os, re, subprocess, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
cli = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'build/duckdb-cli/duckdb')
ext = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, 'build/release/extension/malloy/malloy.duckdb_extension'))  # macOS refuses relative paths
test = os.path.join(root, 'test/sql/malloy.test')

records = []
for block in open(test).read().split('\n\n'):
    lines = [l for l in block.split('\n') if not l.startswith('#')]
    while lines and not lines[0].strip():
        lines.pop(0)
    if not lines:
        continue
    head = lines[0].split()
    if head[0] == 'require':
        records.append(('setup', f"LOAD '{ext}';", None))
        continue
    body = lines[1:]
    if '----' in body:
        i = body.index('----')
        sql, expected = '\n'.join(body[:i]), body[i + 1:]
    else:
        sql, expected = '\n'.join(body), None
    records.append((' '.join(head[:2]), sql, expected))

script = ['.mode list', '.headers off', ".separator '\t'", '.bail off', '.nullvalue NULL']
for n, (kind, sql, _) in enumerate(records):
    script.append(f'.print ###{n}')
    script.append(sql.rstrip().rstrip(';') + ';')
out = subprocess.run(f"'{cli}' -unsigned 2>&1", shell=True, input='\n'.join(script) + '\n',
                     capture_output=True, text=True, cwd=root).stdout
segments = re.split(r'^###(\d+)$', out, flags=re.M)
results = {int(segments[i]): segments[i + 1].strip('\n') for i in range(1, len(segments), 2)}

failures = 0
for n, (kind, sql, expected) in enumerate(records):
    got = results.get(n, '<no output>')
    is_error = 'Error' in got
    if kind.startswith('statement error'):
        ok = is_error and all(e in got for e in (expected or []) if e)
    elif kind in ('statement ok', 'setup'):
        ok = not is_error
    else:
        # The test runner writes an empty string as (empty).
        rows = ['\t'.join(c if c else '(empty)' for c in line.split('\t')) for line in got.split('\n')] if got else []
        ok = not is_error and rows == (expected or [])
    if not ok:
        failures += 1
        print(f'FAIL #{n} {kind}: {sql[:100]}\n  expected: {expected}\n  got: {got[:600]!r}\n')
print(f'{len(records) - failures}/{len(records)} passed')
sys.exit(1 if failures else 0)
