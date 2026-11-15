cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME malloy)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME} C CXX)

set(CMAKE_CXX_STANDARD
    "17"
    CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- QuickJS-ng: the JavaScript engine the Malloy compiler runs in ----------
#
# Built here from its four library sources rather than through its own
# CMakeLists, which also builds command-line tools and sets global flags.
set(QUICKJS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickjs)
add_library(
  malloy_quickjs STATIC ${QUICKJS_DIR}/dtoa.c ${QUICKJS_DIR}/libregexp.c
                        ${QUICKJS_DIR}/libunicode.c ${QUICKJS_DIR}/quickjs.c)
set_target_properties(
  malloy_quickjs
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED ON
             C_EXTENSIONS ON
             POSITION_INDEPENDENT_CODE ON
             C_VISIBILITY_PRESET hidden)
target_include_directories(malloy_quickjs PUBLIC ${QUICKJS_DIR})
target_compile_definitions(malloy_quickjs PRIVATE QUICKJS_NG_BUILD _GNU_SOURCE)
if(MSVC)
  target_compile_definitions(
    malloy_quickjs PRIVATE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601
                           _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
  target_compile_options(malloy_quickjs PRIVATE /experimental:c11atomics /w)
else()
  # QuickJS is third-party code: build it with its own expectations
  # (-funsigned-char) and without DuckDB's warnings.
  target_compile_options(malloy_quickjs PRIVATE -funsigned-char -w)
  if(WIN32)
    target_compile_definitions(malloy_quickjs PRIVATE WIN32_LEAN_AND_MEAN
                                                      _WIN32_WINNT=0x0601)
  endif()
endif()
find_package(Threads)
set(MALLOY_QUICKJS_LIBS ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT})
if(NOT WIN32)
  list(APPEND MALLOY_QUICKJS_LIBS m)
endif()
target_link_libraries(malloy_quickjs PUBLIC ${MALLOY_QUICKJS_LIBS})

# --- The Malloy compiler bundle and driver, compiled in as byte arrays ------
set(MALLOY_JS_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated)
function(malloy_embed_js input symbol output_var)
  set(output ${MALLOY_JS_GENERATED}/${symbol}.cpp)
  add_custom_command(
    OUTPUT ${output}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${MALLOY_JS_GENERATED}
    COMMAND
      ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/${input}
      -DOUTPUT=${output} -DSYMBOL=${symbol} -P
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_js.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${input}
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_js.cmake
    COMMENT "Embedding ${input}")
  set(${output_var}
      ${output}
      PARENT_SCOPE)
endfunction()
malloy_embed_js(js/malloy-embedded.js MALLOY_EMBEDDED_JS MALLOY_EMBEDDED_CPP)
malloy_embed_js(js/driver.js MALLOY_DRIVER_JS MALLOY_DRIVER_CPP)
malloy_embed_js(js/help-content.js MALLOY_HELP_JS MALLOY_HELP_CPP)

# --- The extension -----------------------------------------------------------
include_directories(src/include ${QUICKJS_DIR})

set(EXTENSION_SOURCES
    src/malloy_extension.cpp src/malloy_engine.cpp src/malloy_statements.cpp
    src/malloy_store.cpp ${MALLOY_EMBEDDED_CPP} ${MALLOY_DRIVER_CPP} ${MALLOY_HELP_CPP})

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_link_libraries(${EXTENSION_NAME} malloy_quickjs)
target_link_libraries(${LOADABLE_EXTENSION_NAME} malloy_quickjs)

install(
  TARGETS ${EXTENSION_NAME} malloy_quickjs
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
