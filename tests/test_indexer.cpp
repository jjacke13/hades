#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include "hades/embedding/indexer.h"
#include "hades/embedding/provider.h"
#include "hades/embedding/session_turns.h"
#include "hades/embedding/vector_cache.h"
using namespace hades;

namespace {
struct FakeProvider : EmbeddingProvider {     // deterministic dim-2 vectors, counts calls
  int batches = 0; int texts = 0;
  EmbedResult embed(const std::vector<std::string>& in) override {
    ++batches; texts += static_cast<int>(in.size());
    EmbedResult r; r.model = "fake"; r.dim = 2;
    for (std::size_t i = 0; i < in.size(); ++i) r.vectors.push_back({static_cast<float>(in[i].size()), 1.0f});
    return r;
  }
  std::string model() const override { return "fake"; }
};
std::string tmp(const std::string& n) { return testing::TempDir() + "/" + n; }
void write_store(const std::string& p, int n) {
  std::ofstream f(p, std::ios::trunc);
  for (int i = 0; i < n; ++i) f << "{\"text\":\"fact " << i << "\",\"ts\":" << i << "}\n";
}
}  // namespace

TEST(Indexer, EmbedsAllNewRecords) {
  std::string store = tmp("ix_store.jsonl"), cache = tmp("ix_cache.jsonl");
  std::remove(cache.c_str());
  write_store(store, 3);
  FakeProvider prov;
  VectorCache vc(cache, "fake", 2); ASSERT_TRUE(vc.load());
  auto st = index_archival(prov, vc, store, 32);
  EXPECT_TRUE(st.ok);
  EXPECT_EQ(st.embedded, 3u);
  EXPECT_EQ(st.skipped, 0u);                     // fresh cache: nothing skipped
  EXPECT_EQ(vc.size(), 3u);
  EXPECT_TRUE(vc.has("memory#0"));
}
TEST(Indexer, FailSoftOnProviderError) {
  // A provider that always errors must stop early: ok=false, nothing embedded, no throw.
  struct ErrorProvider : EmbeddingProvider {
    EmbedResult embed(const std::vector<std::string>&) override { return {{}, "fake", 0, "network timeout"}; }
    std::string model() const override { return "fake"; }
  };
  std::string store = tmp("ix_err.jsonl"), cache = tmp("ix_err_cache.jsonl");
  std::remove(cache.c_str());
  write_store(store, 3);
  ErrorProvider ep;
  VectorCache vc(cache, "fake", 2); ASSERT_TRUE(vc.load());
  auto st = index_archival(ep, vc, store, 32);
  EXPECT_FALSE(st.ok);
  EXPECT_EQ(st.embedded, 0u);
  EXPECT_EQ(vc.size(), 0u);
}
TEST(Indexer, IncrementalSkipsAlreadyCached) {
  std::string store = tmp("ix_store2.jsonl"), cache = tmp("ix_cache2.jsonl");
  std::remove(cache.c_str());
  write_store(store, 2);
  { FakeProvider p1; VectorCache vc(cache, "fake", 2); vc.load(); index_archival(p1, vc, store, 32); }
  write_store(store, 4);                        // 2 new records appended
  FakeProvider p2;
  VectorCache vc(cache, "fake", 2); ASSERT_TRUE(vc.load());
  auto st = index_archival(p2, vc, store, 32);
  EXPECT_EQ(st.embedded, 2u);                   // only the 2 NEW ones
  EXPECT_EQ(st.skipped, 2u);
  EXPECT_EQ(p2.texts, 2);                       // provider asked only for the new ones
  EXPECT_EQ(vc.size(), 4u);
}
TEST(Indexer, IndexesSessionTurnsExcludingLive) {
  namespace fs = std::filesystem;
  std::string dir = testing::TempDir() + "/ix_sessions";
  fs::create_directories(dir);
  { std::ofstream f(dir + "/past.jsonl", std::ios::trunc);
    f << "{\"role\":\"user\",\"content\":\"q\"}\n{\"role\":\"assistant\",\"content\":\"a\"}\n"; }
  std::string live = dir + "/live.jsonl";
  { std::ofstream f(live, std::ios::trunc);
    f << "{\"role\":\"user\",\"content\":\"now\"}\n{\"role\":\"assistant\",\"content\":\"here\"}\n"; }
  std::string cache = testing::TempDir() + "/ix_sess_cache.jsonl";
  std::remove(cache.c_str());
  FakeProvider prov;                            // reuse the FakeProvider from the archival tests
  VectorCache vc(cache, "fake", 2); ASSERT_TRUE(vc.load());
  auto st = index_sessions(prov, vc, dir, live, 32);
  EXPECT_TRUE(st.ok);
  EXPECT_EQ(st.embedded, 1u);                   // only past.jsonl's single turn; live excluded
  EXPECT_EQ(vc.size(), 1u);
}
