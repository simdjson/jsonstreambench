// RapidJSON.
//
// Configuration choices, all disclosed in the paper:
//
//   * kParseFullPrecisionFlag. RapidJSON's default number parser is faster but
//     may land one unit in the last place away from the correctly rounded
//     double. simdjson is always exact, so without this flag the two engines
//     disagree on Walmart prices and `--verify` fails. Correctness first: the
//     flag costs RapidJSON throughput and we say so.
//   * kParseValidateEncodingFlag. simdjson validates UTF-8 unconditionally;
//     RapidJSON does so only on request. Leaving it off would be measuring a
//     weaker guarantee.
//   * No in-situ parsing, for the reason given in yyjson_engine.cpp: the
//     parallel run shares one read-only buffer.
//
// Memory is reused across documents through a MemoryPoolAllocator over a
// user-supplied buffer, cleared between documents, so the steady state performs
// no allocation. A fresh Document per line would instead measure malloc.
#include "dom_engine.h"
#include "dom_parallel.h"
#include "dom_queries.h"

#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/rapidjson.h"

#include <memory>
#include <vector>

namespace jsonbench {
namespace dom {
namespace {

constexpr unsigned kFlags = rapidjson::kParseFullPrecisionFlag |
                            rapidjson::kParseValidateEncodingFlag;

using pool_alloc = rapidjson::MemoryPoolAllocator<rapidjson::CrtAllocator>;
using pool_document =
    rapidjson::GenericDocument<rapidjson::UTF8<>, pool_alloc,
                               rapidjson::CrtAllocator>;

struct rj_traits {
  using node = const rapidjson::Value *;

  static bool obj_get(node v, std::string_view key, node &out) {
    if (v == nullptr || !v->IsObject()) { return false; }
    auto it = v->FindMember(
        rapidjson::Value(rapidjson::StringRef(key.data(), key.size())));
    if (it == v->MemberEnd()) { return false; }
    out = &it->value;
    return true;
  }

  static bool arr_at(node v, size_t i, node &out) {
    if (v == nullptr || !v->IsArray() || i >= v->Size()) { return false; }
    out = &(*v)[rapidjson::SizeType(i)];
    return true;
  }

  template <class F> static void arr_each(node v, F &&f) {
    if (v == nullptr || !v->IsArray()) { return; }
    for (const auto &elem : v->GetArray()) { f(&elem); }
  }

  static void feed(node v, extraction &out) {
    if (v == nullptr) { return; }
    if (v->IsString()) {
      out.feed(std::string_view(v->GetString(), v->GetStringLength()));
    } else if (v->IsNumber()) {
      feed_number(v->GetDouble(), out);
    } else if (v->IsBool()) {
      feed_literal(v->GetBool() ? "true" : "false", out);
    } else if (v->IsNull()) {
      feed_literal("null", out);
    }
  }
};

class rj_worker {
public:
  explicit rj_worker(size_t longest, query_id q) : q_(q) {
    // RapidJSON's DOM for a document is a few times its text size; a generous
    // multiple keeps the pool from ever calling malloc after construction,
    // and any shortfall only means the pool grows once.
    buf_.resize(longest * 8 + (64u << 10));
    pool_ = std::make_unique<pool_alloc>(buf_.data(), buf_.size());
    doc_ = std::make_unique<pool_document>(pool_.get());
  }

  void operator()(const char *data, size_t size, extraction &out) {
    for_each_line(data, size, [&](const char *line, size_t len) {
      // Release the previous document's nodes before parsing the next. The
      // parse stack lives in the Document's separate stack allocator and is
      // reused, so this is the only reset needed.
      doc_->SetNull();
      pool_->Clear();
      if (doc_->Parse<kFlags>(line, len).HasParseError()) { return; }
      const rapidjson::Value *root = doc_.get();
      run_query<rj_traits>(q_, root, out);
    });
  }

private:
  std::vector<char> buf_;
  std::unique_ptr<pool_alloc> pool_;
  std::unique_ptr<pool_document> doc_;
  query_id q_;
};

} // namespace

extraction rapidjson_run_serial(const char *data, size_t size, query_id q,
                                size_t longest) {
  rj_worker w(longest, q);
  extraction out;
  w(data, size, out);
  return out;
}

extraction rapidjson_run_parallel(const char *data, size_t size, query_id q,
                                  size_t threads, size_t slice_bytes,
                                  size_t longest) {
  return run_sliced(data, size, threads, slice_bytes, [&] {
    auto w = std::make_shared<rj_worker>(longest, q);
    return [w](const char *d, size_t n, extraction &out) { (*w)(d, n, out); };
  });
}

const char *rapidjson_version_string() { return RAPIDJSON_VERSION_STRING; }

} // namespace dom
} // namespace jsonbench
