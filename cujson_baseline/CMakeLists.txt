# cujson_baseline: simdjson on one bulky document, as cuJSON's paper measures
# it and as it should be measured. Linux only (it uses madvise huge-page hints).
# The query workloads reuse the benchmark's own query code.
add_executable(cujson_baseline
  cujson_baseline.cpp
  ${PROJECT_SOURCE_DIR}/src/simdjson_engine.cpp
  ${PROJECT_SOURCE_DIR}/src/queries.cpp)
target_include_directories(cujson_baseline PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(cujson_baseline PRIVATE
  JSONBENCH_HAVE_NEWLINE_DELIMITED=${NEWLINE_DELIMITED})
target_link_libraries(cujson_baseline PRIVATE simdjson Threads::Threads)
if(JSONBENCH_NATIVE AND HAS_MARCH_NATIVE)
  target_compile_options(cujson_baseline PRIVATE -march=native)
endif()

# github_archive: cuJSON's end-to-end case study (Figure 18) with simdjson,
# serial and with the parallel driver of src/parallel_stream.h.
add_executable(github_archive github_archive.cpp)
target_include_directories(github_archive PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(github_archive PRIVATE
  JSONBENCH_HAVE_NEWLINE_DELIMITED=${NEWLINE_DELIMITED})
target_link_libraries(github_archive PRIVATE simdjson Threads::Threads)
if(JSONBENCH_NATIVE AND HAS_MARCH_NATIVE)
  target_compile_options(github_archive PRIVATE -march=native)
endif()
