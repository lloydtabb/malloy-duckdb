#!/usr/bin/env bash
# Builds only the loadable extension, not DuckDB itself, and fetches the
# official DuckDB CLI of the matching version to load it into. Much faster
# than `make`, which builds all of DuckDB.
#
#   scripts/quick-build.sh
#   build/duckdb-cli/duckdb -unsigned
#   D LOAD 'build/release/extension/malloy/malloy.duckdb_extension';
#
# Needs cmake, ninja, a C/C++ compiler, curl and unzip.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
VERSION=$(git -C duckdb describe --tags --exact-match)

if [ ! -f build/release/build.ninja ]; then
  cmake -G Ninja -DEXTENSION_STATIC_BUILD=1 \
    -DDUCKDB_EXTENSION_CONFIGS="$ROOT/extension_config.cmake" \
    -DENABLE_EXTENSION_AUTOLOADING=0 -DENABLE_EXTENSION_AUTOINSTALL=0 \
    -DCMAKE_BUILD_TYPE=Release -S ./duckdb/ -B build/release
fi
ninja -C build/release malloy_loadable_extension

if [ ! -x build/duckdb-cli/duckdb ]; then
  case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) ASSET=duckdb_cli-linux-amd64.zip ;;
    Linux-aarch64) ASSET=duckdb_cli-linux-arm64.zip ;;
    Darwin-*) ASSET=duckdb_cli-osx-universal.zip ;;
    *) echo "No prebuilt DuckDB CLI known for $(uname -s)-$(uname -m)" >&2; exit 1 ;;
  esac
  mkdir -p build/duckdb-cli
  curl -sSfL -o build/duckdb-cli/cli.zip "https://github.com/duckdb/duckdb/releases/download/$VERSION/$ASSET"
  unzip -oq build/duckdb-cli/cli.zip -d build/duckdb-cli
  rm build/duckdb-cli/cli.zip
fi
echo
echo "Built build/release/extension/malloy/malloy.duckdb_extension for DuckDB $VERSION. Try:"
echo "  build/duckdb-cli/duckdb -unsigned"
echo "  D LOAD 'build/release/extension/malloy/malloy.duckdb_extension';"
