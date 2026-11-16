# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(malloy
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

# Any extra extensions that should be built. json: the tests read
# malloy_compile()'s JSON, which the official DuckDB CLI has built in.
duckdb_extension_load(json)