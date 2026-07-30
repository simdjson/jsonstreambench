// yyjson: the fastest of the conventional DOM parsers we test.
//
// Two configuration choices, both made in yyjson's favour and both disclosed in
// the paper:
//
//   * A pool allocator sized to the longest document in the input, reset by
//     construction on every parse. Without it the measurement is dominated by
//     malloc rather than by parsing, which would flatter us, not yyjson.
//   * We do not pass YYJSON_READ_INSITU. In-situ parsing is faster but writes
//     into the input buffer, and the parallel run shares one read-only buffer
//     across workers, so it is not available there; using it serially only
//     would make the two rows incomparable.
//
// yyjson validates UTF-8 while parsing, as simdjson does, so no flag is needed
// to put the two on the same footing.
#include "dom_engine.h"
#include "dom_parallel.h"
#include "dom_queries.h"

#include "yyjson.h"

#include <cstdlib>
#include <memory>
#include <vector>

namespace jsonbench {
namespace dom {
namespace {

struct yy_traits {
  using node = yyjson_val *;

  static bool obj_get(node v, std::string_view key, node &out) {
    if (v == nullptr || !yyjson_is_obj(v)) { return false; }
    out = yyjson_obj_getn(v, key.data(), key.size());
    return out != nullptr;
  }

  static bool arr_at(node v, size_t i, node &out) {
    if (v == nullptr || !yyjson_is_arr(v)) { return false; }
    out = yyjson_arr_get(v, i);
    return out != nullptr;
  }

  template <class F> static void arr_each(node v, F &&f) {
    if (v == nullptr || !yyjson_is_arr(v)) { return; }
    size_t idx, max;
    yyjson_val *elem;
    yyjson_arr_foreach(v, idx, max, elem) { f(elem); }
  }

  static void feed(node v, extraction &out) {
    if (v == nullptr) { return; }
    switch (yyjson_get_type(v)) {
    case YYJSON_TYPE_STR:
      out.feed(std::string_view(yyjson_get_str(v), yyjson_get_len(v)));
      return;
    case YYJSON_TYPE_NUM:
      feed_number(yyjson_get_num(v), out);
      return;
    case YYJSON_TYPE_BOOL:
      feed_literal(yyjson_get_bool(v) ? "true" : "false", out);
      return;
    case YYJSON_TYPE_NULL:
      feed_literal("null", out);
      return;
    default:
      // An object or array at a leaf of one of our paths never happens on
      // these datasets; simdjson would feed its opening token here.
      return;
    }
  }
};

// One reusable arena plus the query. yyjson's pool allocator needs room for the
// value tree of a single document; yyjson_read_max_memory_usage tells us how
// much, so the arena is exact rather than guessed.
class yy_worker {
public:
  explicit yy_worker(size_t longest) {
    size_t need = yyjson_read_max_memory_usage(longest, 0);
    buf_.resize(need);
    yyjson_alc_pool_init(&alc_, buf_.data(), buf_.size());
  }

  void operator()(const char *data, size_t size, extraction &out) {
    query_id q = q_;
    for_each_line(data, size, [&](const char *line, size_t len) {
      // yyjson_read_opts takes a mutable pointer but only writes to it under
      // YYJSON_READ_INSITU, which we do not set.
      yyjson_doc *doc = yyjson_read_opts(const_cast<char *>(line), len, 0,
                                         &alc_, nullptr);
      if (doc == nullptr) { return; }
      run_query<yy_traits>(q, yyjson_doc_get_root(doc), out);
      yyjson_doc_free(doc); // returns the pool to empty
    });
  }

  void set_query(query_id q) { q_ = q; }

private:
  std::vector<char> buf_;
  yyjson_alc alc_{};
  query_id q_ = query_id::twitter;
};

} // namespace

extraction yyjson_run_serial(const char *data, size_t size, query_id q,
                             size_t longest) {
  yy_worker w(longest);
  w.set_query(q);
  extraction out;
  w(data, size, out);
  return out;
}

extraction yyjson_run_parallel(const char *data, size_t size, query_id q,
                               size_t threads, size_t slice_bytes,
                               size_t longest) {
  return run_sliced(data, size, threads, slice_bytes, [&] {
    auto w = std::make_shared<yy_worker>(longest);
    w->set_query(q);
    return [w](const char *d, size_t n, extraction &out) { (*w)(d, n, out); };
  });
}

const char *yyjson_version_string() { return YYJSON_VERSION_STRING; }

} // namespace dom
} // namespace jsonbench
