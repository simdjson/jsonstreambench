// cujson_baseline -- single-threaded simdjson on one bulky JSON document, the
// way the cuJSON paper (ASPLOS '26, Figures 9 and 10) measures its simdjson
// baseline, and the way it should be measured.
//
//   cujson_baseline --file twitter_large_record.json [options]
//
// cuJSON's harness (paper_reproduced/related_works/simdjson/
// quickstart-original-iterate.cpp in AutomataLab/cuJSON) loads the file, then
// for each of ten runs creates a new ondemand::parser and times only
// parser.iterate(json). Two things follow:
//
//   * iterate() runs stage 1 only (UTF-8 validation and the structural index).
//     Nothing is parsed, and the nesting is never checked. cuJSON, by contrast,
//     validates UTF-8, tokenizes, and matches every bracket (its pairing index).
//     The DOM parser does all of that, and more: it also builds a tape with a
//     jump pointer from each opening bracket to its closing one, and decodes
//     every string and number. --api dom is therefore the conservative
//     full-work counterpart of cuJSON's parse.
//   * A new parser allocates its buffers inside the timed region. For a 1 GB
//     document these are gigabytes of fresh memory, so the measurement is
//     dominated by page faults and by the kernel zeroing pages.
//
// The options separate these effects:
//
//   --api ondemand|dom|query|decode
//                           stage 1 only (cuJSON's measurement); a full DOM
//                           parse; or end to end: stage 1 plus the paper's
//                           query on every record of the dominating array, the
//                           values extracted raw (query) or decoded (decode)
//   --parser fresh|reuse    a new parser per run (timed allocation, as cuJSON
//                           does) or one parser allocated and warmed up once
//   --alloc  malloc|4k|huge|pool
//                           how the large buffers are obtained (see below)
//
// File I/O is outside every timed region, as in cuJSON's harness. Each
// configuration runs once untimed, then --reps timed runs. Each line reports
// the mean, median and best time, the throughput of the mean, and the minor
// page faults per run (getrusage).
#include "common.h"
#include "simdjson_engine.h"

#include "simdjson.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Large allocations. simdjson obtains every parser buffer with
// `new (std::nothrow) T[n]`, so replacing the global allocation functions
// controls them all. Requests below kLarge go to malloc in every mode. Large
// ones depend on --alloc:
//
//   malloc  the C library, unchanged (glibc maps and unmaps each large block,
//           so every run faults its buffers in afresh)
//   4k      a private mapping advised MADV_NOHUGEPAGE: one fault per 4 KiB page
//   huge    a 2 MiB-aligned mapping advised MADV_HUGEPAGE: one fault per 2 MiB,
//           but the kernel still zeroes every page it hands out
//   pool    as huge, but a freed block is kept and handed to the next request
//           that fits, as a long-running process's allocator would: after the
//           untimed run, a fresh parser costs no faults at all
// ---------------------------------------------------------------------------
namespace {

enum class alloc_mode { malloc_, small_pages, huge_pages, pool };
alloc_mode g_alloc = alloc_mode::malloc_;

constexpr size_t kLarge = size_t(1) << 20;
constexpr size_t kHuge = size_t(2) << 20;

struct block {
  void *base = nullptr;  // start of the mapping
  size_t map_size = 0;   // length of the mapping
  void *user = nullptr;  // pointer handed out (2 MiB-aligned within base)
  size_t capacity = 0;   // usable bytes from user
  bool in_use = false;
};
// A fixed table: the allocator must not allocate. A parser holds a handful of
// large buffers at a time.
block g_blocks[256];

block *find_block(void *p) {
  for (block &b : g_blocks) {
    if (b.base && b.user == p) { return &b; }
  }
  return nullptr;
}

void *map_block(size_t n) {
  // Over-allocate by 2 MiB so the user pointer can be 2 MiB-aligned, which
  // transparent huge pages need.
  size_t cap = (n + kHuge - 1) / kHuge * kHuge;
  size_t len = cap + kHuge;
  void *base = mmap(nullptr, len, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) { return nullptr; }
  uintptr_t aligned = (uintptr_t(base) + kHuge - 1) / kHuge * kHuge;
  madvise(reinterpret_cast<void *>(aligned), cap,
          g_alloc == alloc_mode::small_pages ? MADV_NOHUGEPAGE : MADV_HUGEPAGE);
  for (block &b : g_blocks) {
    if (!b.base) {
      b = {base, len, reinterpret_cast<void *>(aligned), cap, true};
      return b.user;
    }
  }
  munmap(base, len);
  return nullptr;
}

void *allocate(size_t n) {
  if (n < kLarge || g_alloc == alloc_mode::malloc_) { return std::malloc(n ? n : 1); }
  if (g_alloc == alloc_mode::pool) {
    // Best fit among the freed blocks.
    block *best = nullptr;
    for (block &b : g_blocks) {
      if (b.base && !b.in_use && b.capacity >= n &&
          (!best || b.capacity < best->capacity)) {
        best = &b;
      }
    }
    if (best) {
      best->in_use = true;
      return best->user;
    }
  }
  return map_block(n);
}

void release(void *p) {
  if (!p) { return; }
  block *b = find_block(p);
  if (!b) {
    std::free(p);
    return;
  }
  if (g_alloc == alloc_mode::pool) {
    b->in_use = false;
    return;
  }
  munmap(b->base, b->map_size);
  *b = block{};
}

}  // namespace

void *operator new(size_t n) {
  void *p = allocate(n);
  if (!p) { throw std::bad_alloc(); }
  return p;
}
void *operator new[](size_t n) { return operator new(n); }
void *operator new(size_t n, const std::nothrow_t &) noexcept { return allocate(n); }
void *operator new[](size_t n, const std::nothrow_t &) noexcept { return allocate(n); }
void operator delete(void *p) noexcept { release(p); }
void operator delete[](void *p) noexcept { release(p); }
void operator delete(void *p, size_t) noexcept { release(p); }
void operator delete[](void *p, size_t) noexcept { release(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { release(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { release(p); }

// ---------------------------------------------------------------------------

namespace {

struct options {
  std::string file;
  std::string label;
  std::vector<std::string> apis{"ondemand", "dom", "query", "decode"};
  std::vector<std::string> parsers{"fresh", "reuse"};
  std::vector<std::string> allocs{"malloc", "4k", "huge", "pool"};
  int reps = 10;
};

[[noreturn]] void usage() {
  std::fprintf(stderr,
      "usage: cujson_baseline --file <document.json> [options]\n"
      "  --label <name>          dataset label for the output (default: file name)\n"
      "  --api a,b               ondemand (stage 1 only), dom (full parse),\n"
      "                          query, decode (stage 1 + query on every record)\n"
      "  --parser a,b            fresh (new parser per run), reuse\n"
      "  --alloc a,b             malloc, 4k, huge, pool\n"
      "  --reps <n>              timed runs per configuration (default 10)\n"
      "Each list defaults to every value.\n");
  std::exit(2);
}

std::vector<std::string> split(const std::string &s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t comma = s.find(',', start);
    if (comma == std::string::npos) { comma = s.size(); }
    if (comma > start) { out.push_back(s.substr(start, comma - start)); }
    start = comma + 1;
  }
  return out;
}

alloc_mode parse_alloc(const std::string &s) {
  if (s == "malloc") { return alloc_mode::malloc_; }
  if (s == "4k") { return alloc_mode::small_pages; }
  if (s == "huge") { return alloc_mode::huge_pages; }
  if (s == "pool") { return alloc_mode::pool; }
  std::fprintf(stderr, "unknown --alloc %s\n", s.c_str());
  usage();
}

long minor_faults() {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_minflt;
}

struct timing {
  double mean_ms = 0, median_ms = 0, best_ms = 0;
  double faults = 0;
  jsonbench::extraction result;  // query and decode only
};

// One pass over the whole document; returns false on an error. `once` is
// called with the parser and fills `out` for the query workloads.
template <typename Parser, typename Fn>
bool run(const simdjson::padded_string &json, bool fresh, int reps, timing &t,
         Fn &&once) {
  using clock = std::chrono::steady_clock;
  std::vector<double> ms;
  long faults = 0;
  std::unique_ptr<Parser> kept;
  if (!fresh) {
    kept.reset(new Parser());
    if (kept->allocate(json.size()) != simdjson::SUCCESS) { return false; }
  }
  // Run 0 is untimed: it warms the caches and, under --alloc pool, the pool.
  for (int r = 0; r <= reps; r++) {
    std::unique_ptr<Parser> owned;
    long f0 = minor_faults();
    auto start = clock::now();
    if (fresh) { owned.reset(new Parser()); }
    jsonbench::extraction out;
    bool ok = once(fresh ? *owned : *kept, out);
    auto end = clock::now();
    long f1 = minor_faults();
    owned.reset();  // freeing a fresh parser is outside the timed region, as in cuJSON's harness
    if (!ok) { return false; }
    if (r == 0) {
      t.result = out;
    } else if (out.matches != t.result.matches || out.sum != t.result.sum) {
      return false;  // every run must extract the same values
    }
    if (r > 0) {
      ms.push_back(std::chrono::duration<double, std::milli>(end - start).count());
      faults += f1 - f0;
    }
  }
  double sum = 0;
  for (double v : ms) { sum += v; }
  t.mean_ms = sum / ms.size();
  std::sort(ms.begin(), ms.end());
  t.median_ms = ms[ms.size() / 2];
  t.best_ms = ms.front();
  t.faults = double(faults) / reps;
  return true;
}

std::string thp_setting() {
  FILE *f = std::fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
  if (!f) { return "unknown"; }
  char buf[128] = {0};
  if (!std::fgets(buf, sizeof(buf), f)) { buf[0] = 0; }
  std::fclose(f);
  std::string s(buf);
  size_t open = s.find('['), close = s.find(']');
  return open != std::string::npos && close > open ? s.substr(open + 1, close - open - 1) : "unknown";
}

}  // namespace

int main(int argc, char **argv) {
  options o;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) { usage(); }
      return argv[++i];
    };
    if (a == "--file") { o.file = value(); }
    else if (a == "--label") { o.label = value(); }
    else if (a == "--api") { o.apis = split(value()); }
    else if (a == "--parser") { o.parsers = split(value()); }
    else if (a == "--alloc") { o.allocs = split(value()); }
    else if (a == "--reps") { o.reps = std::atoi(value().c_str()); }
    else { usage(); }
  }
  if (o.file.empty() || o.reps < 1) { usage(); }
  if (o.label.empty()) {
    size_t slash = o.file.find_last_of('/');
    o.label = slash == std::string::npos ? o.file : o.file.substr(slash + 1);
  }
  jsonbench::query_id q{};
  const bool have_query = jsonbench::query_from_path(o.label, q) ||
                          jsonbench::query_from_path(o.file, q);

  simdjson::padded_string json;
  if (simdjson::padded_string::load(o.file).get(json)) {
    std::fprintf(stderr, "cannot load %s\n", o.file.c_str());
    return 1;
  }
  std::printf("# file=%s bytes=%zu simdjson=%s kernel=%s thp=%s reps=%d\n",
              o.file.c_str(), json.size(), SIMDJSON_VERSION,
              simdjson::get_active_implementation()->name().c_str(),
              thp_setting().c_str(), o.reps);

  for (const std::string &alloc : o.allocs) {
    g_alloc = parse_alloc(alloc);
    for (const std::string &api : o.apis) {
      for (const std::string &parser : o.parsers) {
        if (parser != "fresh" && parser != "reuse") { usage(); }
        bool fresh = parser == "fresh";
        timing t;
        bool ok;
        using jsonbench::extraction;
        namespace sj = jsonbench::sj;
        if (api == "ondemand") {
          ok = run<simdjson::ondemand::parser>(json, fresh, o.reps, t,
              [&](simdjson::ondemand::parser &p, extraction &) {
                return !p.iterate(json).error();
              });
        } else if (api == "dom") {
          ok = run<simdjson::dom::parser>(json, fresh, o.reps, t,
              [&](simdjson::dom::parser &p, extraction &) {
                return !p.parse(json).error();
              });
        } else if (api == "query" || api == "decode") {
          if (!have_query) {
            std::fprintf(stderr, "%s: no query for this dataset\n", o.label.c_str());
            return 1;
          }
          const sj::workload w = api == "query" ? sj::workload::query : sj::workload::decode;
          ok = run<simdjson::ondemand::parser>(json, fresh, o.reps, t,
              [&](simdjson::ondemand::parser &p, extraction &out) {
                out = sj::run_bulky_document(p, json, q, w);
                return out.matches > 0;
              });
        } else {
          usage();
        }
        if (!ok) {
          std::fprintf(stderr, "%s: parse failed (api=%s parser=%s alloc=%s)\n",
                       o.label.c_str(), api.c_str(), parser.c_str(), alloc.c_str());
          return 1;
        }
        std::printf("RESULT dataset=%s api=%s parser=%s alloc=%s bytes=%zu "
                    "mean_ms=%.2f median_ms=%.2f best_ms=%.2f gbps=%.2f gibps=%.2f "
                    "faults_per_run=%.0f matches=%llu hash=%llu\n",
                    o.label.c_str(), api.c_str(), parser.c_str(), alloc.c_str(),
                    json.size(), t.mean_ms, t.median_ms, t.best_ms,
                    json.size() / (t.mean_ms * 1e6),
                    json.size() / (t.mean_ms * 1e-3) / double(1 << 30), t.faults,
                    (unsigned long long)t.result.matches,
                    (unsigned long long)t.result.sum);
        std::fflush(stdout);
      }
    }
  }
  return 0;
}
