// Parse a stream of JSON documents across many threads, on top of simdjson's
// on-demand interface.
//
// This lived in simdjson as an experimental header (PR #2788); it belongs in
// user code instead. Nothing here needs to be inside the library -- it is a
// slicing rule plus a thread pool over the public `iterate_many` API -- and
// keeping it out means callers can adapt the decomposition to their own
// pipeline rather than accept ours.
//
// Design. Each worker claims a byte range from one atomic counter and snaps
// both ends forward to the next delimiter, so slices abut, never split a
// document, and are computed with no coordination beyond the counter. Each
// worker owns its parser and its output vector, so the hot path is lock-free.
// Results come back as one vector per worker ("shards"): values keep their
// order within a shard, but shards interleave with respect to the input.
//
// Formats. The slicing rule needs a delimiter that cannot occur inside a JSON
// value: a line feed for NDJSON, or a record separator (0x1E) for RFC 7464.
// Comma-delimited input is not supported, because a top-level comma can only be
// found by a serial structural scan.
//
// stream_format::newline_delimited, where available, additionally lets simdjson
// skip the tail of a partially read document without walking its structural
// characters.
#ifndef JSONBENCH_PARALLEL_STREAM_H
#define JSONBENCH_PARALLEL_STREAM_H

#include "simdjson.h"

#include <atomic>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

namespace jsonbench {
namespace parallel {

using simdjson::error_code;
using simdjson::stream_format;
using simdjson::SUCCESS;

struct options {
  size_t threads = 0;                 // 0 = hardware_concurrency()
  size_t slice_bytes = 256u << 10;
  stream_format format = stream_format::whitespace_delimited;
};

template <typename T> class result {
public:
  const std::vector<std::vector<T>> &shards() const noexcept { return _shards; }
  size_t size() const noexcept {
    size_t n = 0;
    for (const auto &s : _shards) { n += s.size(); }
    return n;
  }
  size_t errors() const noexcept { return _errors; }
  error_code first_error() const noexcept { return _first_error; }

  std::vector<std::vector<T>> _shards{};
  size_t _errors{0};
  error_code _first_error{SUCCESS};
};

namespace internal {

// Snap `want` forward to the next boundary in [0, hi). Depends only on `want`,
// which is what lets each worker compute its own slice: the end of one slice
// and the start of the next are the same call.
inline size_t snap(const char *data, size_t hi, size_t want,
                   stream_format format) {
  if (want >= hi) { return hi; }
  if (format == stream_format::json_sequence) {
    const void *rs = std::memchr(data + want, 0x1e, hi - want);
    return rs ? size_t(static_cast<const char *>(rs) - data) : hi;
  }
  const void *nl = std::memchr(data + want, '\n', hi - want);
  return nl ? size_t(static_cast<const char *>(nl) - data) + 1 : hi;
}

inline bool splittable(stream_format f) {
  return f == stream_format::whitespace_delimited ||
         f == stream_format::json_sequence
#if JSONBENCH_HAVE_NEWLINE_DELIMITED
         || f == stream_format::newline_delimited
#endif
      ;
}

} // namespace internal

// Extract a value of type T from every document in `json`.
//
// `extract(doc, out)` returns SUCCESS to keep `out`, or an error to skip the
// document. Errors are counted, not fatal.
template <typename T, typename F>
result<T> parse_many(simdjson::padded_string_view json, F &&extract,
                     options opt = {}) {
  result<T> out;
  const char *const data = json.data();
  const size_t length = json.size();

  size_t workers = opt.threads;
  if (workers == 0) {
    unsigned hw = std::thread::hardware_concurrency();
    workers = hw > 1 ? size_t(hw) : 1;
  }
  if (!internal::splittable(opt.format)) { workers = 1; }
  const size_t slice_bytes = opt.slice_bytes ? opt.slice_bytes : (256u << 10);

  out._shards.resize(workers);
  std::vector<size_t> errors(workers, 0);
  std::vector<error_code> first(workers, SUCCESS);
  std::atomic<size_t> cursor{0};

  std::vector<std::thread> pool;
  pool.reserve(workers);
  for (size_t w = 0; w < workers; w++) {
    pool.emplace_back([&, w] {
      simdjson::ondemand::parser parser;
      parser.threaded = false;
      std::vector<T> &shard = out._shards[w];

      for (;;) {
        const size_t raw = cursor.fetch_add(slice_bytes,
                                            std::memory_order_relaxed);
        if (raw >= length) { break; }
        const size_t begin =
            raw == 0 ? 0 : internal::snap(data, length, raw, opt.format);
        const size_t end =
            internal::snap(data, length, raw + slice_bytes, opt.format);
        if (begin >= end) { continue; } // a long document covered by an earlier claim

        simdjson::ondemand::document_stream stream;
        if (auto e = parser
                         .iterate_many(data + begin, end - begin, end - begin,
                                       opt.format)
                         .get(stream)) {
          errors[w]++;
          if (!first[w]) { first[w] = e; }
          continue;
        }
        for (auto it = stream.begin(); it != stream.end(); ++it) {
          auto doc = *it;
          T value;
          if (auto e = extract(doc, value)) {
            errors[w]++;
            if (!first[w]) { first[w] = e; }
            continue;
          }
          shard.push_back(std::move(value));
        }
      }
    });
  }
  for (auto &t : pool) { t.join(); }

  for (size_t w = 0; w < workers; w++) {
    out._errors += errors[w];
    if (!out._first_error) { out._first_error = first[w]; }
  }
  return out;
}

} // namespace parallel
} // namespace jsonbench

#endif
