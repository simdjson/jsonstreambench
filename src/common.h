// Shared vocabulary between the two engines: the query set, the
// order-independent result sink used to prove they agree, and the record table
// that Pison consumes.
#ifndef JSONBENCH_COMMON_H
#define JSONBENCH_COMMON_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jsonbench {

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
// The six JSONPath queries of the Pison paper (Jiang et al., PVLDB 14(4)),
// applied per record to the JSON-lines form of each dataset. The `[*]` prefix
// of the bulky-record form is dropped because each line is already one element
// of the dominating array.
//
// NSPL is the exception: the paper's NSPL query targets the file's metadata
// header ($.meta.view.columns[*].name), which does not exist in the data rows
// that make up the JSON-lines form. Its records are positional arrays, so we
// define an equivalent two-field extraction over fixed columns and say so.
//
// We implement these from the paper's specification rather than from the
// harnesses shipped in cuJSON's `related_works/pison/JSON_lines`, which contain
// copy-paste errors (google.cpp runs the bestbuy query; nspl.cpp runs the
// google query and misspells "text" as "tex", matching nothing).
//
// OpenAlex authors is our own addition (see datasets.sh): the corpus is one
// daily change partition of the public snapshot, and the query selects the two
// root-level fields that identify an author record.
enum class query_id {
  twitter,    // $.user.lang, $.lang
  bestbuy,    // $.categoryPath[1:3].id
  google_map, // $.routes[*].legs[*].steps[*].distance.text
  nspl,       // $[8], $[9]   (see note above)
  walmart,    // $.bestMarketplacePrice.price, $.name
  wiki,       // $.claims.P150[*].mainsnak.property
  openalex,   // $.display_name, $.works_count  (see note below)
};

const char *query_name(query_id q);
bool query_from_name(const std::string &s, query_id &out);
// Nesting depth the query needs, i.e. the number of bitmap levels Pison must
// build. Passing the minimum is the choice most favourable to Pison.
int query_levels(query_id q);
// Guess the query from a dataset filename (twitter_small_records.json -> twitter).
bool query_from_path(const std::string &path, query_id &out);

// ---------------------------------------------------------------------------
// Result sink
// ---------------------------------------------------------------------------
// simdjson's parse_many_parallel returns values grouped per worker, so results
// arrive in a different order than a serial run produces them. The accumulator
// is therefore commutative: a sum of per-value hashes plus a match count. Two
// engines agree iff both fields agree.
struct extraction {
  uint64_t sum = 0;
  uint64_t matches = 0;
  // When non-null, the first values seen are also recorded verbatim. Used by
  // --dump to compare the two engines' output side by side; never set on a
  // timed run.
  std::vector<std::string> *trace = nullptr;
  size_t trace_limit = 0;

  // Normalizing before hashing lets us compare Pison's raw record text against
  // simdjson's raw token. Pison's BitmapIterator::getValue returns the bytes
  // between the field's colon and the next structural character, so a value
  // that is not last in its object comes back with a trailing comma
  // (`"ru",`). simdjson's raw_json_token never does. We therefore strip
  // whitespace, then a trailing comma, then one layer of double quotes -- a
  // no-op on the simdjson side, so the rule stays symmetric.
  // Whitespace, then one trailing comma, then whitespace again. Leaves the
  // quotes in place; callers that need the string body strip them.
  static std::string_view trim_token(std::string_view s) {
    auto is_ws = [](char c) {
      return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    auto trim = [&] {
      while (!s.empty() && is_ws(s.front())) { s.remove_prefix(1); }
      while (!s.empty() && is_ws(s.back())) { s.remove_suffix(1); }
    };
    trim();
    if (!s.empty() && s.back() == ',') { s.remove_suffix(1); trim(); }
    return s;
  }

  static std::string_view normalize(std::string_view s) {
    s = trim_token(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
      s.remove_prefix(1);
      s.remove_suffix(1);
    }
    return s;
  }

  void feed(std::string_view raw) {
    std::string_view v = normalize(raw);
    uint64_t h = 1469598103934665603ULL;
    for (char c : v) {
      h ^= static_cast<unsigned char>(c);
      h *= 1099511628211ULL;
    }
    sum += h; // commutative: order of documents does not matter
    matches++;
    if (trace && trace->size() < trace_limit) { trace->emplace_back(v); }
  }

  void merge(const extraction &o) {
    sum += o.sum;
    matches += o.matches;
  }
};

// ---------------------------------------------------------------------------
// Record table
// ---------------------------------------------------------------------------
// Pison cannot index anything until every record's offset and length are
// known. This is the product of that pass, whichever loader produced it.
struct record_table {
  // Offsets into the (possibly padded) text buffer.
  std::vector<uint64_t> offset;
  std::vector<uint32_t> length;
  size_t count() const { return offset.size(); }
};

} // namespace jsonbench

#endif
