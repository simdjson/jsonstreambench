// Dispatch across the DOM libraries. Each library lives in its own translation
// unit and is compiled only when CPM found it, so a missing dependency degrades
// to a skipped engine rather than a build failure.
#include "dom_engine.h"

#include "dom_queries.h"

namespace jsonbench {
namespace dom {

#if JSONBENCH_HAVE_YYJSON
extraction yyjson_run_serial(const char *, size_t, query_id, size_t);
extraction yyjson_run_parallel(const char *, size_t, query_id, size_t, size_t,
                               size_t);
const char *yyjson_version_string();
#endif
#if JSONBENCH_HAVE_RAPIDJSON
extraction rapidjson_run_serial(const char *, size_t, query_id, size_t);
extraction rapidjson_run_parallel(const char *, size_t, query_id, size_t,
                                  size_t, size_t);
const char *rapidjson_version_string();
#endif
#if JSONBENCH_HAVE_BOOST_JSON
extraction boost_json_run_serial(const char *, size_t, query_id, size_t);
extraction boost_json_run_parallel(const char *, size_t, query_id, size_t,
                                   size_t, size_t);
const char *boost_json_version_string();
#endif
#if JSONBENCH_HAVE_NLOHMANN
extraction nlohmann_run_serial(const char *, size_t, query_id, size_t);
extraction nlohmann_run_parallel(const char *, size_t, query_id, size_t,
                                 size_t, size_t);
const char *nlohmann_version_string();
#endif

const char *library_name(library lib) {
  switch (lib) {
  case library::yyjson: return "yyjson";
  case library::rapidjson: return "rapidjson";
  case library::boost_json: return "boost.json";
  case library::nlohmann: return "nlohmann";
  }
  return "?";
}

const char *engine_name(library lib, bool parallel) {
  switch (lib) {
  case library::yyjson: return parallel ? "yyjson-parallel" : "yyjson";
  case library::rapidjson: return parallel ? "rapidjson-parallel" : "rapidjson";
  case library::boost_json:
    return parallel ? "boost.json-parallel" : "boost.json";
  case library::nlohmann: return parallel ? "nlohmann-parallel" : "nlohmann";
  }
  return "?";
}

bool available(library lib) {
  switch (lib) {
  case library::yyjson: return JSONBENCH_HAVE_YYJSON != 0;
  case library::rapidjson: return JSONBENCH_HAVE_RAPIDJSON != 0;
  case library::boost_json: return JSONBENCH_HAVE_BOOST_JSON != 0;
  case library::nlohmann: return JSONBENCH_HAVE_NLOHMANN != 0;
  }
  return false;
}

const char *library_version(library lib) {
  switch (lib) {
#if JSONBENCH_HAVE_YYJSON
  case library::yyjson: return yyjson_version_string();
#endif
#if JSONBENCH_HAVE_RAPIDJSON
  case library::rapidjson: return rapidjson_version_string();
#endif
#if JSONBENCH_HAVE_BOOST_JSON
  case library::boost_json: return boost_json_version_string();
#endif
#if JSONBENCH_HAVE_NLOHMANN
  case library::nlohmann: return nlohmann_version_string();
#endif
  default: return "n/a";
  }
}

corpus_stats scan_corpus(const char *data, size_t size) {
  constexpr size_t thresholds[5] = {64u << 10, 128u << 10, 256u << 10,
                                    512u << 10, 1024u << 10};
  corpus_stats stats;
  for_each_line(data, size, [&](const char *, size_t n) {
    if (n > stats.longest) { stats.longest = n; }
    for (int i = 0; i < 5; i++) {
      if (n > thresholds[i]) { stats.bytes_above[i] += n; }
    }
  });
  return stats;
}

extraction run_serial(library lib, const char *data, size_t size, query_id q,
                      size_t longest) {
  switch (lib) {
#if JSONBENCH_HAVE_YYJSON
  case library::yyjson: return yyjson_run_serial(data, size, q, longest);
#endif
#if JSONBENCH_HAVE_RAPIDJSON
  case library::rapidjson: return rapidjson_run_serial(data, size, q, longest);
#endif
#if JSONBENCH_HAVE_BOOST_JSON
  case library::boost_json: return boost_json_run_serial(data, size, q, longest);
#endif
#if JSONBENCH_HAVE_NLOHMANN
  case library::nlohmann: return nlohmann_run_serial(data, size, q, longest);
#endif
  default: return extraction{};
  }
}

extraction run_parallel(library lib, const char *data, size_t size, query_id q,
                        size_t threads, size_t slice_bytes, size_t longest) {
  switch (lib) {
#if JSONBENCH_HAVE_YYJSON
  case library::yyjson:
    return yyjson_run_parallel(data, size, q, threads, slice_bytes, longest);
#endif
#if JSONBENCH_HAVE_RAPIDJSON
  case library::rapidjson:
    return rapidjson_run_parallel(data, size, q, threads, slice_bytes, longest);
#endif
#if JSONBENCH_HAVE_BOOST_JSON
  case library::boost_json:
    return boost_json_run_parallel(data, size, q, threads, slice_bytes, longest);
#endif
#if JSONBENCH_HAVE_NLOHMANN
  case library::nlohmann:
    return nlohmann_run_parallel(data, size, q, threads, slice_bytes, longest);
#endif
  default: return extraction{};
  }
}

} // namespace dom
} // namespace jsonbench
