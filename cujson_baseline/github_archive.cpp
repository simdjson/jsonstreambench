// github_archive -- cuJSON's end-to-end case study (ASPLOS '26, Section 8.7,
// Figure 18) with simdjson: return the repository name of every PushEvent,
// $[?(@.type=="PushEvent")].repo.name, from the GitHub Archive events of
// January 1-2, 2015 (554,617 records, 289,117 matches).
//
//   github_archive --lines github_archive_small_records.json
//                  [--document github_archive_large_record.json]
//                  [--threads 1,2,4,...] [--assign static|dynamic] [--reps 10]
//
// The files are those of cuJSON's dataset folder. cuJSON reports 323.1 ms on
// an NVIDIA A100, transfers included. Here the input is already in memory, as
// in the rest of this benchmark, and each configuration reports the mean and
// best of --reps runs after one untimed run:
//
//   lines/serial     simdjson's iterate_many over the JSON lines, one thread
//   lines/threaded   the same, with simdjson's stage 1 of the next batch on a
//                    second thread (parser.threaded), two threads
//   lines/parallel   the slicing driver of src/parallel_stream.h, per thread
//                    count
//   document/serial  the same events as one JSON array (--document), one
//                    parser, one thread
//
// Every run must return the same number of names and the same hash of them.
#include "common.h"
#include "parallel_stream.h"

#include "simdjson.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using namespace simdjson;
using jsonbench::extraction;

namespace {

uint64_t fnv1a(std::string_view s) {
  uint64_t h = 14695981039346656037ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

// $[?(@.type=="PushEvent")].repo.name for one event. The names are unescaped,
// and summed as hashes so that the order in which threads finish is irrelevant.
template <typename Doc> error_code push_repo_name(Doc doc, extraction &out) {
  std::string_view type;
  if (auto e = doc["type"].get_string().get(type)) { return e; }
  if (type != "PushEvent") { return SUCCESS; }
  std::string_view name;
  if (auto e = doc["repo"]["name"].get_string().get(name)) { return e; }
  out.matches++;
  out.sum += fnv1a(name);
  return SUCCESS;
}

extraction lines_serial(ondemand::parser &parser, const padded_string &json,
                        bool threaded) {
  extraction total;
  parser.threaded = threaded;
  ondemand::document_stream stream;
  // One event is 2.7 MB, more than the default 1 MB batch; 16 MB is the
  // benchmark's default (--batch-mb).
  if (parser.iterate_many(json, 16u << 20).get(stream)) { return total; }
  size_t index = 0;
  for (auto doc : stream) {
    ondemand::document_reference ref;
    error_code e = doc.get(ref);
    if (!e) { e = push_repo_name(ref, total); }
    if (e) {
      std::fprintf(stderr, "lines/serial: event %zu: %s\n", index,
                   error_message(e));
      return extraction{};
    }
    index++;
  }
  return total;
}

extraction lines_parallel(const padded_string &json, size_t threads,
                          bool static_partition) {
  jsonbench::parallel::options options;
  options.threads = threads;
  options.static_partition = static_partition;
  options.slice_bytes = 256u << 10;
#if JSONBENCH_HAVE_NEWLINE_DELIMITED
  options.format = stream_format::newline_delimited;
#endif
  auto result = jsonbench::parallel::parse_many<extraction>(
      padded_string_view(json),
      [](auto doc, extraction &out) { return push_repo_name(doc, out); },
      options);
  extraction total;
  if (result.errors()) { return total; }
  for (const auto &shard : result.shards()) {
    for (const extraction &e : shard) { total.merge(e); }
  }
  return total;
}

extraction document_serial(ondemand::parser &parser, const padded_string &json) {
  extraction total;
  ondemand::document doc;
  ondemand::array events;
  if (parser.iterate(json).get(doc) || doc.get_array().get(events)) { return total; }
  for (auto event : events) {
    ondemand::value v;
    if (event.get(v) || push_repo_name(v, total)) { return extraction{}; }
  }
  return total;
}

struct timing {
  double mean_ms = 0, best_ms = 0;
  extraction result;
};

template <typename Fn> bool measure(int reps, timing &t, Fn &&fn) {
  using clock = std::chrono::steady_clock;
  std::vector<double> ms;
  for (int r = 0; r <= reps; r++) {
    auto start = clock::now();
    extraction e = fn();
    auto end = clock::now();
    if (e.matches == 0) {
      t.result = e;
      return false;
    }
    if (r == 0) {
      t.result = e;  // run 0 is untimed
      continue;
    }
    if (e.matches != t.result.matches || e.sum != t.result.sum) { return false; }
    ms.push_back(std::chrono::duration<double, std::milli>(end - start).count());
  }
  double sum = 0;
  for (double v : ms) { sum += v; }
  t.mean_ms = sum / ms.size();
  t.best_ms = *std::min_element(ms.begin(), ms.end());
  return true;
}

void emit(const char *input, const char *mode, size_t threads, size_t bytes,
          const timing &t) {
  std::printf("RESULT input=%s mode=%s threads=%zu bytes=%zu mean_ms=%.2f "
              "best_ms=%.2f gbps=%.2f gibps=%.2f matches=%llu hash=%llu\n",
              input, mode, threads, bytes, t.mean_ms, t.best_ms,
              bytes / (t.mean_ms * 1e6),
              bytes / (t.mean_ms * 1e-3) / double(1 << 30),
              (unsigned long long)t.result.matches,
              (unsigned long long)t.result.sum);
  std::fflush(stdout);
}

std::vector<size_t> parse_threads(const std::string &s) {
  std::vector<size_t> out;
  size_t start = 0;
  while (start < s.size()) {
    size_t comma = s.find(',', start);
    if (comma == std::string::npos) { comma = s.size(); }
    out.push_back(std::strtoull(s.substr(start, comma - start).c_str(), nullptr, 10));
    start = comma + 1;
  }
  return out;
}

[[noreturn]] void usage() {
  std::fprintf(stderr,
      "usage: github_archive --lines <file.json> [--document <file.json>]\n"
      "                      [--threads a,b,c] [--assign static|dynamic] [--reps n]\n");
  std::exit(2);
}

}  // namespace

int main(int argc, char **argv) {
  std::string lines_file, document_file;
  std::vector<size_t> threads;
  int reps = 10;
  bool static_partition = true;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) { usage(); }
      return argv[++i];
    };
    if (a == "--lines") { lines_file = value(); }
    else if (a == "--document") { document_file = value(); }
    else if (a == "--threads") { threads = parse_threads(value()); }
    else if (a == "--reps") { reps = std::atoi(value().c_str()); }
    else if (a == "--assign") {
      std::string v = value();
      if (v != "static" && v != "dynamic") { usage(); }
      static_partition = v == "static";
    }
    else { usage(); }
  }
  if (lines_file.empty() || reps < 1) { usage(); }
  if (threads.empty()) {
    for (size_t t = 1; t <= std::thread::hardware_concurrency(); t *= 2) { threads.push_back(t); }
  }

  padded_string lines;
  if (padded_string::load(lines_file).get(lines)) {
    std::fprintf(stderr, "cannot load %s\n", lines_file.c_str());
    return 1;
  }
  std::printf("# lines=%s bytes=%zu simdjson=%s kernel=%s reps=%d assign=%s\n",
              lines_file.c_str(), lines.size(), SIMDJSON_VERSION,
              get_active_implementation()->name().c_str(), reps,
              static_partition ? "static" : "dynamic");

  // The parallel driver is the measurement; every other configuration must
  // return the same names. cuJSON reports 289,117 matches.
  extraction reference;
  for (size_t n : threads) {
    timing p;
    if (!measure(reps, p, [&] { return lines_parallel(lines, n, static_partition); }) ||
        (reference.matches && (p.result.matches != reference.matches ||
                               p.result.sum != reference.sum))) {
      std::fprintf(stderr, "lines/parallel at %zu threads failed or disagreed "
                   "(matches=%llu)\n", n, (unsigned long long)p.result.matches);
      return 1;
    }
    if (!reference.matches) { reference = p.result; }
    emit("lines", "parallel", n, lines.size(), p);
  }

  ondemand::parser parser;
  for (bool threaded : {false, true}) {
    const char *mode = threaded ? "threaded" : "serial";
    timing t;
    if (!measure(reps, t, [&] { return lines_serial(parser, lines, threaded); }) ||
        t.result.matches != reference.matches || t.result.sum != reference.sum) {
      std::fprintf(stderr, "lines/%s failed or disagreed (matches=%llu)\n", mode,
                   (unsigned long long)t.result.matches);
    } else {
      emit("lines", mode, threaded ? 2 : 1, lines.size(), t);
    }
  }

  if (!document_file.empty()) {
    padded_string document;
    if (padded_string::load(document_file).get(document)) {
      std::fprintf(stderr, "cannot load %s\n", document_file.c_str());
      return 1;
    }
    ondemand::parser big;
    if (big.allocate(document.size())) { return 1; }
    timing d;
    if (!measure(reps, d, [&] { return document_serial(big, document); }) ||
        d.result.matches != reference.matches || d.result.sum != reference.sum) {
      std::fprintf(stderr, "document/serial failed or disagreed\n");
      return 1;
    }
    emit("document", "serial", 1, document.size(), d);
  }
  return 0;
}
