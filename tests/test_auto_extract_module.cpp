// tests/test_auto_extract_module.cpp — AutoExtractModule: human-turn review -> archival facts
//
// A scripted provider stands in for the aux LLM (no socket); no Executor is set, so the
// review runs INLINE during pump — every assertion is deterministic.
#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/blackboard.h"
#include "hades/module/auto_extract_module.h"
using namespace hades;
namespace fs = std::filesystem;

namespace {
struct ScriptedAux : Provider {
  std::string reply = R"(["user prefers metric"])";
  int calls = 0;
  std::string last_digest;
  LlmResponse complete(const LlmRequest& req) override {
    ++calls;
    if (!req.messages.empty())
      last_digest = req.messages.back().value("content", "");
    LlmResponse r;
    r.text = reply;
    r.prompt_tokens = 100000;      // 0.1 Mtok -> with price 2.0: 0.5 USD total w/ completion
    r.completion_tokens = 150000;
    return r;
  }
};

std::string fresh_store(const char* tag) {
  const std::string d =
      ::testing::TempDir() + "/autoex_" + tag + "_" + std::to_string(::getpid());
  fs::remove_all(d);
  fs::create_directories(d);
  return d + "/memory.jsonl";
}
std::vector<nlohmann::json> store_lines(const std::string& p) {
  std::vector<nlohmann::json> out;
  std::ifstream f(p);
  std::string l;
  while (std::getline(f, l)) {
    auto j = nlohmann::json::parse(l, nullptr, false);
    if (!j.is_discarded()) out.push_back(j);
  }
  return out;
}
// Drives one full turn's bus traffic.
void turn(Blackboard& bb, const std::string& origin, const std::string& user,
          const std::string& assistant) {
  bb.post("TURN_ORIGIN", origin, "test");
  bb.post("USER_MESSAGE", user, "test");
  bb.post("ASSISTANT_MESSAGE", assistant, "test");
  bb.pump();
}
AutoExtractModule* attach(Blackboard& bb, std::unique_ptr<ScriptedAux> prov,
                          const std::string& store,
                          std::vector<std::unique_ptr<AutoExtractModule>>& keep) {
  auto m = std::make_unique<AutoExtractModule>(std::move(prov));
  Block cfg;
  cfg.kv["store"] = store;
  cfg.kv["price_per_mtok"] = "2.0";
  m->on_start(cfg, bb);
  m->on_attach(bb);
  keep.push_back(std::move(m));
  return keep.back().get();
}
}  // namespace

TEST(AutoExtract, HumanTurnWritesFactWithProvenanceAndPostsAuxSpend) {
  Blackboard bb;
  const std::string store = fresh_store("basic");
  std::vector<std::unique_ptr<AutoExtractModule>> keep;
  auto prov = std::make_unique<ScriptedAux>();
  ScriptedAux* p = prov.get();
  attach(bb, std::move(prov), store, keep);
  double aux = 0.0;
  bb.subscribe("AUX_SPENT_USD", [&](const Entry& e) { aux = e.value.get<double>(); });
  turn(bb, "human", "please use metric units", "noted, metric from now on");
  bb.pump();                                            // deliver the worker-posted AUX event
  EXPECT_EQ(p->calls, 1);
  EXPECT_NE(p->last_digest.find("U: please use metric units"), std::string::npos);
  auto lines = store_lines(store);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].value("text", ""), "user prefers metric");
  EXPECT_EQ(lines[0].value("src", ""), "auto");
  EXPECT_GT(lines[0].value("ts", 0.0), 0.0);
  EXPECT_DOUBLE_EQ(aux, (100000.0 + 150000.0) / 1e6 * 2.0);  // 0.5 — tokens × price
}

TEST(AutoExtract, NoneMeansNoWriteAndDupsSkipped) {
  Blackboard bb;
  const std::string store = fresh_store("none");
  std::vector<std::unique_ptr<AutoExtractModule>> keep;
  auto prov = std::make_unique<ScriptedAux>();
  ScriptedAux* p = prov.get();
  attach(bb, std::move(prov), store, keep);
  p->reply = "NONE";
  turn(bb, "human", "hi", "hello");
  EXPECT_TRUE(store_lines(store).empty());
  p->reply = R"(["fact one"])";
  turn(bb, "human", "a", "b");
  turn(bb, "human", "c", "d");                          // same fact again -> dup skipped
  EXPECT_EQ(store_lines(store).size(), 1u);
}

TEST(AutoExtract, NonHumanOriginsAndArtifactsIgnored) {
  Blackboard bb;
  const std::string store = fresh_store("origin");
  std::vector<std::unique_ptr<AutoExtractModule>> keep;
  auto prov = std::make_unique<ScriptedAux>();
  ScriptedAux* p = prov.get();
  attach(bb, std::move(prov), store, keep);
  turn(bb, "peer:pi0", "peer question", "peer answer");
  turn(bb, "heartbeat:daily", "tick prompt", "tick reply");
  turn(bb, "human", "real question", "[timed out]");    // artifact
  turn(bb, "human", "", "answer");                      // empty user
  EXPECT_EQ(p->calls, 0);
  EXPECT_TRUE(store_lines(store).empty());
}

TEST(AutoExtract, MaxFactsClampAndMalformedBusSafety) {
  Blackboard bb;
  const std::string store = fresh_store("clamp");
  std::vector<std::unique_ptr<AutoExtractModule>> keep;
  auto prov = std::make_unique<ScriptedAux>();
  ScriptedAux* p = prov.get();
  attach(bb, std::move(prov), store, keep);             // default max_facts = 3
  p->reply = R"(["f1","f2","f3","f4","f5"])";
  turn(bb, "human", "q", "a");
  EXPECT_EQ(store_lines(store).size(), 3u);
  bb.post("TURN_ORIGIN", 42, "x");                      // malformed payloads: no crash
  bb.post("USER_MESSAGE", nlohmann::json::object(), "x");
  bb.post("ASSISTANT_MESSAGE", 7, "x");
  bb.pump();
  SUCCEED();
}

// Cross-task Important (Task 1 review): the digest's per-side 2000-byte truncation can split a
// multibyte codepoint. That digest becomes an LlmRequest message content the REAL provider
// serialises with a STRICT dump() — which THROWS on invalid UTF-8, killing every aux call
// silently. The module must sanitize each digest side to a valid UTF-8 boundary at the source.
TEST(AutoExtract, DigestTruncationIsUtf8Safe) {
  Blackboard bb;
  const std::string store = fresh_store("utf8");
  std::vector<std::unique_ptr<AutoExtractModule>> keep;
  auto prov = std::make_unique<ScriptedAux>();
  ScriptedAux* p = prov.get();
  attach(bb, std::move(prov), store, keep);
  // 1990 ASCII + several 3-byte codepoints so the 2000-byte cut lands mid-codepoint.
  std::string user(1990, 'a');
  for (int i = 0; i < 6; ++i) user += "\xE4\xB8\xAD";   // 中 (U+4E2D), 3 bytes
  turn(bb, "human", user, "ok");
  ASSERT_EQ(p->calls, 1);
  // A strict dump of the received digest must NOT throw — this is exactly how the real
  // OpenAICompatProvider.build_body serialises the request body.
  auto strict_dump = [&] { return nlohmann::json{{"c", p->last_digest}}.dump(); };
  EXPECT_NO_THROW((void)strict_dump());
  EXPECT_EQ(store_lines(store).size(), 1u);
}
