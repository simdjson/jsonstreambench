#ifndef JSONBENCH_SIMDJSON_ENGINE_H
#define JSONBENCH_SIMDJSON_ENGINE_H

#include "common.h"
#include "simdjson.h"

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

// As run_serial, but with an explicit stream format (used by the stream-format
// overhead study: whitespace_delimited vs comma_delimited on the same records).
extraction run_serial_format(const char *data, size_t size, query_id q,
                             workload w, bool threaded, size_t batch_bytes,
                             simdjson::stream_format format,
                             std::vector<std::string> *trace = nullptr,
                             size_t trace_limit = 0);

// One bulky document, a top-level array parsed as a single document (not as a
// stream). The query is applied to every element: $[*].user.lang, $[*].lang.
// `structure` runs stage 1 over the whole document and touches nothing.
extraction run_single_document(const char *data, size_t size, query_id q,
                               workload w);

// One bulky record as published ({"tweets": [...]} and so on), parsed with the
// caller's parser: the query runs on every element of the dominating array
// (bulky_array). For cujson_baseline, which controls the parser's allocation.
extraction run_bulky_document(simdjson::ondemand::parser &parser,
                              const simdjson::padded_string &json, query_id q,
                              workload w);

// This repository's own parallel stream driver, src/parallel_stream.h.
extraction run_parallel(const char *data, size_t size, query_id q, workload w,
                        size_t threads, size_t slice_bytes,
                        bool static_partition = false);

const char *implementation_name();

} // namespace sj
} // namespace jsonbench

#endif
