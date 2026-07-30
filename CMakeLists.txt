cmake_minimum_required(VERSION 3.16)
project(jsonstream_bench LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# A self-contained benchmark comparing simdjson (including the experimental
# parallel stream parser of simdjson PR #2788) against Pison, on the JSON-lines
# corpus used by the Pison and cuJSON papers.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
#   ./build/jsonbench --dataset <file.ndjson> --query twitter
#
# Dependencies are pulled at configure time with CPM and pinned by commit; none
# of them is vendored into this repository. Export CPM_SOURCE_CACHE to share a
# single checkout across build trees.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(JSONBENCH_NATIVE "Build with -march=native" ON)

find_package(Threads REQUIRED)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)
include(get_cpm)

# --- simdjson: the PR #2788 branch, pinned by commit -----------------------
# parse_many_parallel lives in include/simdjson/ondemand_parallel.h on this
# branch. Pinning by SHA keeps the paper's numbers reproducible even if the
# branch is rebased or merged.
CPMAddPackage(
  NAME simdjson
  GITHUB_REPOSITORY simdjson/simdjson
  GIT_TAG 88186d59  # PR #2788
  OPTIONS "SIMDJSON_DEVELOPER_MODE OFF" "BUILD_SHARED_LIBS OFF")

# --- lemire/counters: hardware performance counters (header only) ----------
CPMAddPackage(
  NAME counters
  GITHUB_REPOSITORY lemire/counters
  GIT_TAG c75996db608c6f34aeba67b861c5987e3c266ecd
  DOWNLOAD_ONLY YES)
add_library(counters INTERFACE)
target_include_directories(counters INTERFACE ${counters_SOURCE_DIR}/include)

# --- Pison -----------------------------------------------------------------
# Pison ships only a hand-written makefile, so we compile its sources into a
# static library here. Two adjustments are needed, neither of which changes its
# behaviour:
#   * -include cstdint : Pison's sources use uint64_t without including
#     <cstdint>, which GCC 13+ no longer provides transitively.
#   * the SIMD flags from its own makefile (AVX2 + PCLMULQDQ), which its
#     structural-index construction requires.
CPMAddPackage(
  NAME pison
  GITHUB_REPOSITORY AutomataLab/Pison
  GIT_TAG 8a01ce3354742365321b39ae1ed44edc2a91f7ad
  DOWNLOAD_ONLY YES)
file(GLOB PISON_SOURCES ${pison_SOURCE_DIR}/src/*.cpp)
add_library(pison STATIC ${PISON_SOURCES})
target_include_directories(pison SYSTEM PUBLIC ${pison_SOURCE_DIR}/src)
target_compile_options(pison PRIVATE
  -O3 -mavx -mavx2 -msse -msse2 -msse4 -msse4.2 -mpclmul -w)
target_compile_options(pison PUBLIC -include cstdint)
target_compile_features(pison PUBLIC cxx_std_11)
set_target_properties(pison PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- conventional DOM parsers ----------------------------------------------
# Baselines: what a practitioner uses today. Each is optional; a library that
# does not resolve drops its engine from the run rather than breaking the build.
# All are pinned, and all are header-only or single-file, so the checkout stays
# small.
set(DOM_SOURCES src/dom_engine.cpp)
set(HAVE_YYJSON 0)
set(HAVE_RAPIDJSON 0)
set(HAVE_BOOST_JSON 0)
set(HAVE_NLOHMANN 0)

CPMAddPackage(
  NAME yyjson
  GITHUB_REPOSITORY ibireme/yyjson
  GIT_TAG 0.10.0
  DOWNLOAD_ONLY YES)
if(yyjson_ADDED)
  add_library(yyjson STATIC ${yyjson_SOURCE_DIR}/src/yyjson.c)
  target_include_directories(yyjson SYSTEM PUBLIC ${yyjson_SOURCE_DIR}/src)
  set_target_properties(yyjson PROPERTIES POSITION_INDEPENDENT_CODE ON)
  list(APPEND DOM_SOURCES src/yyjson_engine.cpp)
  set(HAVE_YYJSON 1)
endif()

CPMAddPackage(
  NAME rapidjson
  GITHUB_REPOSITORY Tencent/rapidjson
  GIT_TAG 24b5e7a8b27f42fa16b96fc70aade9106cf7102f
  DOWNLOAD_ONLY YES)
if(rapidjson_ADDED)
  add_library(rapidjson INTERFACE)
  target_include_directories(rapidjson SYSTEM INTERFACE
    ${rapidjson_SOURCE_DIR}/include)
  list(APPEND DOM_SOURCES src/rapidjson_engine.cpp)
  set(HAVE_RAPIDJSON 1)
endif()

# Boost.JSON. Its standalone mode was removed after 1.80, so it needs Boost's
# core headers -- but only fifteen header-only libraries of them, listed here
# rather than pulling in Boost entire. They are pinned to the same release as
# Boost.JSON itself, and nothing is compiled: boost_json_engine.cpp includes
# <boost/json/src.hpp>, which is the library's single-translation-unit form.
set(BOOST_RELEASE boost-1.87.0)
set(BOOST_JSON_DEPS
  config assert static_assert throw_exception container move intrusive core
  system variant2 mp11 align endian describe container_hash)
set(BOOST_INCLUDE_DIRS "")
set(BOOST_ALL_FOUND YES)
foreach(blib json ${BOOST_JSON_DEPS})
  CPMAddPackage(
    NAME boost_${blib}
    GITHUB_REPOSITORY boostorg/${blib}
    GIT_TAG ${BOOST_RELEASE}
    DOWNLOAD_ONLY YES)
  if(boost_${blib}_ADDED)
    list(APPEND BOOST_INCLUDE_DIRS ${boost_${blib}_SOURCE_DIR}/include)
  else()
    set(BOOST_ALL_FOUND NO)
  endif()
endforeach()
if(BOOST_ALL_FOUND)
  add_library(boost_json INTERFACE)
  target_include_directories(boost_json SYSTEM INTERFACE ${BOOST_INCLUDE_DIRS})
  list(APPEND DOM_SOURCES src/boost_json_engine.cpp)
  set(HAVE_BOOST_JSON 1)
endif()

CPMAddPackage(
  NAME nlohmann_json
  GITHUB_REPOSITORY nlohmann/json
  GIT_TAG v3.11.3
  DOWNLOAD_ONLY YES)
if(nlohmann_json_ADDED)
  add_library(nlohmann_json INTERFACE)
  target_include_directories(nlohmann_json SYSTEM INTERFACE
    ${nlohmann_json_SOURCE_DIR}/include)
  list(APPEND DOM_SOURCES src/nlohmann_engine.cpp)
  set(HAVE_NLOHMANN 1)
endif()

# --- the benchmark ---------------------------------------------------------
add_executable(jsonbench
  src/main.cpp
  src/queries.cpp
  src/simdjson_engine.cpp
  src/pison_engine.cpp
  ${DOM_SOURCES})
target_include_directories(jsonbench PRIVATE src)
target_compile_definitions(jsonbench PRIVATE
  JSONBENCH_HAVE_YYJSON=${HAVE_YYJSON}
  JSONBENCH_HAVE_RAPIDJSON=${HAVE_RAPIDJSON}
  JSONBENCH_HAVE_BOOST_JSON=${HAVE_BOOST_JSON}
  JSONBENCH_HAVE_NLOHMANN=${HAVE_NLOHMANN})
target_link_libraries(jsonbench PRIVATE
  simdjson pison counters Threads::Threads)
if(HAVE_YYJSON)
  target_link_libraries(jsonbench PRIVATE yyjson)
endif()
if(HAVE_RAPIDJSON)
  target_link_libraries(jsonbench PRIVATE rapidjson)
endif()
if(HAVE_BOOST_JSON)
  target_link_libraries(jsonbench PRIVATE boost_json)
endif()
if(HAVE_NLOHMANN)
  target_link_libraries(jsonbench PRIVATE nlohmann_json)
endif()
if(JSONBENCH_NATIVE)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-march=native" HAS_MARCH_NATIVE)
  if(HAS_MARCH_NATIVE)
    target_compile_options(jsonbench PRIVATE -march=native)
  endif()
endif()

# --- dataset preparation tool ----------------------------------------------
# Derives the JSON-lines form of a dataset from its bulky single-record form by
# minifying every element of the dominating array onto its own line.
add_executable(make_ndjson tools/make_ndjson.cpp)
target_link_libraries(make_ndjson PRIVATE simdjson)
target_include_directories(make_ndjson PRIVATE src)
