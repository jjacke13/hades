// tests/test_compactor_module.cpp — CompactorModule: summarize-on-request, always-terminal
//
// FakeProvider + no executor -> the whole flow runs inline on pump(); the final test wires
// a REAL Arbiter and the module on one bus for the full detect->summarize->apply->fold loop.
#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <string>
#include <vector>
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/executor.h"
#include "hades/module/compactor_module.h"
#include "hades/arbiter.h"
using namespace hades;

namespace {
struct FakeProvider : Provider {
  std::string reply = "MERGED SUMMARY";
  bool throw_on_call = false;
  std::vector<LlmRequest> seen;
  LlmResponse complete(const LlmRequest& r) override {
    seen.push_back(r);
    if (throw_on_call) throw std::runtime_error("aux down");
    LlmResponse resp;
    resp.text = reply;
    resp.prompt_tokens = 100;
    resp.completion_tokens = 50;
    return resp;
  }
};
// Construct-in-place rig: CompactorModule holds a std::atomic member, so it is neither
// copyable nor movable — the provider is injected at construction, never assigned after.
struct Rig {
  Blackboard bb;
  std::unique_ptr<FakeProvider> owned = std::make_unique<FakeProvider>();
  FakeProvider* prov = owned.get();
  CompactorModule m{std::move(owned)};
  void start(const Block& cfg = Block{}) { m.on_start(cfg, bb); m.on_attach(bb); }
};
nlohmann::json request(int upto = 3) {
  return {{"session", "s1"},
          {"upto", upto},
          {"span", nlohmann::json::array({{{"role", "user"}, {"content", "we set port 9090"}}})},
          {"current_summary", "PRIOR SUMMARY"}};
}
}  // namespace

TEST(CompactorModule, SummarizesAndPostsSessionSummary) {
  Rig r;
  r.start();
  nlohmann::json out, aux;
  r.bb.subscribe("SESSION_SUMMARY", [&](const Entry& e) { out = e.value; });
  r.bb.subscribe("AUX_SPENT_USD", [&](const Entry& e) { aux = e.value; });
  r.bb.post("COMPACT_REQUEST", request(), "arbiter");
  r.bb.pump();
  ASSERT_FALSE(out.is_null());
  EXPECT_EQ(out.value("session", ""), "s1");
  EXPECT_EQ(out.value("upto", 0), 3);
  EXPECT_EQ(out.value("text", ""), "MERGED SUMMARY");
  // The request carried BOTH the prior summary and the span content (rolling merge input).
  ASSERT_EQ(r.prov->seen.size(), 1u);
  const std::string user = r.prov->seen[0].messages.back()["content"].get<std::string>();
  EXPECT_NE(user.find("PRIOR SUMMARY"), std::string::npos);
  EXPECT_NE(user.find("we set port 9090"), std::string::npos);
  EXPECT_TRUE(aux.is_null());   // price_per_mtok unset -> no spend delta
}

TEST(CompactorModule, ProviderThrowPostsCompactFailed) {
  Rig r;
  r.prov->throw_on_call = true;
  r.start();
  nlohmann::json failed, out;
  r.bb.subscribe("COMPACT_FAILED", [&](const Entry& e) { failed = e.value; });
  r.bb.subscribe("SESSION_SUMMARY", [&](const Entry& e) { out = e.value; });
  r.bb.post("COMPACT_REQUEST", request(), "arbiter");
  r.bb.pump();
  EXPECT_TRUE(out.is_null());
  ASSERT_FALSE(failed.is_null());                  // ALWAYS-terminal: failure still replies
  EXPECT_EQ(failed.value("session", ""), "s1");
  EXPECT_EQ(failed.value("upto", 0), 3);
}

TEST(CompactorModule, ReplyTruncatedToCharLimit) {
  Rig r;
  r.prov->reply = std::string(200, 's');
  Block cfg; cfg.kv["summary_char_limit"] = "50";
  r.start(cfg);
  nlohmann::json out;
  r.bb.subscribe("SESSION_SUMMARY", [&](const Entry& e) { out = e.value; });
  r.bb.post("COMPACT_REQUEST", request(), "arbiter");
  r.bb.pump();
  const std::string text = out.value("text", "");
  EXPECT_LE(text.size(), 50u + 12u);               // limit + " (truncated)" marker
  EXPECT_NE(text.find(" (truncated)"), std::string::npos);
}

TEST(CompactorModule, MalformedAndEmptyRequestsIgnored) {
  Rig r;
  r.start();
  bool any = false;
  r.bb.subscribe("SESSION_SUMMARY", [&](const Entry&) { any = true; });
  r.bb.subscribe("COMPACT_FAILED", [&](const Entry&) { any = true; });
  r.bb.post("COMPACT_REQUEST", "not an object", "x");
  r.bb.post("COMPACT_REQUEST", {{"session", "s"}, {"upto", 0},
                                {"span", nlohmann::json::array()}}, "x");  // empty span / upto 0
  r.bb.pump();
  EXPECT_FALSE(any);                               // not from the Arbiter's detect -> no reply owed
}

TEST(CompactorModule, PriceYieldsAuxSpendDelta) {
  Rig r;
  Block cfg; cfg.kv["price_per_mtok"] = "10.0";
  r.start(cfg);
  nlohmann::json aux;
  r.bb.subscribe("AUX_SPENT_USD", [&](const Entry& e) { aux = e.value; });
  r.bb.post("COMPACT_REQUEST", request(), "arbiter");
  r.bb.pump();
  ASSERT_TRUE(aux.is_number());
  EXPECT_NEAR(aux.get<double>(), 150.0 / 1e6 * 10.0, 1e-12);
}

TEST(CompactorModule, FullLoopDetectSummarizeApplyFold) {
  Rig r;                                           // FakeProvider replies "MERGED SUMMARY"
  Arbiter a; a.set_system_prompt("SOUL");
  a.set_history_budget_chars(400.0);
  a.on_attach(r.bb);
  r.start();
  nlohmann::json req;
  r.bb.subscribe("LLM_REQUEST", [&](const Entry& e) { req = e.value; });
  const std::string big(150, 'x');
  for (int i = 0; i < 5; ++i) {                    // overflow -> detect -> summarize -> apply
    r.bb.post("USER_MESSAGE", big + std::to_string(i), "chat"); r.bb.pump();
    r.bb.post("LLM_RESPONSE", {{"text", "ok"}, {"epoch", (std::uint64_t)(i + 1)}}, "llm");
    r.bb.pump();
  }
  const std::string sys = req["messages"][0]["content"].get<std::string>();
  EXPECT_NE(sys.find("Earlier in this session (compacted"), std::string::npos);
  EXPECT_NE(sys.find("MERGED SUMMARY"), std::string::npos);
}

TEST(CompactorModule, WhitespaceReplyPostsCompactFailed) {
  Rig r;
  r.prov->reply = "  \n\n  ";
  r.start();
  nlohmann::json failed, out;
  r.bb.subscribe("COMPACT_FAILED", [&](const Entry& e) { failed = e.value; });
  r.bb.subscribe("SESSION_SUMMARY", [&](const Entry& e) { out = e.value; });
  r.bb.post("COMPACT_REQUEST", request(), "arbiter");
  r.bb.pump();
  EXPECT_TRUE(out.is_null());                      // blank is NOT a summary (review I1)
  ASSERT_FALSE(failed.is_null());
  EXPECT_EQ(failed.value("upto", 0), 3);
}

TEST(CompactorModule, NonStringFieldsNeverWedgeTheSlot) {
  // review I2: object span elements with non-string role/content (and a non-string
  // current_summary) must neither throw out of pump() nor leak the busy slot.
  Rig r;
  r.start();
  int terminals = 0;
  r.bb.subscribe("SESSION_SUMMARY", [&](const Entry&) { ++terminals; });
  r.bb.subscribe("COMPACT_FAILED", [&](const Entry&) { ++terminals; });
  nlohmann::json bad = {{"session", "s1"}, {"upto", 3},
                        {"span", nlohmann::json::array({{{"role", 7}, {"content", 9}}})},
                        {"current_summary", 42}};
  r.bb.post("COMPACT_REQUEST", bad, "arbiter");
  r.bb.pump();                                     // must not throw
  EXPECT_EQ(terminals, 1);                         // terminal posted, slot released
  r.bb.post("COMPACT_REQUEST", request(), "arbiter");
  r.bb.pump();
  EXPECT_EQ(terminals, 2);                         // next request served — no wedge
}

TEST(CompactorModule, ExecutorPathCompactsCrossThread) {
  // The production shape (final review M1): the worker runs on a REAL Executor thread and
  // posts the terminal back to the pump — the one path the inline tests structurally miss.
  // Slow provider forces genuine cross-thread overlap; the Executor is declared AFTER the
  // module — destroyed first, joins the worker while module + bus are alive (live teardown
  // order). This is the TSan-relevant compactor test.
  struct SlowProvider : Provider {
    LlmResponse complete(const LlmRequest&) override {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      LlmResponse r;
      r.text = "CROSS-THREAD SUMMARY";
      r.prompt_tokens = 10;
      r.completion_tokens = 5;
      return r;
    }
  };
  Blackboard bb;
  CompactorModule m(std::make_unique<SlowProvider>());
  m.on_start(Block{}, bb);
  m.on_attach(bb);
  nlohmann::json out;
  bb.subscribe("SESSION_SUMMARY", [&](const Entry& e) { out = e.value; });
  Executor ex(2);
  m.set_executor(&ex);
  bb.post("COMPACT_REQUEST", request(), "arbiter");
  bb.pump();                                       // handler submits; worker still sleeping
  EXPECT_TRUE(out.is_null());
  ASSERT_TRUE(bb.run_until([&] { return !out.is_null(); }, 5.0));
  EXPECT_EQ(out.value("text", ""), "CROSS-THREAD SUMMARY");
  EXPECT_EQ(out.value("upto", 0), 3);
}
