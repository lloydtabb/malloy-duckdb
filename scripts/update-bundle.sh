#!/usr/bin/env bash
# Rebuilds js/malloy-embedded.js from a Malloy checkout.
#
#   scripts/update-bundle.sh /path/to/malloy
#
# The checkout must have run `npm install`; this builds packages/malloy and
# then the packages/malloy-embedded bundle, and copies it here.
set -euo pipefail
MALLOY=${1:?usage: $0 /path/to/malloy}
HERE=$(cd "$(dirname "$0")/.." && pwd)
(cd "$MALLOY" && npm run build -w packages/malloy && npm run build -w packages/malloy-embedded)
cp "$MALLOY/packages/malloy-embedded/dist/malloy-embedded.js" "$HERE/js/malloy-embedded.js"
VERSION=$(node -p "require('$MALLOY/packages/malloy/package.json').version")
COMMIT=$(git -C "$MALLOY" rev-parse --short HEAD)
echo "Built from malloydata/malloy packages/malloy-embedded, version $VERSION, commit $COMMIT." > "$HERE/js/BUNDLE_SOURCE"
echo "Updated js/malloy-embedded.js to Malloy $VERSION ($COMMIT)"
