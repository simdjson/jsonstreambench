// Conventional tree-building JSON parsers, run over the same JSON-lines input
// and the same queries as simdjson and Pison.
//
// Two things are measured for each library:
//
//   run_serial   -- one thread, one document at a time. The baseline a
//                   practitioner gets today.
//   run_parallel -- the same library under this paper's slicing rule: workers
//                   claim byte ranges from one atomic counter, snap them to
//                   line feeds, and parse their own slice with their own
//                   parser. The slicing does not know what parses a document,
//                   so it should carry any of these; run_parallel is how we
//                   check that claim instead of asserting it.
#ifndef JSONBENCH_DOM_ENGINE_H
#define JSONBENCH_DOM_ENGINE_H

#include "common.h"

#include <cstddef>

namespace jsonbench {
namespace dom {

// Which library. Each is a separate translation unit so that a heavy header
// (nlohmann in particular) does not slow every other build.
enum class library { yyjson, rapidjson, boost_json, nlohmann };

const char *library_name(library lib);
// Engine label for the RESULT lines, e.g. "yyjson" / "yyjson-parallel".
const char *engine_name(library lib, bool parallel);
// Version string of the library as built, for the metadata sidecar.
const char *library_version(library lib);
// False when the library was not available at configure time, in which case
// main.cpp skips it silently.
bool available(library lib);

struct corpus_stats {
  size_t longest = 0;
  size_t bytes_above[5] = {}; // thresholds: 64/128/256/512/1024 KiB
};
corpus_stats scan_corpus(const char *data, size_t size);

extraction run_serial(library lib, const char *data, size_t size, query_id q,
                      size_t longest);
extraction run_parallel(library lib, const char *data, size_t size, query_id q,
                        size_t threads, size_t slice_bytes, size_t longest);

// Mirrors parallel::options::static_partition for the DOM drivers, which share
// run_sliced rather than a per-call options struct. Set before a run.
inline bool &static_partition_flag() {
  static bool value = true;
  return value;
}

} // namespace dom
} // namespace jsonbench

#endif
