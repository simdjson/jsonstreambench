// nlohmann/json.
//
// The most widely used C++ JSON library and the slowest of the four, by a
// margin that is the point of including it: it is the baseline a great many
// applications are actually running.
//
// There is no arena to reuse here. nlohmann::json allocates each node with the
// global allocator by design -- its value type is a tagged union holding
// std::string, std::vector and std::map -- so unlike the other three there is no
// configuration that avoids per-document allocation. We therefore reuse nothing
// but the parser call itself, which is the library used as intended.
//
// nlohmann validates UTF-8 while parsing, as simdjson does.
#include "dom_engine.h"
#include "dom_parallel.h"
#include "dom_queries.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace jsonbench {
namespace dom {
namespace {

using njson = nlohmann::json;

struct nl_traits {
  using node = const njson *;

  static bool obj_get(node v, std::string_view key, node &out) {
    if (v == nullptr || !v->is_object()) { return false; }
    auto it = v->find(key);
    if (it == v->end()) { return false; }
    out = &(*it);
    return true;
  }

  static bool arr_at(node v, size_t i, node &out) {
    if (v == nullptr || !v->is_array() || i >= v->size()) { return false; }
    out = &(*v)[i];
    return true;
  }

  template <class F> static void arr_each(node v, F &&f) {
    if (v == nullptr || !v->is_array()) { return; }
    for (const njson &elem : *v) { f(&elem); }
  }

  static void feed(node v, extraction &out) {
    if (v == nullptr) { return; }
    if (v->is_string()) {
      const std::string &s = v->get_ref<const std::string &>();
      out.feed(std::string_view(s.data(), s.size()));
    } else if (v->is_number()) {
      feed_number(v->get<double>(), out);
    } else if (v->is_boolean()) {
      feed_literal(v->get<bool>() ? "true" : "false", out);
    } else if (v->is_null()) {
      feed_literal("null", out);
    }
  }
};

class nl_worker {
public:
  explicit nl_worker(query_id q) : q_(q) {}

  void operator()(const char *data, size_t size, extraction &out) {
    for_each_line(data, size, [&](const char *line, size_t len) {
      // Non-throwing parse: a malformed line is skipped rather than aborting
      // the run, matching what the other engines do.
      njson doc = njson::parse(line, line + len, nullptr, false);
      if (doc.is_discarded()) { return; }
      run_query<nl_traits>(q_, &doc, out);
    });
  }

private:
  query_id q_;
};

} // namespace

extraction nlohmann_run_serial(const char *data, size_t size, query_id q,
                               size_t /*longest*/) {
  nl_worker w(q);
  extraction out;
  w(data, size, out);
  return out;
}

extraction nlohmann_run_parallel(const char *data, size_t size, query_id q,
                                 size_t threads, size_t slice_bytes,
                                 size_t /*longest*/) {
  return run_sliced(data, size, threads, slice_bytes, [&] {
    auto w = std::make_shared<nl_worker>(q);
    return [w](const char *d, size_t n, extraction &out) { (*w)(d, n, out); };
  });
}

const char *nlohmann_version_string() {
  static const std::string v = std::to_string(NLOHMANN_JSON_VERSION_MAJOR) +
                               "." +
                               std::to_string(NLOHMANN_JSON_VERSION_MINOR) +
                               "." +
                               std::to_string(NLOHMANN_JSON_VERSION_PATCH);
  return v.c_str();
}

} // namespace dom
} // namespace jsonbench
