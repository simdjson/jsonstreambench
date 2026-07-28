// Building the record table that Pison needs before it can index anything.
//
// Pison's BitmapConstructor consumes a Record{text, rec_start_pos, rec_length},
// so a stream of N documents must first be turned into a table of N offsets.
// Pison's published benchmarks -- and cuJSON's reproduction of them -- start
// their timer *after* this pass, which is why we measure it explicitly.
//
// The layout is not negotiable. SerialBitmap::setRecordLength derives
// mNumTmpWords = length/32 for its scan loop but sizes every bitmap
// mNumWords = length/64, and the scan writes word j/2. A record whose length is
// not a multiple of 64 therefore writes one word past the end of every bitmap:
// in practice a segfault, in general silent heap corruption. Pison's own loader
// pads each record with 'd' characters up to a 64-byte multiple for exactly
// this reason. So a Pison front end must *copy* the input into a padded,
// concatenated buffer; it cannot index the stream in place. simdjson has no
// such requirement, and that difference is structural rather than incidental.
//
// We therefore measure four points, which separate what the algorithm costs
// from what the implementation costs:
//
//   published_loader : Pison's actual RecordLoader::loadRecords, unmodified.
//                      This is the pass its published numbers exclude. It only
//                      accepts a path, so the figure includes a page-cache
//                      read; every other timing here is memory to memory.
//
//   reference_loader : the same required layout -- split, pad to 64 bytes,
//                      concatenate -- written competently and serially. The
//                      gap to published_loader is pure implementation waste
//                      (fgets, five strlen calls per record, a growing
//                      std::string, one Record allocation per line).
//
//   fast_loader      : the same required layout, computed in parallel: scan for
//                      boundaries, prefix-sum the padded lengths, then copy and
//                      pad each record into its slot concurrently. This is the
//                      steelman -- the best a Pison front end can do -- and it
//                      is what every Pison measurement in this benchmark uses.
//
//   boundary_scan    : boundaries only, no copy and no padding. This is a lower
//                      bound on any loader, and it is what simdjson's
//                      dispatcher actually does (one memchr per slice, inside
//                      the parallel region). It is *not* a valid Pison input;
//                      we report it to show what the mandatory copy costs.
#ifndef JSONBENCH_LOADERS_H
#define JSONBENCH_LOADERS_H

#include "common.h"

#include "RecordLoader.h"
#include "Records.h"

#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace jsonbench {

// A 64-byte-aligned byte buffer. Pison allocates its record text with
// posix_memalign(&p, 64, ...) and its index construction issues aligned vector
// loads against it, so handing it a std::string (16-byte aligned in practice,
// and only incidentally more) raises SIGBUS on some inputs. Honouring the
// alignment is part of using Pison correctly, not a tuning choice.
class aligned_buffer {
public:
  aligned_buffer() = default;
  ~aligned_buffer() { reset(); }
  aligned_buffer(const aligned_buffer &) = delete;
  aligned_buffer &operator=(const aligned_buffer &) = delete;

  void reset() {
    if (ptr_) { free(ptr_); ptr_ = nullptr; }
    size_ = 0;
  }
  void resize(size_t n) {
    reset();
    if (n == 0) { return; }
    void *p = nullptr;
    size_t rounded = (n + 63) & ~size_t(63);
    if (posix_memalign(&p, 64, rounded) != 0) { p = nullptr; rounded = 0; }
    ptr_ = static_cast<char *>(p);
    size_ = rounded;
  }
  char *data() { return ptr_; }
  const char *data() const { return ptr_; }
  size_t size() const { return size_; }

private:
  char *ptr_ = nullptr;
  size_t size_ = 0;
};

// The text a loader hands to Pison, plus its record table.
struct loaded_records {
  aligned_buffer text; // records concatenated, each padded to a 64-byte multiple
  record_table table;
};

namespace detail {

constexpr size_t kMinRecordBytes = 5;

// Split [0,size) into `threads` ranges that begin just after a newline.
inline std::vector<size_t> chunk_bounds(const char *data, size_t size,
                                        size_t threads) {
  std::vector<size_t> bounds(threads + 1);
  bounds[0] = 0;
  bounds[threads] = size;
  for (size_t t = 1; t < threads; t++) {
    size_t guess = size / threads * t;
    if (guess >= size) { guess = size; }
    const char *nl =
        static_cast<const char *>(memchr(data + guess, '\n', size - guess));
    bounds[t] = nl ? size_t(nl - data) + 1 : size;
  }
  for (size_t t = 1; t <= threads; t++) {
    if (bounds[t] < bounds[t - 1]) { bounds[t] = bounds[t - 1]; }
  }
  return bounds;
}

// Record boundaries within [begin,stop): offsets into `data` and the length of
// each record with its line terminator trimmed.
inline void scan_range(const char *data, size_t begin, size_t stop,
                       std::vector<uint64_t> &offs,
                       std::vector<uint32_t> &lens) {
  size_t pos = begin;
  offs.reserve((stop - begin) / 512 + 8);
  lens.reserve((stop - begin) / 512 + 8);
  while (pos < stop) {
    const char *nl =
        static_cast<const char *>(memchr(data + pos, '\n', stop - pos));
    size_t end = nl ? size_t(nl - data) + 1 : stop;
    size_t trimmed = end - pos;
    while (trimmed > 0 && (data[pos + trimmed - 1] == '\n' ||
                           data[pos + trimmed - 1] == '\r')) {
      trimmed--;
    }
    if (trimmed > kMinRecordBytes) {
      offs.push_back(pos);
      lens.push_back(uint32_t(trimmed));
    }
    pos = end;
  }
}

// Pison rounds every record up to a 64-byte multiple. Its loader appends at
// least one padding byte even when the length already divides 64 (it computes
// 64 - len % 64, which is 64 rather than 0); we reproduce that so our layout
// matches its own exactly.
inline size_t padded_length(size_t len) { return len + (64 - len % 64); }

// Run `body(t)` for t in [0,threads), inline when threads == 1 so that
// single-threaded measurements stay on the thread the counters are attached to.
template <typename Body> void parallel_for(size_t threads, Body &&body) {
  if (threads <= 1) {
    body(size_t(0));
    return;
  }
  std::vector<std::thread> pool;
  pool.reserve(threads);
  for (size_t t = 0; t < threads; t++) { pool.emplace_back(body, t); }
  for (auto &th : pool) { th.join(); }
}

} // namespace detail

// ---------------------------------------------------------------------------
// Pison's own loader, called unmodified.
// ---------------------------------------------------------------------------
inline size_t published_loader(const std::string &path) {
  RecordSet *rs = RecordLoader::loadRecords(const_cast<char *>(path.c_str()));
  if (rs == nullptr) { return 0; }
  size_t n = size_t(rs->size());
  delete rs;
  return n;
}

// ---------------------------------------------------------------------------
// Boundaries only: the lower bound, and what simdjson's dispatcher does.
// ---------------------------------------------------------------------------
inline size_t boundary_scan(const char *data, size_t size, size_t threads) {
  if (threads < 1) { threads = 1; }
  auto bounds = detail::chunk_bounds(data, size, threads);
  std::vector<std::vector<uint64_t>> offs(threads);
  std::vector<std::vector<uint32_t>> lens(threads);
  detail::parallel_for(threads, [&](size_t t) {
    detail::scan_range(data, bounds[t], bounds[t + 1], offs[t], lens[t]);
  });
  size_t total = 0;
  for (auto &o : offs) { total += o.size(); }
  return total;
}

// ---------------------------------------------------------------------------
// Pison's required layout, serial, written competently.
// ---------------------------------------------------------------------------
inline void reference_loader(const char *data, size_t size,
                             loaded_records &out) {
  out.text.reset();
  out.table.offset.clear();
  out.table.length.clear();

  std::vector<uint64_t> offs;
  std::vector<uint32_t> lens;
  detail::scan_range(data, 0, size, offs, lens);

  size_t total = 0;
  for (uint32_t l : lens) { total += detail::padded_length(l); }
  out.text.resize(total + 64);
  char *dst = out.text.data();

  size_t cursor = 0;
  out.table.offset.reserve(offs.size());
  out.table.length.reserve(offs.size());
  for (size_t i = 0; i < offs.size(); i++) {
    size_t padded = detail::padded_length(lens[i]);
    memcpy(dst + cursor, data + offs[i], lens[i]);
    memset(dst + cursor + lens[i], 'd', padded - lens[i]);
    out.table.offset.push_back(cursor);
    out.table.length.push_back(uint32_t(padded));
    cursor += padded;
  }
}

// ---------------------------------------------------------------------------
// Pison's required layout, in parallel: the steelman.
// ---------------------------------------------------------------------------
inline void fast_loader(const char *data, size_t size, size_t threads,
                        loaded_records &out) {
  if (threads < 1) { threads = 1; }
  out.text.reset();
  out.table.offset.clear();
  out.table.length.clear();

  auto bounds = detail::chunk_bounds(data, size, threads);
  std::vector<std::vector<uint64_t>> offs(threads);
  std::vector<std::vector<uint32_t>> lens(threads);
  detail::parallel_for(threads, [&](size_t t) {
    detail::scan_range(data, bounds[t], bounds[t + 1], offs[t], lens[t]);
  });

  // Prefix-sum the padded sizes so each chunk knows where its records land.
  std::vector<size_t> chunk_start(threads + 1, 0);
  for (size_t t = 0; t < threads; t++) {
    size_t bytes = 0;
    for (uint32_t l : lens[t]) { bytes += detail::padded_length(l); }
    chunk_start[t + 1] = chunk_start[t] + bytes;
  }
  std::vector<size_t> index_start(threads + 1, 0);
  for (size_t t = 0; t < threads; t++) {
    index_start[t + 1] = index_start[t] + offs[t].size();
  }
  const size_t records = index_start[threads];
  out.text.resize(chunk_start[threads] + 64);
  out.table.offset.resize(records);
  out.table.length.resize(records);
  char *dst = out.text.data();

  detail::parallel_for(threads, [&](size_t t) {
    size_t cursor = chunk_start[t];
    size_t idx = index_start[t];
    for (size_t i = 0; i < offs[t].size(); i++) {
      size_t len = lens[t][i];
      size_t padded = detail::padded_length(len);
      memcpy(dst + cursor, data + offs[t][i], len);
      memset(dst + cursor + len, 'd', padded - len);
      out.table.offset[idx] = cursor;
      out.table.length[idx] = uint32_t(padded);
      cursor += padded;
      idx++;
    }
  });
}

} // namespace jsonbench

#endif
