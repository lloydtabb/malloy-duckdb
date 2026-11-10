# Usage: cmake -DINPUT=<file.js> -DOUTPUT=<file.cpp> -DSYMBOL=<NAME> -P embed_js.cmake
#
# Writes OUTPUT defining `extern const unsigned char NAME[]` (the file's bytes, then a
# NUL, as JS_Eval requires) and `extern const size_t NAME_LEN` (excluding the
# NUL). A byte array rather than a string literal, because compilers cap the
# length of string literals and the Malloy bundle is megabytes.
file(READ "${INPUT}" content HEX)
string(LENGTH "${content}" hex_length)
math(EXPR byte_count "${hex_length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")
# Break the initializer into lines so the generated file stays readable to tools.
string(REGEX REPLACE "((0x..,){32})" "\\1\n" bytes "${bytes}")
file(WRITE "${OUTPUT}" "// Generated from ${INPUT} by cmake/embed_js.cmake. Do not edit.\n#include <cstddef>\n")
file(APPEND "${OUTPUT}" "extern const unsigned char ${SYMBOL}[];\nextern const size_t ${SYMBOL}_LEN;\n")
file(APPEND "${OUTPUT}" "const unsigned char ${SYMBOL}[] = {\n${bytes}0x00};\n")
file(APPEND "${OUTPUT}" "const size_t ${SYMBOL}_LEN = ${byte_count};\n")
