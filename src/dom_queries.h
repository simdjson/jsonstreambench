// The six benchmark queries, written once against a traits shim so that every
// tree-building parser (yyjson, RapidJSON, Boost.JSON, nlohmann/json) runs the
// same extraction rather than six hand-written walks apiece.
//
// A traits type T supplies:
//
//   using node = <handle to a value>;
//   static bool obj_get(node, std::string_view key, node &out);
//   static bool arr_at (node, size_t index, node &out);
//   template <class F> static void arr_each(node, F &&f);   // f(node)
//   static void feed(node, extraction &out);
//
// obj_get / arr_at return false when the path does not exist, which is the
// normal case on these datasets and must not be an error.
//
// Only the `decode` workload is meaningful here. A structural index can hand
// back the raw bytes of a value, and simdjson on demand can too, but a DOM
// parser has already unescaped every string and converted every number by the
// time we can look at it -- that work is not optional and cannot be skipped.
// So these engines are comparable to simdjson's `decode` numbers, not to its
// `query` numbers, and main.cpp emits them as such.
#ifndef JSONBENCH_DOM_QUERIES_H
#define JSONBENCH_DOM_QUERIES_H

#include "common.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace jsonbench {
namespace dom {

// ---------------------------------------------------------------------------
// Decoded-value formatting
// ---------------------------------------------------------------------------
// simdjson's decode path feeds the sink an unescaped string body, or a number
// rendered with "%.17g", or the literal token for true/false/null. Every DOM
// traits implementation funnels through these two helpers so all engines agree
// with it bit for bit; `--verify` checks that they do.
inline void feed_number(double d, extraction &out) {
  char buf[40];
  int n = std::snprintf(buf, sizeof(buf), "%.17g", d);
  out.feed(std::string_view(buf, size_t(n < 0 ? 0 : n)));
}

inline void feed_literal(const char *lit, extraction &out) {
  out.feed(std::string_view(lit, std::strlen(lit)));
}

// ---------------------------------------------------------------------------
// The six queries (see common.h for the JSONPath expressions)
// ---------------------------------------------------------------------------

// $.user.lang, $.lang
template <class T> void q_twitter(typename T::node doc, extraction &out) {
  typename T::node v;
  if (T::obj_get(doc, "lang", v)) { T::feed(v, out); }
  typename T::node user;
  if (T::obj_get(doc, "user", user) && T::obj_get(user, "lang", v)) {
    T::feed(v, out);
  }
}

// $.categoryPath[1:3].id -- elements 1 and 2
template <class T> void q_bestbuy(typename T::node doc, extraction &out) {
  typename T::node path;
  if (!T::obj_get(doc, "categoryPath", path)) { return; }
  for (size_t i = 1; i <= 2; i++) {
    typename T::node elem, id;
    if (T::arr_at(path, i, elem) && T::obj_get(elem, "id", id)) {
      T::feed(id, out);
    }
  }
}

// $.routes[*].legs[*].steps[*].distance.text
template <class T> void q_google(typename T::node doc, extraction &out) {
  typename T::node routes;
  if (!T::obj_get(doc, "routes", routes)) { return; }
  T::arr_each(routes, [&](typename T::node route) {
    typename T::node legs;
    if (!T::obj_get(route, "legs", legs)) { return; }
    T::arr_each(legs, [&](typename T::node leg) {
      typename T::node steps;
      if (!T::obj_get(leg, "steps", steps)) { return; }
      T::arr_each(steps, [&](typename T::node step) {
        typename T::node dist, text;
        if (T::obj_get(step, "distance", dist) &&
            T::obj_get(dist, "text", text)) {
          T::feed(text, out);
        }
      });
    });
  });
}

// $[8], $[9] -- NSPL data rows are positional arrays; see common.h.
template <class T> void q_nspl(typename T::node doc, extraction &out) {
  for (size_t i = 8; i <= 9; i++) {
    typename T::node cell;
    if (T::arr_at(doc, i, cell)) { T::feed(cell, out); }
  }
}

// $.bestMarketplacePrice.price, $.name
template <class T> void q_walmart(typename T::node doc, extraction &out) {
  typename T::node v;
  if (T::obj_get(doc, "name", v)) { T::feed(v, out); }
  typename T::node bmp;
  if (T::obj_get(doc, "bestMarketplacePrice", bmp) &&
      T::obj_get(bmp, "price", v)) {
    T::feed(v, out);
  }
}

// $.claims.P150[*].mainsnak.property
template <class T> void q_wiki(typename T::node doc, extraction &out) {
  typename T::node claims, p150;
  if (!T::obj_get(doc, "claims", claims)) { return; }
  if (!T::obj_get(claims, "P150", p150)) { return; }
  T::arr_each(p150, [&](typename T::node claim) {
    typename T::node snak, prop;
    if (T::obj_get(claim, "mainsnak", snak) &&
        T::obj_get(snak, "property", prop)) {
      T::feed(prop, out);
    }
  });
}

// $.display_name, $.works_count
template <class T> void q_openalex(typename T::node doc, extraction &out) {
  typename T::node v;
  if (T::obj_get(doc, "display_name", v)) { T::feed(v, out); }
  if (T::obj_get(doc, "works_count", v)) { T::feed(v, out); }
}

template <class T>
void run_query(query_id q, typename T::node doc, extraction &out) {
  switch (q) {
  case query_id::twitter:    q_twitter<T>(doc, out); return;
  case query_id::bestbuy:    q_bestbuy<T>(doc, out); return;
  case query_id::google_map: q_google<T>(doc, out);  return;
  case query_id::nspl:       q_nspl<T>(doc, out);    return;
  case query_id::walmart:    q_walmart<T>(doc, out); return;
  case query_id::wiki:       q_wiki<T>(doc, out);    return;
  case query_id::openalex:   q_openalex<T>(doc, out); return;
  }
}

// ---------------------------------------------------------------------------
// Line splitting
// ---------------------------------------------------------------------------
// None of these libraries has a JSON-lines reader, so every engine needs this.
// It is the same rule the parallel parser uses -- a line feed cannot occur
// inside a JSON value -- applied serially. `f(ptr, len)` gets one document,
// with trailing whitespace already removed.
template <class F>
void for_each_line(const char *data, size_t size, F &&f) {
  size_t pos = 0;
  while (pos < size) {
    const char *nl =
        static_cast<const char *>(std::memchr(data + pos, '\n', size - pos));
    size_t end = nl ? size_t(nl - data) : size;
    size_t stop = end;
    while (stop > pos && (data[stop - 1] == '\r' || data[stop - 1] == ' ')) {
      stop--;
    }
    if (stop > pos) { f(data + pos, stop - pos); }
    pos = nl ? end + 1 : size;
  }
}

// Longest line in the buffer: DOM parsers need an arena big enough for the
// biggest document, and sizing it once beats growing it per document.
inline size_t longest_line(const char *data, size_t size) {
  size_t best = 0;
  for_each_line(data, size, [&](const char *, size_t n) {
    if (n > best) { best = n; }
  });
  return best;
}

} // namespace dom
} // namespace jsonbench

#endif
