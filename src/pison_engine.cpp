#include "pison_engine.h"

#include "BitmapConstructor.h"
#include "BitmapIterator.h"
#include "Records.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace jsonbench {
namespace pison {

const char *workload_name(workload w) {
  switch (w) {
  case workload::structure: return "structure";
  case workload::query: return "query";
  case workload::decode: return "decode";
  }
  return "?";
}

namespace {

// Pison's getValue() usually hands back a malloc'd copy of the raw value text,
// but on two paths (SerialBitmapIterator.cpp:277 and :283) it returns the
// string literal "" instead. Pison's own examples free the result
// unconditionally, which is undefined behaviour and aborts with
// "munmap_chunk(): invalid pointer" on these datasets. A malloc'd result always
// holds at least one character, so freeing exactly the non-empty results is
// both safe and complete.
struct owned_value {
  char *p;
  explicit owned_value(char *q) : p(q) {}
  ~owned_value() { if (p && p[0] != '\0') { free(p); } }
  owned_value(const owned_value &) = delete;
  owned_value &operator=(const owned_value &) = delete;
  std::string_view view() const {
    return p ? std::string_view(p) : std::string_view();
  }
};

// In `decode` mode we do to Pison's raw token what simdjson's decode mode does
// to its own: unescape a JSON string, or convert a number and re-render it. A
// structural index cannot produce these, so this code is ours, not Pison's --
// it exists so that both engines can be compared on equal output.
void feed_decoded(std::string_view raw, extraction &out) {
  // getValue returns the bytes up to the next structural character, so a value
  // that is not last in its object arrives with a trailing comma. Strip it
  // before testing for quotes, or every string falls through to the number
  // branch and is never unescaped.
  std::string_view s = extraction::trim_token(raw);
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    std::string decoded;
    decoded.reserve(s.size());
    for (size_t i = 1; i + 1 < s.size(); i++) {
      char c = s[i];
      if (c != '\\' || i + 2 >= s.size()) { decoded.push_back(c); continue; }
      char n = s[++i];
      switch (n) {
      case 'n': decoded.push_back('\n'); break;
      case 't': decoded.push_back('\t'); break;
      case 'r': decoded.push_back('\r'); break;
      case 'b': decoded.push_back('\b'); break;
      case 'f': decoded.push_back('\f'); break;
      case '/': decoded.push_back('/'); break;
      case '"': decoded.push_back('"'); break;
      case '\\': decoded.push_back('\\'); break;
      case 'u': {
        // Minimal \uXXXX handling: enough to match simdjson's output for the
        // BMP escapes these datasets contain.
        if (i + 4 < s.size()) {
          unsigned cp = 0;
          for (int k = 1; k <= 4; k++) {
            char h = s[i + size_t(k)];
            cp <<= 4;
            if (h >= '0' && h <= '9') { cp |= unsigned(h - '0'); }
            else if (h >= 'a' && h <= 'f') { cp |= unsigned(h - 'a' + 10); }
            else if (h >= 'A' && h <= 'F') { cp |= unsigned(h - 'A' + 10); }
          }
          i += 4;
          if (cp < 0x80) {
            decoded.push_back(char(cp));
          } else if (cp < 0x800) {
            decoded.push_back(char(0xC0 | (cp >> 6)));
            decoded.push_back(char(0x80 | (cp & 0x3F)));
          } else {
            decoded.push_back(char(0xE0 | (cp >> 12)));
            decoded.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            decoded.push_back(char(0x80 | (cp & 0x3F)));
          }
        }
        break;
      }
      default: decoded.push_back(n); break;
      }
    }
    out.feed(decoded);
    return;
  }
  // A number: convert and re-render exactly as the simdjson side does.
  char *endp = nullptr;
  std::string tmp(s);
  double d = strtod(tmp.c_str(), &endp);
  if (endp && endp != tmp.c_str()) {
    char buf[40];
    int n = std::snprintf(buf, sizeof(buf), "%.17g", d);
    out.feed(std::string_view(buf, size_t(n < 0 ? 0 : n)));
  } else {
    out.feed(s);
  }
}

inline void take(BitmapIterator *iter, extraction &out, workload w) {
  owned_value v(iter->getValue());
  if (!v.p) { return; }
  if (w == workload::decode) {
    feed_decoded(v.view(), out);
  } else {
    out.feed(v.view());
  }
}

// --- the six queries, mirroring simdjson_engine.cpp ------------------------

// $.user.lang, $.lang
void q_twitter(BitmapIterator *iter, extraction &out, workload w) {
  if (!iter->isObject()) { return; }
  std::unordered_set<char *> keys;
  keys.insert(const_cast<char *>("user"));
  keys.insert(const_cast<char *>("lang"));
  char *key = nullptr;
  while ((key = iter->moveToKey(keys)) != nullptr) {
    if (strcmp(key, "lang") == 0) {
      take(iter, out, w);
    } else {
      if (!iter->down()) { continue; }
      if (iter->isObject() && iter->moveToKey(const_cast<char *>("lang"))) {
        take(iter, out, w);
      }
      iter->up();
    }
  }
}

// $.categoryPath[1:3].id
void q_bestbuy(BitmapIterator *iter, extraction &out, workload w) {
  if (!(iter->isObject() && iter->moveToKey(const_cast<char *>("categoryPath")))) {
    return;
  }
  if (!iter->down()) { return; }
  if (iter->isArray()) {
    for (int idx = 1; idx <= 2; ++idx) {
      if (iter->moveToIndex(idx)) {
        if (!iter->down()) { continue; }
        if (iter->isObject() && iter->moveToKey(const_cast<char *>("id"))) {
          take(iter, out, w);
        }
        iter->up();
      }
    }
  }
  iter->up();
}

// $.routes[*].legs[*].steps[*].distance.text
void q_google(BitmapIterator *iter, extraction &out, workload w) {
  if (!(iter->isObject() && iter->moveToKey(const_cast<char *>("routes")))) {
    return;
  }
  if (!iter->down()) { return; }
  while (iter->isArray() && iter->moveNext()) {
    if (!iter->down()) { continue; }
    if (iter->isObject() && iter->moveToKey(const_cast<char *>("legs"))) {
      if (!iter->down()) { iter->up(); continue; }
      while (iter->isArray() && iter->moveNext()) {
        if (!iter->down()) { continue; }
        if (iter->isObject() && iter->moveToKey(const_cast<char *>("steps"))) {
          if (!iter->down()) { iter->up(); continue; }
          while (iter->isArray() && iter->moveNext()) {
            if (!iter->down()) { continue; }
            if (iter->isObject() &&
                iter->moveToKey(const_cast<char *>("distance"))) {
              if (iter->down()) {
                if (iter->isObject() &&
                    iter->moveToKey(const_cast<char *>("text"))) {
                  take(iter, out, w);
                }
                iter->up();
              }
            }
            iter->up();
          }
          iter->up();
        }
        iter->up();
      }
      iter->up();
    }
    iter->up();
  }
  iter->up();
}

// $[8], $[9]
void q_nspl(BitmapIterator *iter, extraction &out, workload w) {
  if (!iter->isArray()) { return; }
  for (int idx = 8; idx <= 9; ++idx) {
    if (iter->moveToIndex(idx)) { take(iter, out, w); }
  }
}

// $.bestMarketplacePrice.price, $.name
void q_walmart(BitmapIterator *iter, extraction &out, workload w) {
  if (!iter->isObject()) { return; }
  std::unordered_set<char *> keys;
  keys.insert(const_cast<char *>("bestMarketplacePrice"));
  keys.insert(const_cast<char *>("name"));
  char *key = nullptr;
  while ((key = iter->moveToKey(keys)) != nullptr) {
    if (strcmp(key, "name") == 0) {
      take(iter, out, w);
    } else {
      if (!iter->down()) { continue; }
      if (iter->isObject() && iter->moveToKey(const_cast<char *>("price"))) {
        take(iter, out, w);
      }
      iter->up();
    }
  }
}

// $.claims.P150[*].mainsnak.property
void q_wiki(BitmapIterator *iter, extraction &out, workload w) {
  if (!(iter->isObject() && iter->moveToKey(const_cast<char *>("claims")))) {
    return;
  }
  if (!iter->down()) { return; }
  if (iter->isObject() && iter->moveToKey(const_cast<char *>("P150"))) {
    if (iter->down()) {
      while (iter->isArray() && iter->moveNext()) {
        if (!iter->down()) { continue; }
        if (iter->isObject() &&
            iter->moveToKey(const_cast<char *>("mainsnak"))) {
          if (iter->down()) {
            if (iter->isObject() &&
                iter->moveToKey(const_cast<char *>("property"))) {
              take(iter, out, w);
            }
            iter->up();
          }
        }
        iter->up();
      }
      iter->up();
    }
  }
  iter->up();
}

// $.display_name, $.works_count
void q_openalex(BitmapIterator *iter, extraction &out, workload w) {
  if (!iter->isObject()) { return; }
  std::unordered_set<char *> keys;
  keys.insert(const_cast<char *>("display_name"));
  keys.insert(const_cast<char *>("works_count"));
  char *key = nullptr;
  while ((key = iter->moveToKey(keys)) != nullptr) {
    take(iter, out, w);
  }
}

void run_query(BitmapIterator *iter, extraction &out, query_id q, workload w) {
  switch (q) {
  case query_id::twitter: q_twitter(iter, out, w); break;
  case query_id::bestbuy: q_bestbuy(iter, out, w); break;
  case query_id::google_map: q_google(iter, out, w); break;
  case query_id::nspl: q_nspl(iter, out, w); break;
  case query_id::walmart: q_walmart(iter, out, w); break;
  case query_id::wiki: q_wiki(iter, out, w); break;
  case query_id::openalex: q_openalex(iter, out, w); break;
  }
}

// Index (and optionally query) one record. Returns its contribution.
void do_record(const char *text, uint64_t offset, size_t length, query_id q,
               workload w, int levels, int index_threads, extraction &out) {
  Record rec;
  rec.text = const_cast<char *>(text);
  rec.rec_start_pos = long(offset);
  rec.rec_length = long(length);
  rec.can_delete_text = false; // the buffer belongs to the caller

  Bitmap *bm = BitmapConstructor::construct(&rec, index_threads, levels);
  if (bm == nullptr) { return; }
  if (w == workload::structure) {
    out.matches++;
  } else {
    BitmapIterator *iter = BitmapConstructor::getIterator(bm);
    if (iter) {
      run_query(iter, out, q, w);
      delete iter;
    }
  }
  delete bm;
}

} // namespace

extraction run_stream(const char *text, const record_table &table, query_id q,
                      workload w, int levels, size_t threads,
                      std::vector<std::string> *trace, size_t trace_limit) {
  const size_t n = table.count();
  if (threads <= 1) {
    extraction total;
    total.trace = trace;
    total.trace_limit = trace_limit;
    for (size_t i = 0; i < n; i++) {
      do_record(text, table.offset[i], table.length[i], q, w, levels, 1, total);
    }
    return total;
  }

  std::vector<extraction> shards(threads);
  std::atomic<size_t> cursor{0};
  const size_t chunk = 64; // amortize the atomic without starving the tail
  std::vector<std::thread> pool;
  pool.reserve(threads);
  for (size_t t = 0; t < threads; t++) {
    pool.emplace_back([&, t] {
      extraction &local = shards[t];
      for (;;) {
        size_t begin = cursor.fetch_add(chunk, std::memory_order_relaxed);
        if (begin >= n) { break; }
        size_t end = begin + chunk < n ? begin + chunk : n;
        for (size_t i = begin; i < end; i++) {
          do_record(text, table.offset[i], table.length[i], q, w, levels, 1,
                    local);
        }
      }
    });
  }
  for (auto &th : pool) { th.join(); }
  extraction total;
  for (auto &s : shards) { total.merge(s); }
  return total;
}

extraction run_single_record(const char *text, size_t size, query_id q,
                             workload w, int levels, int threads) {
  extraction total;
  do_record(text, 0, size, q, w, levels, threads, total);
  return total;
}

} // namespace pison
} // namespace jsonbench
