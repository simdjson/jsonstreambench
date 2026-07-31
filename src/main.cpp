// jsonbench -- simdjson (incl. PR #2788 parse_many_parallel) vs Pison on a
// stream of JSON documents.
//
//   jsonbench --dataset twitter_small_records.json [options]
//
// Emits one `RESULT key=value ...` line per measured configuration, which
// bench/generate.py turns into the paper's tables and figures.
//
// Fairness rules, applied uniformly:
//
//   * Both engines start from the same in-memory buffer. File I/O is outside
//     every timed region.
//   * Pison's record-table pass is timed and reported, not skipped. It is
//     mandatory -- Pison cannot index anything without it -- and Pison's own
//     benchmarks, and cuJSON's reproduction of them, start the clock after it.
//     We report it with both Pison's loader and a fast parallel one, so the
//     published implementation is not held against the algorithm.
//   * Both engines answer the same query and must agree on the match count and
//     on a commutative hash of the extracted values.
//   * Pison is given the minimum number of bitmap levels its query needs.
#include "common.h"
#include "dom_engine.h"
#include "loaders.h"
#include "metrics.h"
#include "pison_engine.h"
#include "simdjson_engine.h"

#include "simdjson.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace jsonbench;

namespace {

struct options {
  std::string dataset;
  std::string label;
  std::string query_name;
  std::vector<size_t> threads;
  int reps = 3;
  size_t slice_kb = 1024;   // parse_many_parallel slice size
  size_t batch_mb = 16;     // iterate_many batch for the built-in 2-thread mode
  bool single_record = false;
  bool verify_only = false;
  size_t dump = 0;
  int levels = 0; // 0 = derive from the query
  // Which output sections to produce. Defaults to all of them; the sweep
  // scripts narrow this so a slice-size or thread-count study does not re-run
  // the loader and agreement work every time.
  std::string sections = "load,verify,single,scaling,e2e";
  // Restrict the run to one engine. Aggregate profilers (perf stat -a) cannot
  // attribute counters to an engine when several run in the same process, so
  // isolating one is the only way to ask "what is *this* engine waiting on".
  std::string only_engine;
  // Force a simdjson kernel (haswell, icelake, ...) instead of runtime dispatch.
  std::string impl;

  bool wants(const char *s) const {
    return sections.find(s) != std::string::npos;
  }
  bool wants_engine(const char *e) const {
    return only_engine.empty() || only_engine == e;
  }
};

void usage() {
  std::fprintf(stderr,
      "usage: jsonbench --dataset <file> [options]\n"
      "  --dataset <path>     JSON-lines file to benchmark (required)\n"
      "  --label <name>       short name for the output (default: filename stem)\n"
      "  --query <name>       twitter|bestbuy|google_map|nspl|walmart|wiki\n"
      "                       (default: inferred from the filename)\n"
      "  --threads a,b,c      thread counts to sweep (default: 1..hw, doubling)\n"
      "  --reps <n>           repetitions per configuration, best wins (default 3)\n"
      "  --slice-kb <n>       parse_many_parallel slice size (default 1024)\n"
      "  --batch-mb <n>       iterate_many batch size (default 16)\n"
      "  --single-record      treat the input as one bulky JSON document\n"
      "  --verify             check that the engines agree, then exit\n"
      "  --dump <n>           print the first n extracted values from each\n"
      "                       engine side by side, then exit\n"
      "  --sections <list>    comma list of load,verify,single,scaling,e2e\n"
      "                       (default: all)\n"
      "  --engine-only <name> run only this engine (e.g. simdjson-parallel,\n"
      "                       yyjson-parallel); for profiling one engine alone\n"
      "  --impl <name>        force a simdjson kernel (haswell, icelake, ...)\n");
}

bool parse_args(int argc, char **argv, options &o) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
    };
    if (a == "--dataset") { o.dataset = next(); }
    else if (a == "--label") { o.label = next(); }
    else if (a == "--query") { o.query_name = next(); }
    else if (a == "--reps") { o.reps = atoi(next().c_str()); }
    else if (a == "--slice-kb") { o.slice_kb = strtoull(next().c_str(), nullptr, 10); }
    else if (a == "--batch-mb") { o.batch_mb = strtoull(next().c_str(), nullptr, 10); }
    else if (a == "--single-record") { o.single_record = true; }
    else if (a == "--verify") { o.verify_only = true; }
    else if (a == "--dump") { o.dump = strtoull(next().c_str(), nullptr, 10); }
    else if (a == "--sections") { o.sections = next(); }
    else if (a == "--engine-only") { o.only_engine = next(); }
    else if (a == "--impl") { o.impl = next(); }
    else if (a == "--levels") { o.levels = atoi(next().c_str()); }
    else if (a == "--threads") {
      std::stringstream ss(next());
      std::string tok;
      while (std::getline(ss, tok, ',')) {
        if (!tok.empty()) { o.threads.push_back(strtoull(tok.c_str(), nullptr, 10)); }
      }
    } else if (a == "-h" || a == "--help") { usage(); exit(0); }
    else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); return false; }
  }
  return !o.dataset.empty();
}

bool read_file(const std::string &path, std::string &out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) { return false; }
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

std::string stem(const std::string &path) {
  size_t slash = path.find_last_of('/');
  std::string s = (slash == std::string::npos) ? path : path.substr(slash + 1);
  size_t dot = s.find_last_of('.');
  return (dot == std::string::npos) ? s : s.substr(0, dot);
}

// One RESULT line. Counter fields are emitted only when they were measured on
// the thread that did the work; per-byte normalization happens here so the
// paper's tables never have to know the file size.
void emit(const char *engine, const char *phase, const char *workload_name,
          size_t threads, const options &o, const std::string &label,
          size_t bytes, uint64_t docs, const measurement &s, const extraction &e) {
  double gb = double(bytes) / (1024.0 * 1024.0 * 1024.0);
  std::printf("RESULT dataset=%s engine=%s phase=%s workload=%s threads=%zu "
              "bytes=%zu docs=%llu seconds=%.6f gbps=%.4f mdocs=%.4f "
              "cpu_seconds=%.6f cpu_s_per_gb=%.4f matches=%llu hash=%llu",
              label.c_str(), engine, phase, workload_name, threads, bytes,
              (unsigned long long)docs, s.seconds, gb / s.seconds,
              double(docs) / 1e6 / s.seconds, s.cpu_seconds,
              s.cpu_seconds / (gb > 0 ? gb : 1.0),
              (unsigned long long)e.matches, (unsigned long long)e.sum);
  if (s.has_counters && s.instructions > 0) {
    std::printf(" instr_per_byte=%.4f cycles_per_byte=%.4f ipc=%.4f "
                "branchmiss_per_kb=%.4f cachemiss_per_kb=%.4f",
                s.instructions / double(bytes), s.cycles / double(bytes),
                s.cycles > 0 ? s.instructions / s.cycles : 0.0,
                s.branch_misses / (double(bytes) / 1024.0),
                s.cache_misses / (double(bytes) / 1024.0));
  }
  std::printf(" reps=%d\n", o.reps);
  std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv) {
  options o;
  if (!parse_args(argc, argv, o)) { usage(); return 1; }

  if (!o.impl.empty()) {
    auto wanted = simdjson::get_available_implementations()[o.impl];
    if (wanted == nullptr || !wanted->supported_by_runtime_system()) {
      std::fprintf(stderr, "simdjson implementation unavailable: %s\n",
                   o.impl.c_str());
      return 1;
    }
    simdjson::get_active_implementation() = wanted;
  }

  query_id q;
  if (!o.query_name.empty()) {
    if (!query_from_name(o.query_name, q)) {
      std::fprintf(stderr, "unknown query: %s\n", o.query_name.c_str());
      return 1;
    }
  } else if (!query_from_path(o.dataset, q)) {
    std::fprintf(stderr,
                 "cannot infer query from '%s'; pass --query\n",
                 o.dataset.c_str());
    return 1;
  }
  // Pison's level_num bounds how many bitmap levels it allocates. It must be at
  // least the query's depth, but also at least the *document's* depth: the
  // constructor only allocates mLevColonBitmap[0..level_num-1] while index
  // construction happily writes at the record's true nesting level, so a record
  // deeper than level_num dereferences a null bitmap pointer (SIGBUS). We
  // therefore derive a per-dataset value and allow an override.
  const int levels = o.levels > 0 ? o.levels : query_levels(q);
  const std::string label = o.label.empty() ? stem(o.dataset) : o.label;

  std::string raw;
  if (!read_file(o.dataset, raw)) {
    std::fprintf(stderr, "cannot read %s\n", o.dataset.c_str());
    return 1;
  }
  // simdjson reads a little past the end of the buffer; give it the padding it
  // documents. Pison reads in 64-byte blocks, so the same slack serves both.
  const size_t bytes = raw.size();
  raw.append(64 + 64, '\0');
  const char *data = raw.data();

  unsigned hw = std::thread::hardware_concurrency();
  if (hw == 0) { hw = 1; }
  if (o.threads.empty()) {
    for (size_t t = 1; t <= hw; t *= 2) { o.threads.push_back(t); }
    if (o.threads.back() != hw) { o.threads.push_back(hw); }
  }

  std::printf("# jsonbench dataset=%s query=%s levels=%d bytes=%zu "
              "simdjson_impl=%s hardware_threads=%u counters=%s\n",
              label.c_str(), query_name(q), levels, bytes,
              sj::implementation_name(), hw,
              counters::has_performance_counters() ? "yes" : "no");

  // -----------------------------------------------------------------------
  // The bulky-record scenario: Pison's own headline, where a stream parser
  // has nothing to offer.
  // -----------------------------------------------------------------------
  if (o.single_record) {
    // simdjson first: it cannot fail here, and Pison's parallel constructor can
    // (see below), so measuring it first keeps a crash from costing us the
    // whole comparison.
    auto ss = measure_single(o.reps, [&] {
      sj::run_serial(data, bytes, q, sj::workload::structure, false, bytes + 1);
    });
    extraction se =
        sj::run_serial(data, bytes, q, sj::workload::structure, false, bytes + 1);
    emit("simdjson", "index", "structure", 1, o, label, bytes, 1, ss, se);
    auto sq = measure_single(o.reps, [&] {
      sj::run_serial(data, bytes, q, sj::workload::query, false, bytes + 1);
    });
    extraction sqe =
        sj::run_serial(data, bytes, q, sj::workload::query, false, bytes + 1);
    emit("simdjson", "query", "query", 1, o, label, bytes, 1, sq, sqe);

    for (size_t t : o.threads) {
      // Pison's ParallelBitmapConstructor aborts with heap corruption at
      // exactly 2 threads on these inputs; skip that point rather than die.
      if (t == 2) { continue; }
      auto s = measure_parallel(o.reps, [&] {
        pison::run_single_record(data, bytes, q, pison::workload::structure,
                                 levels, int(t));
      });
      extraction e = pison::run_single_record(
          data, bytes, q, pison::workload::structure, levels, int(t));
      emit("pison", "index", "structure", t, o, label, bytes, 1, s, e);
    }
    return 0;
  }

  // -----------------------------------------------------------------------
  // Record table: the pass Pison must run before it can index anything.
  // -----------------------------------------------------------------------
  if (o.wants("load")) {
    // Pison's actual loader, unmodified. It only takes a path, so this figure
    // includes a page-cache read; every other timing in this program is
    // memory-to-memory.
    size_t n = 0;
    auto s = measure_single(o.reps, [&] { n = published_loader(o.dataset); });
    emit("pison", "load-published", "load", 1, o, label, bytes, n, s,
         extraction{0, n});
  }

  loaded_records ref_records; // same layout as Pison's, written competently
  auto load_ref = measure_single(o.reps, [&] {
    reference_loader(data, bytes, ref_records);
  });
  loaded_records fast_records; // the same layout, computed in parallel
  auto load_fast1 = measure_single(o.reps, [&] {
    fast_loader(data, bytes, 1, fast_records);
  });

  const uint64_t docs = fast_records.table.count();
  if (o.wants("load")) {
    emit("pison", "load-reference", "load", 1, o, label, bytes, docs, load_ref,
         extraction{0, docs});
    emit("pison", "load-fast", "load", 1, o, label, bytes, docs, load_fast1,
         extraction{0, docs});
    for (size_t t : o.threads) {
      if (t == 1) { continue; }
      auto s = measure_parallel(o.reps, [&] {
        loaded_records tmp;
        fast_loader(data, bytes, t, tmp);
      });
      emit("pison", "load-fast", "load", t, o, label, bytes, docs, s,
           extraction{0, docs});
    }
    // Boundary discovery alone: the lower bound on the pass, and what
    // simdjson's dispatcher does. Not a valid Pison input -- Pison needs the
    // padded copy -- so the gap to load-fast is what that copy costs.
    size_t n = 0;
    auto s1 = measure_single(o.reps, [&] { n = boundary_scan(data, bytes, 1); });
    emit("boundary-only", "load", "load", 1, o, label, bytes, n, s1,
         extraction{0, n});
    for (size_t t : o.threads) {
      if (t == 1) { continue; }
      auto s = measure_parallel(o.reps, [&] { boundary_scan(data, bytes, t); });
      emit("boundary-only", "load", "load", t, o, label, bytes, n, s,
           extraction{0, n});
    }
  }
  if (ref_records.table.count() != fast_records.table.count()) {
    std::fprintf(stderr,
                 "# WARNING: loaders disagree on record count (%zu vs %zu)\n",
                 ref_records.table.count(), fast_records.table.count());
  }

  // Everything below indexes the padded buffer the fast loader produced: the
  // configuration most favourable to Pison.
  const char *ptext = fast_records.text.data();
  const record_table &tbl = fast_records.table;

  // -----------------------------------------------------------------------
  // Correctness: both engines, same query, same answer.
  // -----------------------------------------------------------------------
  static const dom::library kDomLibraries[] = {
      dom::library::yyjson, dom::library::rapidjson,
      dom::library::boost_json, dom::library::nlohmann};
  // Sizes each DOM worker's arena. One pass over the input, taken once here so
  // that no timed region pays for it.
  const size_t dom_longest = dom::longest_document(data, bytes);
  bool agree = true;
  extraction pison_ref;
  if (o.wants("verify") || o.dump > 0) {
    pison_ref = pison::run_stream(ptext, tbl, q, pison::workload::query, levels, 1);
    extraction sj_ref =
        sj::run_serial(data, bytes, q, sj::workload::query, false, 1u << 20);
    agree = (pison_ref.matches == sj_ref.matches) &&
            (pison_ref.sum == sj_ref.sum);
    std::printf("# agreement query: pison matches=%llu hash=%llu | "
                "simdjson matches=%llu hash=%llu | %s\n",
                (unsigned long long)pison_ref.matches,
                (unsigned long long)pison_ref.sum,
                (unsigned long long)sj_ref.matches,
                (unsigned long long)sj_ref.sum,
                agree ? "AGREE" : (pison_ref.matches == sj_ref.matches
                                       ? "COUNT-ONLY" : "DISAGREE"));
    extraction pison_dec =
        pison::run_stream(ptext, tbl, q, pison::workload::decode, levels, 1);
    extraction sj_dec =
        sj::run_serial(data, bytes, q, sj::workload::decode, false, 1u << 20);
    std::printf("# agreement decode: pison matches=%llu hash=%llu | "
                "simdjson matches=%llu hash=%llu | %s\n",
                (unsigned long long)pison_dec.matches,
                (unsigned long long)pison_dec.sum,
                (unsigned long long)sj_dec.matches,
                (unsigned long long)sj_dec.sum,
                (pison_dec.matches == sj_dec.matches &&
                 pison_dec.sum == sj_dec.sum)
                    ? "AGREE"
                    : (pison_dec.matches == sj_dec.matches ? "COUNT-ONLY"
                                                           : "DISAGREE"));
    // Every DOM library must reproduce simdjson's decode result exactly, both
    // serially and under the slicing rule. The parallel check also proves the
    // slices abut: a dropped or duplicated document changes the match count.
    for (auto lib : kDomLibraries) {
      if (!dom::available(lib)) { continue; }
      extraction ser = dom::run_serial(lib, data, bytes, q, dom_longest);
      extraction par =
          dom::run_parallel(lib, data, bytes, q, 8, 256u << 10, dom_longest);
      bool ok = ser.matches == sj_dec.matches && ser.sum == sj_dec.sum;
      bool ok_par = par.matches == sj_dec.matches && par.sum == sj_dec.sum;
      if (!ok || !ok_par) { agree = false; }
      std::printf("# agreement decode: %s matches=%llu hash=%llu | %s"
                  " (parallel matches=%llu hash=%llu | %s)\n",
                  dom::library_name(lib), (unsigned long long)ser.matches,
                  (unsigned long long)ser.sum,
                  ok ? "AGREE" : (ser.matches == sj_dec.matches ? "COUNT-ONLY"
                                                                : "DISAGREE"),
                  (unsigned long long)par.matches,
                  (unsigned long long)par.sum,
                  ok_par ? "AGREE"
                         : (par.matches == sj_dec.matches ? "COUNT-ONLY"
                                                          : "DISAGREE"));
    }
  }
  if (o.dump > 0) {
    std::vector<std::string> pt, st;
    pison::run_stream(ptext, tbl, q, pison::workload::query, levels, 1, &pt,
                      o.dump);
    sj::run_serial(data, bytes, q, sj::workload::query, false, 1u << 20, &st,
                   o.dump);
    std::printf("# %-4s %-38s %-38s %s\n", "i", "pison", "simdjson", "same");
    for (size_t i = 0; i < o.dump; i++) {
      const char *a = i < pt.size() ? pt[i].c_str() : "<none>";
      const char *b = i < st.size() ? st[i].c_str() : "<none>";
      std::printf("# %-4zu %-38.38s %-38.38s %s\n", i, a, b,
                  (i < pt.size() && i < st.size() && pt[i] == st[i]) ? "yes"
                                                                    : "NO");
    }
    return 0;
  }
  if (o.verify_only) { return agree ? 0 : 2; }

  // -----------------------------------------------------------------------
  // Single-threaded: this is where the instruction-count analysis lives.
  // -----------------------------------------------------------------------
  struct named_pison { const char *phase; pison::workload w; };
  const named_pison pison_phases[] = {
      {"index", pison::workload::structure},
      {"query", pison::workload::query},
      {"decode", pison::workload::decode},
  };
  struct named_sj { const char *phase; sj::workload w; };
  const named_sj sj_phases[] = {
      {"index", sj::workload::structure},
      {"query", sj::workload::query},
      {"decode", sj::workload::decode},
  };

  if (o.wants("single")) {
    for (const auto &ph : pison_phases) {
      auto s = measure_single(o.reps, [&] {
        pison::run_stream(ptext, tbl, q, ph.w, levels, 1);
      });
      extraction e = pison::run_stream(ptext, tbl, q, ph.w, levels, 1);
      emit("pison", ph.phase, pison::workload_name(ph.w), 1, o, label, bytes,
           docs, s, e);
    }
    for (const auto &ph : sj_phases) {
      auto s = measure_single(o.reps, [&] {
        sj::run_serial(data, bytes, q, ph.w, false, 1u << 20);
      });
      extraction e = sj::run_serial(data, bytes, q, ph.w, false, 1u << 20);
      emit("simdjson", ph.phase, sj::workload_name(ph.w), 1, o, label, bytes,
           docs, s, e);
    }
    // simdjson's own two-thread stage1/stage2 pipeline, for reference.
    auto s = measure_parallel(o.reps, [&] {
      sj::run_serial(data, bytes, q, sj::workload::query, true,
                     o.batch_mb << 20);
    });
    extraction e = sj::run_serial(data, bytes, q, sj::workload::query, true,
                                  o.batch_mb << 20);
    emit("simdjson-builtin", "query", "query", 2, o, label, bytes, docs, s, e);

    // Conventional tree-building parsers. A DOM parse unescapes every string
    // and converts every number whether the query needs it or not, so these
    // are the analogue of simdjson's `decode` row, not of its `query` row.
    for (auto lib : kDomLibraries) {
      if (!dom::available(lib)) { continue; }
      auto ds = measure_single(o.reps, [&] {
        dom::run_serial(lib, data, bytes, q, dom_longest);
      });
      extraction de = dom::run_serial(lib, data, bytes, q, dom_longest);
      emit(dom::engine_name(lib, false), "decode", "decode", 1, o, label, bytes,
           docs, ds, de);
    }
  }

  // -----------------------------------------------------------------------
  // Thread scaling.
  // -----------------------------------------------------------------------
  for (size_t t : o.wants("scaling") ? o.threads : std::vector<size_t>{}) {
    if (o.wants_engine("pison-parallel"))
    for (const auto &ph : pison_phases) {
      auto s = measure_parallel(o.reps, [&] {
        pison::run_stream(ptext, tbl, q, ph.w, levels, t);
      });
      extraction e = pison::run_stream(ptext, tbl, q, ph.w, levels, t);
      emit("pison-parallel", ph.phase, pison::workload_name(ph.w), t, o, label,
           bytes, docs, s, e);
    }
    if (o.wants_engine("simdjson-parallel"))
    for (const auto &ph : sj_phases) {
      auto s = measure_parallel(o.reps, [&] {
        sj::run_parallel(data, bytes, q, ph.w, t, o.slice_kb << 10);
      });
      extraction e = sj::run_parallel(data, bytes, q, ph.w, t, o.slice_kb << 10);
      emit("simdjson-parallel", ph.phase, sj::workload_name(ph.w), t, o, label,
           bytes, docs, s, e);
    }
    // The same slicing rule, carrying a conventional DOM parser instead of the
    // on-demand one. Nothing in the decomposition knows what parses a document,
    // so this measures how much of our throughput comes from the slicing and
    // how much from on-demand parsing.
    for (auto lib : kDomLibraries) {
      if (!dom::available(lib)) { continue; }
      if (!o.wants_engine(dom::engine_name(lib, true))) { continue; }
      auto ds = measure_parallel(o.reps, [&] {
        dom::run_parallel(lib, data, bytes, q, t, o.slice_kb << 10, dom_longest);
      });
      extraction de = dom::run_parallel(lib, data, bytes, q, t,
                                        o.slice_kb << 10, dom_longest);
      emit(dom::engine_name(lib, true), "decode", "decode", t, o, label, bytes,
           docs, ds, de);
    }
  }

  // -----------------------------------------------------------------------
  // End to end: from an in-memory buffer to query results. For Pison that
  // includes the record table; for simdjson there is nothing to add.
  // -----------------------------------------------------------------------
  for (size_t t : o.wants("e2e") ? o.threads : std::vector<size_t>{}) {
    auto s_ref = measure_parallel(o.reps, [&] {
      loaded_records lr;
      reference_loader(data, bytes, lr);
      pison::run_stream(lr.text.data(), lr.table, q, pison::workload::query,
                        levels, t);
    });
    emit("pison-e2e-published", "e2e", "query", t, o, label, bytes, docs, s_ref,
         extraction{0, pison_ref.matches});

    auto s_fast = measure_parallel(o.reps, [&] {
      loaded_records lr;
      fast_loader(data, bytes, t, lr);
      pison::run_stream(lr.text.data(), lr.table, q, pison::workload::query,
                        levels, t);
    });
    emit("pison-e2e-fast", "e2e", "query", t, o, label, bytes, docs, s_fast,
         extraction{0, pison_ref.matches});
  }
  return 0;
}
