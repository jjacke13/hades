// tests/test_llm_module.cpp — unit tests for LLMModule Blackboard integration
//
// Uses a StubProvider to verify that LLMModule subscribes LLM_REQUEST, posts
// LLM_RESPONSE with the provider text, accumulates BUDGET_SPENT_USD across
// turns at a configurable price_per_mtok, and survives a malformed request
// without throwing — all interactions are through the Blackboard pump().

#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <thread>
#include "hades/module/llm_module.h"
#include "hades/blackboard.h"
#include "hades/executor.h"
using namespace hades;
struct StubProvider : Provider {
  LlmResponse complete(const LlmRequest&) override {
    LlmResponse r; r.text="ok"; r.prompt_tokens=1000000; r.completion_tokens=0; return r; }
};
TEST(LLMModule, AnswersRequestAndTracksBudget) {
  Blackboard bb;
  LLMModule m(std::make_unique<StubProvider>());
  Block cfg; cfg.kv["price_per_mtok"]="2.0";
  m.on_start(cfg, bb); m.on_attach(bb);
  std::string text; double spent=-1;
  bb.subscribe("LLM_RESPONSE", [&](const Entry& e){ text=e.value["text"]; });
  bb.subscribe("BUDGET_SPENT_USD", [&](const Entry& e){ spent=e.value.get<double>(); });
  bb.post("LLM_REQUEST", {{"messages",nlohmann::json::array()},{"tools",nlohmann::json::array()}}, "arb");
  bb.pump();
  EXPECT_EQ(text,"ok");
  EXPECT_DOUBLE_EQ(spent, 2.0);    // 1e6 prompt tokens * $2/Mtok
}
TEST(LLMModule, BudgetAccumulatesAcrossTurns) {
  Blackboard bb;
  LLMModule m(std::make_unique<StubProvider>());
  Block cfg; cfg.kv["price_per_mtok"]="2.0";
  m.on_start(cfg, bb); m.on_attach(bb);
  double spent=-1;
  bb.subscribe("BUDGET_SPENT_USD", [&](const Entry& e){ spent=e.value.get<double>(); });
  bb.post("LLM_REQUEST", {{"messages",nlohmann::json::array()},{"tools",nlohmann::json::array()}}, "arb");
  bb.pump();
  bb.post("LLM_REQUEST", {{"messages",nlohmann::json::array()},{"tools",nlohmann::json::array()}}, "arb");
  bb.pump();
  EXPECT_DOUBLE_EQ(spent, 4.0);   // cumulative: $2 + $2
}
TEST(LLMModule, MalformedRequestDoesNotThrow) {
  Blackboard bb;
  LLMModule m(std::make_unique<StubProvider>());
  Block cfg; m.on_start(cfg, bb); m.on_attach(bb);
  bool got=false;
  bb.subscribe("LLM_RESPONSE", [&](const Entry&){ got=true; });
  bb.post("LLM_REQUEST", {{"tools", 42}}, "arb");   // no messages; tools wrong type
  EXPECT_NO_THROW(bb.pump());
  EXPECT_TRUE(got);   // still produced a response
}
TEST(LLMModule, EchoesRequestEpoch) {
  Blackboard bb;
  LLMModule m(std::make_unique<StubProvider>());
  Block cfg; m.on_start(cfg, bb); m.on_attach(bb);
  std::uint64_t echoed = 0;
  bb.subscribe("LLM_RESPONSE", [&](const Entry& e){ echoed = e.value.value("epoch", static_cast<std::uint64_t>(0)); });
  bb.post("LLM_REQUEST",
          {{"messages",nlohmann::json::array()},{"tools",nlohmann::json::array()},{"epoch",7}}, "arb");
  bb.pump();
  EXPECT_EQ(echoed, static_cast<std::uint64_t>(7));   // request epoch echoed into the response
}
TEST(LLMModule, OffloadsToExecutorWithoutBlockingTheBus) {
  // A provider whose complete() blocks ~50ms; with an Executor the post(LLM_REQUEST) must
  // return before the response exists, and run_until collects it.
  struct SlowProvider : Provider {
    LlmResponse complete(const LlmRequest&) override {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      LlmResponse r; r.text = "slow ok"; return r;
    }
  };
  // Declaration order is load-bearing: Executor MUST be declared LAST so it is
  // destroyed FIRST (reverse order: ex -> m -> bb). Its dtor joins the worker
  // while the module and Blackboard are still alive — the worker reads only
  // provider_/bb_ and posts ONLY LLM_RESPONSE (budget accrues on the pump thread,
  // not in the worker), so run_until() seeing LLM_RESPONSE does NOT prove the
  // worker has returned. Joining first closes a use-after-free on the
  // module's provider_ / the Blackboard.
  Blackboard bb;
  LLMModule m(std::make_unique<SlowProvider>());
  Executor ex(2);
  m.set_executor(&ex);
  Block cfg; cfg.kv["price_per_mtok"] = "0";
  m.on_start(cfg, bb); m.on_attach(bb);
  std::string text;
  bb.subscribe("LLM_RESPONSE", [&](const Entry& e){ text = e.value.value("text", std::string{}); });
  bb.post("LLM_REQUEST", {{"messages", nlohmann::json::array()}, {"tools", nlohmann::json::array()}}, "arb");
  // Right after pump returns, the worker is still sleeping -> no response yet (bus not blocked).
  bb.pump();
  EXPECT_TRUE(text.empty());                          // offloaded: not done synchronously
  bool ok = bb.run_until([&]{ return !text.empty(); }, 5.0);
  EXPECT_TRUE(ok);
  EXPECT_EQ(text, "slow ok");
}
TEST(LLMModule, ResolvesLlmTimeoutFromSession) {
  // on_start resolves the per-call cpr timeout from the Session block BEFORE the
  // provider build / api-key check, so an injected provider lets us assert the
  // resolved value without needing a real endpoint or HADES_API_KEY.
  Blackboard bb;
  LLMModule m(std::make_unique<StubProvider>());   // injected -> no real provider build
  Block cfg; cfg.kv["llm_timeout_s"] = "5";
  m.on_start(cfg, bb);
  EXPECT_DOUBLE_EQ(m.resolved_llm_timeout_s(), 5.0);

  LLMModule m2(std::make_unique<StubProvider>());
  Block cfg2;   // no llm_timeout_s -> default
  m2.on_start(cfg2, bb);
  EXPECT_DOUBLE_EQ(m2.resolved_llm_timeout_s(), 600.0);
}
TEST(LLMModule, AuxSpendFoldsIntoCumulativeBudget) {
  // AUX_SPENT_USD is a per-call DELTA from an aux consumer (auto-extract); the LLMModule
  // folds it into the same cumulative spent_ the LLM_RESPONSE handler owns, so
  // stay_on_budget gates aux spend too. Non-numeric payloads are ignored.
  Blackboard bb;
  LLMModule m(std::make_unique<StubProvider>());
  Block cfg; cfg.kv["price_per_mtok"] = "2.0";
  m.on_start(cfg, bb); m.on_attach(bb);
  double budget = -1.0;
  bb.subscribe("BUDGET_SPENT_USD", [&](const Entry& e){ budget = e.value.get<double>(); });
  bb.post("AUX_SPENT_USD", 0.25, "auto_extract");
  bb.post("AUX_SPENT_USD", "garbage", "x");            // ignored, no crash
  bb.post("AUX_SPENT_USD", 0.25, "auto_extract");
  bb.pump();
  EXPECT_DOUBLE_EQ(budget, 0.5);                        // two deltas accumulated
}
TEST(LLMModule, BudgetAccruesOnPumpThreadUnderOffload) {
  // Race-free budget: under an Executor the worker posts ONLY LLM_RESPONSE; the
  // LLMModule accrues spent_ and posts BUDGET_SPENT_USD from a pump-thread
  // handler reacting to LLM_RESPONSE, so spent_ is written only on the pump
  // thread (no data race even if two workers overlap). Provider returns known
  // tokens (1e6 prompt) at $2/Mtok -> $2.00.
  struct TokenProvider : Provider {
    LlmResponse complete(const LlmRequest&) override {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));   // genuinely offloaded
      LlmResponse r; r.text = "ok"; r.prompt_tokens = 1000000; r.completion_tokens = 0;
      return r;
    }
  };
  // Declaration order is load-bearing: Executor declared LAST so it is destroyed
  // FIRST (ex -> m -> bb). Its dtor joins the worker while module + Blackboard
  // are still alive — the worker calls provider_->complete() and bb_->post(),
  // both of which dangle if the module/bb were torn down before the join.
  Blackboard bb;
  LLMModule m(std::make_unique<TokenProvider>());
  Executor ex(2);
  m.set_executor(&ex);
  Block cfg; cfg.kv["price_per_mtok"] = "2.0";
  m.on_start(cfg, bb); m.on_attach(bb);
  double spent = -1; bool budget_seen = false;
  bb.subscribe("BUDGET_SPENT_USD", [&](const Entry& e){ spent = e.value.get<double>(); budget_seen = true; });
  bb.post("LLM_REQUEST", {{"messages", nlohmann::json::array()}, {"tools", nlohmann::json::array()}}, "arb");
  // Offloaded: budget is not available synchronously after the first pump.
  EXPECT_TRUE(bb.run_until([&]{ return budget_seen; }, 5.0));
  EXPECT_DOUBLE_EQ(spent, 2.0);   // accrued on the pump thread from LLM_RESPONSE
}
