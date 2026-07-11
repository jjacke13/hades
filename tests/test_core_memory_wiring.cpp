// tests/test_core_memory_wiring.cpp — build_agent wires core_memory + live core memory: adding a
// fact writes the core file, the NEXT turn's system prompt contains it, and the Session
// memory_char_limit reaches the tool argv (an over-cap add is refused end-to-end).
#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include <memory>
#include <vector>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
#include "hades/llm/provider.h"
using namespace hades;

namespace {
// Turn 1: call core_memory add. Turn 2+: plain answer. Captures the messages it was sent.
struct AddThenAnswer : Provider {
  int n = 0;
  std::string add_text = "lives in Patras";
  std::vector<nlohmann::json> seen;
  LlmResponse complete(const LlmRequest& req) override {
    seen.push_back(req.messages.empty() ? nlohmann::json::array() : nlohmann::json(req.messages));
    LlmResponse r;
    if (n++ == 0)
      r.tool_call = {{"id", "c1"}, {"name", "core_memory"},
                     {"arguments", {{"action", "add"}, {"text", add_text}}}};
    else
      r.text = "done";
    return r;
  }
};
}  // namespace

TEST(CoreMemoryWiring, CoreMemoryToolWithoutMemoryFileThrows) {
  Blackboard bb;
  std::vector<Block> tools;
  Block t; t.section = "Tool"; t.name = "core_memory"; t.kv["native"] = CORE_MEMORY_BIN; tools.push_back(t);
  // Session block has NO memory_file -> misconfiguration -> must fail fast.
  EXPECT_THROW(build_agent(bb, std::make_unique<AddThenAnswer>(), tools, {}, "m", Block{}, Block{}),
               MalConfig);
}

TEST(CoreMemoryWiring, AddedFactAppearsInNextTurnSystemPrompt) {
  const std::string core = ::testing::TempDir() + "/wire_cm_" + std::to_string(::getpid()) + ".md";
  std::remove(core.c_str());
  Blackboard bb;
  std::vector<Block> tools;
  Block t; t.section = "Tool"; t.name = "core_memory"; t.kv["native"] = CORE_MEMORY_BIN; tools.push_back(t);
  Block session; session.section = "Session"; session.kv["memory_file"] = core;

  auto prov = std::make_unique<AddThenAnswer>();
  AddThenAnswer* provp = prov.get();
  Agent agent = build_agent(bb, std::move(prov), tools, {}, "m", Block{}, session);

  bb.post("USER_MESSAGE", "remember where I live", "chat"); bb.pump();  // turn 1: model adds
  bb.post("USER_MESSAGE", "where do I live?", "chat");     bb.pump();   // turn 2: model answers

  std::ifstream f(core); std::string body((std::istreambuf_iterator<char>(f)), {});
  EXPECT_NE(body.find("lives in Patras"), std::string::npos);
  ASSERT_GE(provp->seen.size(), 2u);
  const auto& turn2 = provp->seen.back();
  ASSERT_FALSE(turn2.empty());
  EXPECT_EQ(turn2[0]["role"], "system");
  EXPECT_NE(turn2[0]["content"].get<std::string>().find("lives in Patras"), std::string::npos);
}

TEST(CoreMemoryWiring, MemoryCharLimitReachesToolArgv) {
  const std::string core = ::testing::TempDir() + "/wire_cm_cap_" + std::to_string(::getpid()) + ".md";
  std::remove(core.c_str());
  Blackboard bb;
  std::vector<Block> tools;
  Block t; t.section = "Tool"; t.name = "core_memory"; t.kv["native"] = CORE_MEMORY_BIN; tools.push_back(t);
  Block session; session.section = "Session";
  session.kv["memory_file"] = core;
  session.kv["memory_char_limit"] = "40";        // tiny cap so one long add overflows

  auto prov = std::make_unique<AddThenAnswer>();
  prov->add_text = std::string(100, 'x');        // "- " + 100 chars + "\n" > 40
  Agent agent = build_agent(bb, std::move(prov), tools, {}, "m", Block{}, session);
  nlohmann::json tool_result;
  bb.subscribe("TOOL_RESULT", [&](const Entry& e) { tool_result = e.value; });

  bb.post("USER_MESSAGE", "remember this", "chat"); bb.pump();

  ASSERT_TRUE(tool_result.is_object());
  EXPECT_FALSE(tool_result.value("ok", true));   // over-cap add refused through the real argv
  std::ifstream f(core); std::string body((std::istreambuf_iterator<char>(f)), {});
  EXPECT_TRUE(body.empty());                      // nothing written
}
