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

# --- the benchmark ---------------------------------------------------------
add_executable(jsonbench
  src/main.cpp
  src/queries.cpp
  src/simdjson_engine.cpp
  src/pison_engine.cpp)
target_include_directories(jsonbench PRIVATE src)
target_link_libraries(jsonbench PRIVATE
  simdjson pison counters Threads::Threads)
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
