#ifndef JSONBENCH_SIMDJSON_ENGINE_H
#define JSONBENCH_SIMDJSON_ENGINE_H

#include "common.h"

#include <cstddef>

namespace jsonbench {
namespace sj {

// What a run should do with each document.
enum class workload {
  structure, // walk the stream, touch no value: simdjson's stage 1 alone,
             // the direct analogue of Pison's bitmap construction
  query,     // stage 1 + the JSONPath query, extracting raw tokens, which is
             // what Pison's index+query produces
  decode,    // as `query`, but strings are unescaped and numbers converted to
             // machine types -- the work a real application needs and that a
             // structural index does not provide
};

const char *workload_name(workload w);

// Single-threaded iterate_many. `threaded` turns on simdjson's built-in
// stage1/stage2 overlap (its own two-thread pipeline).
// `trace`, when non-null, collects the first `trace_limit` extracted values so
// that --dump can compare the engines' output directly. Never used on a timed
// run.
extraction run_serial(const char *data, size_t size, query_id q, workload w,
                      bool threaded, size_t batch_bytes,
                      std::vector<std::string> *trace = nullptr,
                      size_t trace_limit = 0);

// experimental::parse_many_parallel from simdjson PR #2788.
extraction run_parallel(const char *data, size_t size, query_id q, workload w,
                        size_t threads, size_t slice_bytes,
                        bool static_partition = false);

const char *implementation_name();

} // namespace sj
} // namespace jsonbench

#endif
