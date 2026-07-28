#include "common.h"

#include <cstring>

namespace jsonbench {

const char *query_name(query_id q) {
  switch (q) {
  case query_id::twitter: return "twitter";
  case query_id::bestbuy: return "bestbuy";
  case query_id::google_map: return "google_map";
  case query_id::nspl: return "nspl";
  case query_id::walmart: return "walmart";
  case query_id::wiki: return "wiki";
  }
  return "?";
}

bool query_from_name(const std::string &s, query_id &out) {
  if (s == "twitter") { out = query_id::twitter; return true; }
  if (s == "bestbuy") { out = query_id::bestbuy; return true; }
  if (s == "google_map" || s == "google") { out = query_id::google_map; return true; }
  if (s == "nspl") { out = query_id::nspl; return true; }
  if (s == "walmart") { out = query_id::walmart; return true; }
  if (s == "wiki") { out = query_id::wiki; return true; }
  return false;
}

// Bitmap levels Pison must build. Its cost grows with this number, so we pass
// the smallest value that is *valid*, which is the choice most favourable to
// Pison.
//
// The value is not simply the query's depth. Pison's SerialBitmap constructor
// allocates mLevColonBitmap[0..level_num-1], but index construction writes at
// the record's true nesting level regardless, so a record deeper than level_num
// dereferences a null bitmap pointer and dies with SIGBUS. level_num must
// therefore cover the *document*, not the query. These are the minima we
// established empirically: the smallest level_num for which Pison neither
// crashes nor disagrees with simdjson on any record of the dataset.
//
//   dataset     query                                       query  minimum
//                                                           depth  level_num
//   twitter     $.user.lang, $.lang                           2        2
//   bestbuy     $.categoryPath[1:3].id                        3        3
//   google_map  $.routes[*].legs[*].steps[*].distance.text    7        8
//   nspl        $[8], $[9]                                    1        1
//   walmart     $.bestMarketplacePrice.price, $.name          2        2
//   wiki        $.claims.P150[*].mainsnak.property            4        5
int query_levels(query_id q) {
  switch (q) {
  case query_id::twitter: return 2;
  case query_id::bestbuy: return 3;
  case query_id::google_map: return 8;
  case query_id::nspl: return 1;
  case query_id::walmart: return 2;
  case query_id::wiki: return 5;
  }
  return 22; // MAX_LEVEL: always safe, never fast
}

bool query_from_path(const std::string &path, query_id &out) {
  // Longest names first so "google_map" wins over a bare "google".
  static const char *names[] = {"google_map", "bestbuy", "twitter",
                                "walmart",    "wiki",    "nspl"};
  for (const char *n : names) {
    if (path.find(n) != std::string::npos) { return query_from_name(n, out); }
  }
  // The cuJSON scalability corpus uses directory names bb / gmp / nspl.
  if (path.find("/bb/") != std::string::npos) { out = query_id::bestbuy; return true; }
  if (path.find("/gmp/") != std::string::npos) { out = query_id::google_map; return true; }
  return false;
}

} // namespace jsonbench
