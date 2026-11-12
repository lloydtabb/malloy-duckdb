#!/usr/bin/env bash
# Builds only the loadable extension, not DuckDB itself, and fetches the
# official DuckDB CLI of the matching version to load it into. Much faster
# than `make`, which builds all of DuckDB.
#
#   scripts/quick-build.sh
#   build/duckdb-cli/duckdb -unsigned
#   D LOAD 'build/release/extension/malloy/malloy.duckdb_extension';
#
# On macOS the LOAD path must be absolute (the official CLI is hardened and
# dyld rejects relative paths), so the hint printed at the end uses one.
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
    -DOVERRIDE_GIT_DESCRIBE="$VERSION" \
    -DCMAKE_BUILD_TYPE=Release -S ./duckdb/ -B build/release
fi
# The extension is stamped with this version, which LOAD checks.
ninja -C build/release malloy_loadable_extension

if [ ! -x build/duckdb-cli/duckdb ]; then
  case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) PLATFORM=linux-amd64 ;;
    Linux-aarch64) PLATFORM=linux-arm64 ;;
    Darwin-*) PLATFORM=osx-universal ;;
    *) echo "No prebuilt DuckDB CLI known for $(uname -s)-$(uname -m)" >&2; exit 1 ;;
  esac
  mkdir -p build/duckdb-cli
  curl -sSfL -o build/duckdb-cli/cli.tar.gz \
    "https://github.com/duckdb/duckdb/releases/download/$VERSION/duckdb-cli-$PLATFORM.tar.gz"
  tar -xzf build/duckdb-cli/cli.tar.gz -C build/duckdb-cli
  rm build/duckdb-cli/cli.tar.gz
fi
echo
echo "Built build/release/extension/malloy/malloy.duckdb_extension for DuckDB $VERSION. Try:"
echo "  build/duckdb-cli/duckdb -unsigned"
echo "  D LOAD '$ROOT/build/release/extension/malloy/malloy.duckdb_extension';"
