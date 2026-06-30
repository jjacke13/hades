#include <gtest/gtest.h>
#include <fstream>
#include <memory>
#include <string>
#include "hades/module/embedding_memory_module.h"
#include "hades/blackboard.h"
#include "hades/config.h"
using namespace hades;

namespace {
struct FakeProvider : EmbeddingProvider {       // "alpha"->(1,0), "beta"->(0,1), query echoes
  EmbedResult embed(const std::vector<std::string>& in) override {
    EmbedResult r; r.model = "fake"; r.dim = 2;
    for (const auto& t : in) {
      if (t.find("alpha") != std::string::npos) r.vectors.push_back({1.0f, 0.0f});
      else if (t.find("beta") != std::string::npos) r.vectors.push_back({0.0f, 1.0f});
      else r.vectors.push_back({1.0f, 0.0f});   // default near alpha
    }
    return r;
  }
  std::string model() const override { return "fake"; }
};
struct FailProvider : EmbeddingProvider {
  EmbedResult embed(const std::vector<std::string>&) override { return {{}, "fake", 0, "boom"}; }
  std::string model() const override { return "fake"; }
};
struct ShiftingProvider : EmbeddingProvider {   // index/probe -> "modelA"; a "SHIFT" query -> "modelB"
  EmbedResult embed(const std::vector<std::string>& in) override {
    bool shift = false;
    for (const auto& t : in) if (t.find("SHIFT") != std::string::npos) shift = true;
    EmbedResult r; r.dim = 2; r.model = shift ? "modelB" : "modelA";
    for (std::size_t i = 0; i < in.size(); ++i) r.vectors.push_back({1.0f, 0.0f});
    return r;
  }
  std::string model() const override { return "modelA"; }
};
std::string tmp(const std::string& n) { return testing::TempDir() + "/" + n; }
Block cfg(const std::string& store, const std::string& cache) {
  Block b; b.section = "Embedding";
  b.kv["memory_store"] = store; b.kv["cache_dir"] = cache; b.kv["min_similarity"] = "0.2";
  b.kv["index_sessions"] = "false";
  return b;
}
}  // namespace

TEST(EmbeddingMemoryModule, RetrievesSemanticMatchAndPosts) {
  std::string store = tmp("em_store.jsonl"), cache = tmp("em_cache");
  { std::ofstream f(store, std::ios::trunc); f << "{\"text\":\"alpha fact\",\"ts\":1}\n{\"text\":\"beta fact\",\"ts\":2}\n"; }
  std::remove((cache + "/memory.vec.jsonl").c_str());
  Blackboard bb;
  EmbeddingMemoryModule m(std::make_unique<FakeProvider>());
  m.on_start(cfg(store, cache), bb);            // inline index (no executor)
  m.on_attach(bb);
  std::string got;
  bb.subscribe("RETRIEVED_MEMORY_SEMANTIC", [&](const Entry& e) { got = e.value.get<std::string>(); });
  bb.post("USER_MESSAGE", "tell me about alpha", "chat");
  bb.pump();
  EXPECT_NE(got.find("alpha fact"), std::string::npos);
  EXPECT_EQ(got.find("beta fact"), std::string::npos);  // below floor for an alpha query
}
TEST(EmbeddingMemoryModule, ProviderFailureIsSoftEmpty) {
  std::string store = tmp("em_store2.jsonl"), cache = tmp("em_cache2");
  { std::ofstream f(store, std::ios::trunc); f << "{\"text\":\"alpha\",\"ts\":1}\n"; }
  std::remove((cache + "/memory.vec.jsonl").c_str());
  Blackboard bb;
  EmbeddingMemoryModule m(std::make_unique<FailProvider>());
  m.on_start(cfg(store, cache), bb);
  m.on_attach(bb);
  std::string got = "UNSET";
  bb.subscribe("RETRIEVED_MEMORY_SEMANTIC", [&](const Entry& e) { got = e.value.get<std::string>(); });
  bb.post("USER_MESSAGE", "alpha?", "chat");
  bb.pump();                                    // must NOT crash
  EXPECT_EQ(got, "");                            // fail-soft: empty
}
TEST(EmbeddingMemoryModule, CacheStampMismatchAtQueryIsSoftEmpty) {
  // Defense-in-depth (path C): if the query's embedding model differs from the cache's stamp, the
  // query must fail-soft (never compare incomparable vectors) -> "" / keyword-only. run_index_
  // stamps the cache "modelA"; the "SHIFT" query returns "modelB" -> VectorCache.load() mismatch.
  std::string store = tmp("em_store3.jsonl"), cache = tmp("em_cache3");
  { std::ofstream f(store, std::ios::trunc); f << "{\"text\":\"alpha fact\",\"ts\":1}\n"; }
  std::remove((cache + "/memory.vec.jsonl").c_str());
  Blackboard bb;
  EmbeddingMemoryModule m(std::make_unique<ShiftingProvider>());
  m.on_start(cfg(store, cache), bb);
  m.on_attach(bb);
  std::string got = "UNSET";
  bb.subscribe("RETRIEVED_MEMORY_SEMANTIC", [&](const Entry& e) { got = e.value.get<std::string>(); });
  bb.post("USER_MESSAGE", "SHIFT please", "chat");
  bb.pump();
  EXPECT_EQ(got, "");                            // stamp mismatch -> fail-soft empty
}
