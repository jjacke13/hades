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
