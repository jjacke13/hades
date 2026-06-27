// tests/test_llm_module.cpp — unit tests for LLMModule Blackboard integration
//
// Uses a StubProvider to verify that LLMModule subscribes LLM_REQUEST, posts
// LLM_RESPONSE with the provider text, accumulates BUDGET_SPENT_USD across
// turns at a configurable price_per_mtok, and survives a malformed request
// without throwing — all interactions are through the Blackboard pump().

#include <gtest/gtest.h>
#include "hades/module/llm_module.h"
#include "hades/blackboard.h"
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
