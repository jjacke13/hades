// tests/test_pin_fact_wiring.cpp — build_agent wires pin_fact + live core memory: pinning a fact
// writes the core file, and the NEXT turn's system prompt contains it (end-to-end through the graph).
#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include <memory>
#include <vector>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/llm/provider.h"
using namespace hades;

namespace {
// Turn 1: call pin_fact. Turn 2+: plain answer. Captures the messages it was sent.
struct PinThenAnswer : Provider {
  int n = 0;
  std::vector<nlohmann::json> seen;
  LlmResponse complete(const LlmRequest& req) override {
    seen.push_back(req.messages.empty() ? nlohmann::json::array() : nlohmann::json(req.messages));
    LlmResponse r;
    if (n++ == 0)
      r.tool_call = {{"id", "c1"}, {"name", "pin_fact"}, {"arguments", {{"text", "lives in Patras"}}}};
    else
      r.text = "done";
    return r;
  }
};
}  // namespace

TEST(PinFactWiring, PinnedFactAppearsInNextTurnSystemPrompt) {
  const std::string core = ::testing::TempDir() + "/wire_core_" + std::to_string(::getpid()) + ".md";
  std::remove(core.c_str());
  Blackboard bb;
  std::vector<Block> tools;
  Block t; t.section = "Tool"; t.name = "pin_fact"; t.kv["native"] = PIN_FACT_BIN; tools.push_back(t);
  Block session; session.section = "Session"; session.kv["memory_file"] = core;

  auto prov = std::make_unique<PinThenAnswer>();
  PinThenAnswer* provp = prov.get();
  Agent agent = build_agent(bb, std::move(prov), tools, {}, "m", Block{}, session);

  bb.post("USER_MESSAGE", "remember where I live", "chat"); bb.pump();  // turn 1: model calls pin_fact
  bb.post("USER_MESSAGE", "where do I live?", "chat");     bb.pump();   // turn 2: model answers

  // The core file got the pin, and turn 2's system message (messages[0]) contains it.
  std::ifstream f(core); std::string body((std::istreambuf_iterator<char>(f)), {});
  EXPECT_NE(body.find("lives in Patras"), std::string::npos);
  ASSERT_GE(provp->seen.size(), 2u);
  const auto& turn2 = provp->seen.back();
  ASSERT_FALSE(turn2.empty());
  EXPECT_EQ(turn2[0]["role"], "system");
  EXPECT_NE(turn2[0]["content"].get<std::string>().find("lives in Patras"), std::string::npos);
}
