#include "simdjson_engine.h"

#include "parallel_stream.h"
#include "simdjson.h"

#include <string>

using namespace simdjson;

namespace jsonbench {
namespace sj {

const char *workload_name(workload w) {
  switch (w) {
  case workload::structure: return "structure";
  case workload::query: return "query";
  case workload::decode: return "decode";
  }
  return "?";
}

const char *implementation_name() {
  static std::string name = get_active_implementation()->name();
  return name.c_str();
}

namespace {

// ---------------------------------------------------------------------------
// Value extraction
// ---------------------------------------------------------------------------
// In `query` mode we take the raw JSON token, which is exactly what Pison's
// BitmapIterator::getValue returns: the bytes as they appear in the input, no
// unescaping, no number conversion. That keeps the comparison honest.
//
// In `decode` mode we additionally unescape strings and convert numbers, which
// is what an application actually consumes. A structural index cannot do this
// at all, so the difference between the two modes is the work Pison's numbers
// leave on the table.
inline void take(ondemand::value v, extraction &out, workload w) {
  if (w == workload::decode) {
    ondemand::json_type t;
    if (v.type().get(t)) { return; }
    if (t == ondemand::json_type::string) {
      std::string_view s;
      if (!v.get_string().get(s)) { out.feed(s); }
      return;
    }
    if (t == ondemand::json_type::number) {
      double d;
      if (!v.get_double().get(d)) {
        // Feed the decoded value's bytes so the sink still sees real work.
        char buf[40];
        int n = std::snprintf(buf, sizeof(buf), "%.17g", d);
        out.feed(std::string_view(buf, size_t(n < 0 ? 0 : n)));
      }
      return;
    }
  }
  std::string_view raw = v.raw_json_token();
  out.feed(raw);
}

// ---------------------------------------------------------------------------
// The six queries
// ---------------------------------------------------------------------------
// Written against the JSONPath expressions of the Pison paper (Table 1),
// applied per record.

// $.user.lang, $.lang
template <typename Doc> error_code q_twitter(Doc doc, extraction &out, workload w) {
  ondemand::object root;
  if (auto e = doc.get_object().get(root)) { return e; }
  for (auto field : root) {
    std::string_view key;
    if (auto e = field.unescaped_key().get(key)) { return e; }
    if (key == "lang") {
      take(field.value(), out, w);
    } else if (key == "user") {
      ondemand::object user;
      if (!field.value().get_object().get(user)) {
        ondemand::value lang;
        if (!user["lang"].get(lang)) { take(lang, out, w); }
      }
    }
  }
  return SUCCESS;
}

// $.categoryPath[1:3].id  -- elements 1 and 2
template <typename Doc> error_code q_bestbuy(Doc doc, extraction &out, workload w) {
  ondemand::array path;
  if (doc["categoryPath"].get_array().get(path)) { return SUCCESS; }
  size_t idx = 0;
  for (auto elem : path) {
    if (idx == 1 || idx == 2) {
      ondemand::object o;
      if (!elem.get_object().get(o)) {
        ondemand::value id;
        if (!o["id"].get(id)) { take(id, out, w); }
      }
    }
    idx++;
    if (idx > 2) { break; }
  }
  return SUCCESS;
}

// $.routes[*].legs[*].steps[*].distance.text
template <typename Doc> error_code q_google(Doc doc, extraction &out, workload w) {
  ondemand::array routes;
  if (doc["routes"].get_array().get(routes)) { return SUCCESS; }
  for (auto route : routes) {
    ondemand::array legs;
    if (route["legs"].get_array().get(legs)) { continue; }
    for (auto leg : legs) {
      ondemand::array steps;
      if (leg["steps"].get_array().get(steps)) { continue; }
      for (auto step : steps) {
        ondemand::object dist;
        if (step["distance"].get_object().get(dist)) { continue; }
        ondemand::value text;
        if (!dist["text"].get(text)) { take(text, out, w); }
      }
    }
  }
  return SUCCESS;
}

// $[8], $[9] -- NSPL data rows are positional arrays; see common.h.
template <typename Doc> error_code q_nspl(Doc doc, extraction &out, workload w) {
  ondemand::array row;
  if (auto e = doc.get_array().get(row)) { return e; }
  size_t idx = 0;
  for (auto cell : row) {
    if (idx == 8 || idx == 9) {
      ondemand::value v;
      if (!cell.get(v)) { take(v, out, w); }
    }
    idx++;
    if (idx > 9) { break; }
  }
  return SUCCESS;
}

// $.bestMarketplacePrice.price, $.name
template <typename Doc> error_code q_walmart(Doc doc, extraction &out, workload w) {
  ondemand::object root;
  if (auto e = doc.get_object().get(root)) { return e; }
  for (auto field : root) {
    std::string_view key;
    if (auto e = field.unescaped_key().get(key)) { return e; }
    if (key == "name") {
      take(field.value(), out, w);
    } else if (key == "bestMarketplacePrice") {
      ondemand::object bmp;
      if (!field.value().get_object().get(bmp)) {
        ondemand::value price;
        if (!bmp["price"].get(price)) { take(price, out, w); }
      }
    }
  }
  return SUCCESS;
}

// $.claims.P150[*].mainsnak.property
template <typename Doc> error_code q_wiki(Doc doc, extraction &out, workload w) {
  ondemand::object claims;
  if (doc["claims"].get_object().get(claims)) { return SUCCESS; }
  ondemand::array p150;
  if (claims["P150"].get_array().get(p150)) { return SUCCESS; }
  for (auto claim : p150) {
    ondemand::object snak;
    if (claim["mainsnak"].get_object().get(snak)) { continue; }
    ondemand::value prop;
    if (!snak["property"].get(prop)) { take(prop, out, w); }
  }
  return SUCCESS;
}

template <typename Doc>
error_code dispatch(Doc doc, extraction &out, query_id q, workload w) {
  if (w == workload::structure) {
    // Touch nothing. iterate_many still runs stage 1 over every batch, and
    // parse_many_parallel still slices and indexes every document, so this
    // isolates structural indexing.
    out.matches++;
    return SUCCESS;
  }
  switch (q) {
  case query_id::twitter: return q_twitter(doc, out, w);
  case query_id::bestbuy: return q_bestbuy(doc, out, w);
  case query_id::google_map: return q_google(doc, out, w);
  case query_id::nspl: return q_nspl(doc, out, w);
  case query_id::walmart: return q_walmart(doc, out, w);
  case query_id::wiki: return q_wiki(doc, out, w);
  }
  return SUCCESS;
}

} // namespace

extraction run_serial(const char *data, size_t size, query_id q, workload w,
                      bool threaded, size_t batch_bytes,
                      std::vector<std::string> *trace, size_t trace_limit) {
  ondemand::parser parser;
  parser.threaded = threaded;
  extraction total;
  ondemand::document_stream stream;
  // The corpus is one document per line, the same structure Pison is handed as
  // its record table, so we tell simdjson too.
  if (!parser
           .iterate_many(data, size, batch_bytes,
#if JSONBENCH_HAVE_NEWLINE_DELIMITED
                         stream_format::newline_delimited
#else
                         stream_format::whitespace_delimited
#endif
                         )
           .get(stream)) {
    for (auto it = stream.begin(); it != stream.end(); ++it) {
      auto doc = *it;
      extraction one;
      one.trace = trace;
      one.trace_limit = trace_limit;
      if (!dispatch(doc, one, q, w)) { total.merge(one); }
    }
  }
  return total;
}

extraction run_parallel(const char *data, size_t size, query_id q, workload w,
                        size_t threads, size_t slice_bytes,
                        bool static_partition) {
  parallel::options options;
  options.threads = threads;
  options.slice_bytes = slice_bytes;
  options.static_partition = static_partition;
#if JSONBENCH_HAVE_NEWLINE_DELIMITED
  // Our corpus is strictly one document per line, which lets simdjson skip the
  // tail of a partially read document instead of walking it.
  options.format = stream_format::newline_delimited;
#else
  options.format = stream_format::whitespace_delimited;
#endif
  padded_string_view json(data, size, size + SIMDJSON_PADDING);
  auto result = parallel::parse_many<extraction>(
      json,
      [q, w](auto doc, extraction &out) { return dispatch(doc, out, q, w); },
      options);
  extraction total;
  for (const auto &shard : result.shards()) {
    for (const extraction &e : shard) { total.merge(e); }
  }
  return total;
}

} // namespace sj
} // namespace jsonbench
