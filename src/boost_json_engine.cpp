// Boost.JSON.
//
// Boost.JSON's documented fast path is a `stream_parser` reused across inputs
// together with a monotonic resource, which is what we use: the resource is
// released between documents so the steady state performs no allocation, and
// the parser's own scratch is reused. Parsing straight to `boost::json::value`
// with the default resource would spend most of its time in the allocator.
//
// Boost.JSON validates UTF-8 by default, as simdjson does.
#include "dom_engine.h"
#include "dom_parallel.h"
#include "dom_queries.h"

// src.hpp is Boost.JSON's single-translation-unit form: including it here
// compiles the library into this object file, so no Boost binary is needed.
#include <boost/json/src.hpp>

#include <memory>
#include <vector>

namespace jsonbench {
namespace dom {
namespace {

namespace bj = boost::json;

struct bj_traits {
  using node = const bj::value *;

  static bool obj_get(node v, std::string_view key, node &out) {
    if (v == nullptr || !v->is_object()) { return false; }
    const bj::value *f = v->get_object().if_contains(key);
    if (f == nullptr) { return false; }
    out = f;
    return true;
  }

  static bool arr_at(node v, size_t i, node &out) {
    if (v == nullptr || !v->is_array()) { return false; }
    const bj::array &a = v->get_array();
    if (i >= a.size()) { return false; }
    out = &a[i];
    return true;
  }

  template <class F> static void arr_each(node v, F &&f) {
    if (v == nullptr || !v->is_array()) { return; }
    for (const bj::value &elem : v->get_array()) { f(&elem); }
  }

  static void feed(node v, extraction &out) {
    if (v == nullptr) { return; }
    switch (v->kind()) {
    case bj::kind::string: {
      const bj::string &s = v->get_string();
      out.feed(std::string_view(s.data(), s.size()));
      return;
    }
    case bj::kind::double_: feed_number(v->get_double(), out); return;
    case bj::kind::int64: feed_number(double(v->get_int64()), out); return;
    case bj::kind::uint64: feed_number(double(v->get_uint64()), out); return;
    case bj::kind::bool_:
      feed_literal(v->get_bool() ? "true" : "false", out);
      return;
    case bj::kind::null: feed_literal("null", out); return;
    default: return;
    }
  }
};

class bj_worker {
public:
  explicit bj_worker(size_t longest, query_id q) : q_(q) {
    // Boost.JSON's value tree runs several times the text size; sizing the
    // arena from the longest document keeps the monotonic resource from ever
    // reaching for more memory.
    buf_.resize(longest * 16 + (256u << 10));
  }

  void operator()(const char *data, size_t size, extraction &out) {
    for_each_line(data, size, [&](const char *line, size_t len) {
      // A fresh monotonic_resource over the same buffer is how Boost.JSON
      // "frees" a document: the resource owns every node, so resetting it is
      // the whole deallocation.
      bj::monotonic_resource mr(buf_.data(), buf_.size());
      parser_.reset(&mr);
      boost::system::error_code ec;
      parser_.write(line, len, ec);
      if (ec) { return; }
      parser_.finish(ec);
      if (ec) { return; }
      const bj::value v = parser_.release();
      run_query<bj_traits>(q_, &v, out);
    });
  }

private:
  std::vector<unsigned char> buf_;
  bj::stream_parser parser_;
  query_id q_;
};

} // namespace

extraction boost_json_run_serial(const char *data, size_t size, query_id q,
                                 size_t longest) {
  bj_worker w(longest, q);
  extraction out;
  w(data, size, out);
  return out;
}

extraction boost_json_run_parallel(const char *data, size_t size, query_id q,
                                   size_t threads, size_t slice_bytes,
                                   size_t longest) {
  return run_sliced(data, size, threads, slice_bytes, [&] {
    auto w = std::make_shared<bj_worker>(longest, q);
    return [w](const char *d, size_t n, extraction &out) { (*w)(d, n, out); };
  });
}

const char *boost_json_version_string() { return "boost.json"; }

} // namespace dom
} // namespace jsonbench
