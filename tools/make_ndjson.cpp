// make_ndjson -- derive the JSON-lines form of a dataset from its bulky
// single-record form.
//
//   make_ndjson <bulky.json> <out.ndjson> [dominating-array-key]
//
// The Pison and cuJSON corpora ship each dataset twice: as one bulky JSON
// record, and as a stream of the elements of its "dominating array", one per
// line. Only two of the six JSON-lines files are actually published, so we
// regenerate the rest from the bulky records, which are all available.
//
// Each element is minified (whitespace outside strings removed) so that it
// occupies exactly one line -- JSON forbids literal newlines inside strings,
// so minifying is sufficient to guarantee that.
//
// When the key is omitted we take the first top-level array-valued member, or
// the document itself if it is already an array. The known keys are:
//
//   twitter -> tweets    nspl    -> data     bestbuy -> products
//   google  -> items     walmart -> items    wiki    -> items
#include "simdjson.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace simdjson;

int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: %s <bulky.json> <out.ndjson> [array-key]\n", argv[0]);
    return 1;
  }
  const char *in_path = argv[1];
  const char *out_path = argv[2];
  const char *want_key = (argc > 3) ? argv[3] : nullptr;

  padded_string json;
  if (auto e = padded_string::load(in_path).get(json)) {
    std::fprintf(stderr, "cannot load %s: %s\n", in_path, error_message(e));
    return 1;
  }

  // On-demand rather than DOM: these inputs are around a gigabyte, and we only
  // ever need each element's raw text, never a materialized tree.
  ondemand::parser parser;
  ondemand::document doc;
  if (auto e = parser.iterate(json).get(doc)) {
    std::fprintf(stderr, "parse failed: %s\n", error_message(e));
    return 1;
  }

  ondemand::array items;
  std::string chosen_key;
  ondemand::json_type root_type;
  if (auto e = doc.type().get(root_type)) {
    std::fprintf(stderr, "cannot type root: %s\n", error_message(e));
    return 1;
  }
  if (root_type == ondemand::json_type::array) {
    if (auto e = doc.get_array().get(items)) {
      std::fprintf(stderr, "root array: %s\n", error_message(e));
      return 1;
    }
    chosen_key = "<root>";
  } else if (root_type == ondemand::json_type::object) {
    ondemand::object obj;
    if (auto e = doc.get_object().get(obj)) {
      std::fprintf(stderr, "root object: %s\n", error_message(e));
      return 1;
    }
    bool found = false;
    for (auto field : obj) {
      std::string_view key;
      if (field.unescaped_key().get(key)) { continue; }
      if (want_key && key != std::string_view(want_key)) { continue; }
      ondemand::value v = field.value();
      ondemand::json_type vt;
      if (v.type().get(vt)) { continue; }
      if (vt == ondemand::json_type::array) {
        if (v.get_array().get(items)) { continue; }
        chosen_key = std::string(key);
        found = true;
        break;
      }
    }
    if (!found) {
      std::fprintf(stderr, "no%s top-level array found\n",
                   want_key ? " matching" : "");
      return 1;
    }
  } else {
    std::fprintf(stderr, "root is neither object nor array\n");
    return 1;
  }

  std::FILE *out = std::fopen(out_path, "wb");
  if (!out) {
    std::fprintf(stderr, "cannot write %s\n", out_path);
    return 1;
  }
  std::vector<char> buffer;
  size_t count = 0, written = 0;
  for (auto item : items) {
    ondemand::value v;
    if (item.get(v)) { continue; }
    std::string_view raw;
    if (v.raw_json().get(raw)) { continue; }
    if (buffer.size() < raw.size() + 64) { buffer.resize(raw.size() + 64); }
    size_t out_len = 0;
    if (auto e = simdjson::minify(raw.data(), raw.size(), buffer.data(), out_len)) {
      std::fprintf(stderr, "minify failed on record %zu: %s\n", count,
                   error_message(e));
      std::fclose(out);
      return 1;
    }
    std::fwrite(buffer.data(), 1, out_len, out);
    std::fputc('\n', out);
    written += out_len + 1;
    count++;
  }
  std::fclose(out);
  std::fprintf(stderr, "%s: key=%s records=%zu bytes=%zu -> %s\n", in_path,
               chosen_key.c_str(), count, written, out_path);
  return 0;
}
