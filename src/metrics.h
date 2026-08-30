// Measurement plumbing: wall clock, hardware performance counters, and
// process-wide CPU time.
//
// Two regimes, deliberately separated:
//
//   * Single-threaded phases are measured with lemire/counters, which opens
//     perf events for the calling thread. Retired instructions and cycles per
//     input byte are then properties of the algorithm, comparable across
//     machines. This is where the instruction-count analysis lives.
//
//   * Multi-threaded phases cannot use those counters: perf_event_open without
//     `inherit` sees only the calling thread, and both engines create their
//     workers internally. For those we report wall-clock throughput plus CPU
//     time from getrusage(), which does aggregate every thread. CPU seconds per
//     gigabyte is the resource-efficiency measure -- speed per core rather than
//     speed -- and it exposes work that parallelism wastes rather than saves.
#ifndef JSONBENCH_METRICS_H
#define JSONBENCH_METRICS_H

#include "counters/event_counter.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <sys/resource.h>
#include <vector>

namespace jsonbench {

inline double cpu_seconds() {
  struct rusage ru;
  if (getrusage(RUSAGE_SELF, &ru) != 0) { return 0.0; }
  return double(ru.ru_utime.tv_sec) + double(ru.ru_utime.tv_usec) * 1e-6 +
         double(ru.ru_stime.tv_sec) + double(ru.ru_stime.tv_usec) * 1e-6;
}

// One measured configuration.
struct measurement {
  double seconds = 0;      // wall clock, best of the repetitions
  double worst_seconds = 0; // wall clock of the slowest repetition
  double cpu_seconds = 0;  // CPU time of that same repetition, all threads
  double instructions = 0; // 0 when counters are unavailable or meaningless
  double cycles = 0;
  double branch_misses = 0;
  double cache_misses = 0;
  bool has_counters = false;

  // Spread between the slowest and fastest repetition, relative to the
  // fastest, in percent. Reporting only the best hides how repeatable a
  // configuration was, and repeatability varies enormously between machines.
  double spread_pct() const {
    if (seconds <= 0 || worst_seconds <= 0) { return 0; }
    return 100.0 * (worst_seconds - seconds) / seconds;
  }
};

// Run `fn` `reps` times and keep the fastest repetition, with per-thread
// hardware counters. Only meaningful when `fn` does its work on the calling
// thread.
template <typename Fn> measurement measure_single(int reps, Fn &&fn) {
  static counters::event_collector collector;
  measurement best;
  best.seconds = 1e30;
  const bool counters_ok = collector.has_events();
  for (int r = 0; r < reps; r++) {
    double cpu0 = cpu_seconds();
    collector.start();
    fn();
    counters::event_count c = collector.end();
    double cpu1 = cpu_seconds();
    if (c.elapsed_sec() > best.worst_seconds) { best.worst_seconds = c.elapsed_sec(); }
    if (c.elapsed_sec() < best.seconds) {
      best.seconds = c.elapsed_sec();
      best.cpu_seconds = cpu1 - cpu0;
      best.has_counters = counters_ok;
      if (counters_ok) {
        best.instructions = c.instructions();
        best.cycles = c.cycles();
        best.branch_misses = c.branch_misses();
        best.cache_misses = c.cache_misses();
      }
    }
  }
  return best;
}

// Run `fn` `reps` times and keep the fastest, with wall clock and CPU time
// only. Use for anything that spawns threads.
template <typename Fn> measurement measure_parallel(int reps, Fn &&fn) {
  measurement best;
  best.seconds = 1e30;
  for (int r = 0; r < reps; r++) {
    double cpu0 = cpu_seconds();
    auto t0 = std::chrono::steady_clock::now();
    fn();
    auto t1 = std::chrono::steady_clock::now();
    double cpu1 = cpu_seconds();
    double s = std::chrono::duration<double>(t1 - t0).count();
    if (s > best.worst_seconds) { best.worst_seconds = s; }
    if (s < best.seconds) {
      best.seconds = s;
      best.cpu_seconds = cpu1 - cpu0;
    }
  }
  return best;
}

} // namespace jsonbench

#endif
