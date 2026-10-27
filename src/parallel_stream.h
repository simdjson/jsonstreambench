// Parse a stream of JSON documents across many threads, on top of simdjson's
// on-demand interface.
//
// This lived in simdjson as an experimental header (PR #2788); it belongs in
// user code instead. Nothing here needs to be inside the library -- it is a
// slicing rule plus a thread pool over the public `iterate_many` API -- and
// keeping it out means callers can adapt the decomposition to their own
// pipeline rather than accept ours.
//
// Design. Each worker claims a slice index from one atomic counter and calls
// simdjson::slice_at, which snaps both ends to a delimiter so slices abut and
// never split a document. Each
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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sched.h>
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
  size_t slice_bytes = 64u << 10;
  stream_format format = stream_format::whitespace_delimited;
  // Each worker owns one contiguous run of slices. The alternative, claiming
  // the next free slice from a shared counter, scatters a worker's regions
  // across the input and costs up to 1.55x; it is kept only for reproduction.
  bool static_partition = true;
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

inline bool splittable(stream_format f) {
  return f == stream_format::whitespace_delimited ||
         f == stream_format::json_sequence
#if JSONBENCH_HAVE_NEWLINE_DELIMITED
         || f == stream_format::newline_delimited
#endif
      ;
}

inline char delimiter_for(stream_format f) {
  return f == stream_format::json_sequence ? char(0x1e) : '\n';
}

#if JSONBENCH_HAVE_NEWLINE_DELIMITED
using simdjson::slice_at;
#else
// simdjson::slice_at is not in released simdjson yet; same contract.
inline simdjson::padded_string_view slice_at(simdjson::padded_string_view data,
                                             char delimiter, size_t block_size,
                                             size_t index) noexcept {
  if (block_size == 0 || index > data.size() / block_size) { return {}; }
  const size_t raw_begin = index * block_size;
  if (raw_begin >= data.size()) { return {}; }
  auto snap = [&](size_t want) -> size_t {
    if (want >= data.size()) { return data.size(); }
    const void *p = std::memchr(data.data() + want, delimiter, data.size() - want);
    return p ? size_t(static_cast<const char *>(p) - data.data()) + 1 : data.size();
  };
  const size_t begin = (raw_begin == 0) ? 0 : snap(raw_begin);
  const size_t end = snap(raw_begin + block_size);
  if (begin >= end) { return {}; }
  return simdjson::padded_string_view(data.data() + begin, end - begin,
                                      data.capacity() - begin);
}
#endif

} // namespace internal

// Extract a value of type T from every document in `json`.
//
// `extract(doc, out)` returns SUCCESS to keep `out`, or an error to skip the
// document. Errors are counted, not fatal.
template <typename T, typename F>
result<T> parse_many(simdjson::padded_string_view json, F &&extract,
                     options opt = {}) {
  result<T> out;
  const size_t length = json.size();
  const char delimiter = internal::delimiter_for(opt.format);

  size_t workers = opt.threads;
  if (workers == 0) {
    unsigned hw = std::thread::hardware_concurrency();
    workers = hw > 1 ? size_t(hw) : 1;
  }
  if (!internal::splittable(opt.format)) { workers = 1; }
  const size_t slice_bytes = opt.slice_bytes ? opt.slice_bytes : (256u << 10);

  // skew-probe: JSONBENCH_CHUNK=k claims k adjacent slices per fetch_add;
  // JSONBENCH_TRACE=1 prints each worker's bytes, slices, cpu and finish time.
  static const size_t chunk = [] {
    const char *e = std::getenv("JSONBENCH_CHUNK");
    return e ? size_t(std::strtoull(e, nullptr, 10)) : size_t(1);
  }();
  static const bool trace = std::getenv("JSONBENCH_TRACE") != nullptr;
  static size_t call = 0;
  call++;
  struct worker_trace { size_t bytes = 0, slices = 0; long long start_ns = 0, end_ns = 0;
                        int cpu0 = -1, cpu1 = -1; };
  std::vector<worker_trace> wt(workers);
  const auto t0 = std::chrono::steady_clock::now();
  auto since = [&] { return (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now() - t0).count(); };

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
      worker_trace &tr = wt[w];
      tr.start_ns = since();
      tr.cpu0 = sched_getcpu();
      size_t claimed = 0, claim_end = 0;

      const size_t slices = (length + slice_bytes - 1) / slice_bytes;
      size_t next = slices * w / workers;
      const size_t last = slices * (w + 1) / workers;

      for (;;) {
        size_t i;
        if (opt.static_partition) {
          if (next >= last) { break; }
          i = next++;
        } else {
          if (claimed == claim_end) {
            claimed = cursor.fetch_add(chunk, std::memory_order_relaxed);
            claim_end = claimed + chunk;
          }
          i = claimed++;
        }
        if (i * slice_bytes >= length) { break; }
        auto piece = internal::slice_at(json, delimiter, slice_bytes, i);
        if (piece.empty()) { continue; }
        tr.bytes += piece.size();
        tr.slices++;

        simdjson::ondemand::document_stream stream;
        if (auto e = parser.iterate_many(piece, piece.size(), opt.format)
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
      tr.end_ns = since();
      tr.cpu1 = sched_getcpu();
    });
  }
  for (auto &t : pool) { t.join(); }
  if (trace) {
    for (size_t w = 0; w < workers; w++) {
      std::fprintf(stderr, "TRACE call=%zu threads=%zu assign=%s chunk=%zu slice=%zu w=%zu "
                   "cpu0=%d cpu1=%d bytes=%zu slices=%zu start_ns=%lld end_ns=%lld\n",
                   call, workers, opt.static_partition ? "static" : "dynamic",
                   opt.static_partition ? size_t(0) : chunk, slice_bytes, w,
                   wt[w].cpu0, wt[w].cpu1, wt[w].bytes, wt[w].slices,
                   wt[w].start_ns, wt[w].end_ns);
    }
  }

  for (size_t w = 0; w < workers; w++) {
    out._errors += errors[w];
    if (!out._first_error) { out._first_error = first[w]; }
  }
  return out;
}

} // namespace parallel
} // namespace jsonbench

#endif
