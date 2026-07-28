#ifndef JSONBENCH_PISON_ENGINE_H
#define JSONBENCH_PISON_ENGINE_H

#include "common.h"

#include <cstddef>

namespace jsonbench {
namespace pison {

enum class workload {
  structure, // leveled bitmap construction only -- what Pison's published
             // benchmarks time, and what cuJSON reports for Pison
  query,     // bitmap construction + the JSONPath query via BitmapIterator
  decode,    // as `query`, plus unescaping strings / converting numbers, so
             // the output matches what simdjson's decode mode produces
};

const char *workload_name(workload w);

// Task-level parallelism over records: each worker owns whole records and
// builds their indexes independently. This is what Pison's own README
// recommends for a stream of small records ("parallelism can be easily
// achieved at the task level"). threads == 1 reproduces the published
// single-threaded configuration.
// `trace`, when non-null, collects the first `trace_limit` extracted values so
// that --dump can compare the engines' output directly. Only honoured when
// threads == 1; never used on a timed run.
extraction run_stream(const char *text, const record_table &table, query_id q,
                      workload w, int levels, size_t threads,
                      std::vector<std::string> *trace = nullptr,
                      size_t trace_limit = 0);

// One bulky record indexed by Pison's ParallelBitmapConstructor -- the
// scenario Pison's paper is built around, and the one a stream parser does not
// address.
extraction run_single_record(const char *text, size_t size, query_id q,
                             workload w, int levels, int threads);

} // namespace pison
} // namespace jsonbench

#endif
