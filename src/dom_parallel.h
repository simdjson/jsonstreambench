// The paper's slicing rule, factored out so a DOM parser can be dropped into
// it. This is deliberately the same design as
// simdjson::experimental::parse_many_parallel: one atomic counter, each worker
// claims the next S bytes and snaps both ends forward to a line feed, so slices
// abut exactly and no document is split. Nothing here knows how a document is
// parsed.
//
// The only difference from the simdjson version is what happens inside a slice:
// there, one document_stream walks it; here, the caller's functor is handed the
// slice and iterates lines itself with its own parser and its own arena.
#ifndef JSONBENCH_DOM_PARALLEL_H
#define JSONBENCH_DOM_PARALLEL_H

#include "common.h"
#include "dom_queries.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <thread>
#include <vector>

namespace jsonbench {
namespace dom {

// Snap `want` forward to just past the next line feed, or to `hi`.
inline size_t snap_to_newline(const char *data, size_t hi, size_t want) {
  if (want >= hi) { return hi; }
  const char *nl =
      static_cast<const char *>(std::memchr(data + want, '\n', hi - want));
  return nl ? size_t(nl - data) + 1 : hi;
}

// `make_worker()` is called once per thread and returns a callable
// `void(const char *slice, size_t len, extraction &)`. Constructing per thread
// is what gives each worker its own parser and arena, so nothing is shared and
// no lock is taken on the hot path.
template <class MakeWorker>
extraction run_sliced(const char *data, size_t length, size_t threads,
                      size_t slice_bytes, MakeWorker make_worker) {
  if (threads == 0) { threads = 1; }
  if (slice_bytes == 0) { slice_bytes = 256 * 1024; }
  std::atomic<size_t> cursor{0};
  std::vector<extraction> shards(threads);
  std::vector<std::thread> workers;
  workers.reserve(threads);
  for (size_t i = 0; i < threads; i++) {
    workers.emplace_back([&, i] {
      auto parse_slice = make_worker();
      extraction &mine = shards[i];
      for (;;) {
        const size_t raw = cursor.fetch_add(slice_bytes,
                                            std::memory_order_relaxed);
        if (raw >= length) { break; }
        const size_t begin = (raw == 0) ? 0 : snap_to_newline(data, length, raw);
        const size_t end = snap_to_newline(data, length, raw + slice_bytes);
        // Two claims can land inside one long document; the earlier one covers
        // it and this slice is empty.
        if (begin >= end) { continue; }
        parse_slice(data + begin, end - begin, mine);
      }
    });
  }
  for (auto &w : workers) { w.join(); }
  extraction total;
  for (const auto &s : shards) { total.merge(s); }
  return total;
}

} // namespace dom
} // namespace jsonbench

#endif
