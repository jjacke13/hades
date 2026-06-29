// tests/test_memory_wiring.cpp — build_agent wires MemoryModule (before Arbiter) and the
// save_memory tool path; a seeded store surfaces through the live graph as RETRIEVED_MEMORY.
#include <gtest/gtest.h>
#include <fstream>
#include <memory>
#include <vector>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
#include "hades/llm/provider.h"
using namespace hades;

namespace {
struct AnswerProvider : Provider {  // minimal: always a plain answer, no tool calls
  LlmResponse complete(const LlmRequest&) override { LlmResponse r; r.text = "ok"; return r; }
};
}  // namespace

TEST(MemoryWiring, WhitespaceStorePathThrows) {
  Blackboard bb;
  std::vector<Block> tools;
  Block mem; mem.section = "Memory"; mem.kv["store"] = "/tmp/has space/memory.jsonl";
  EXPECT_THROW(build_agent(bb, std::make_unique<AnswerProvider>(), tools, {}, "m", mem), MalConfig);
}

TEST(MemoryWiring, MemoryAttachedAndSeededStoreSurfaces) {
  const std::string store = ::testing::TempDir() + "/wire.jsonl";
  { std::ofstream f(store); f << R"({"text":"vaios likes lightning network","ts":1.0})" << "\n"; }

  Blackboard bb;
  std::vector<Block> tools;  // a save_memory Tool block with the bare binary (no path)
  Block t; t.section = "Tool"; t.name = "save_memory"; t.kv["native"] = SAVE_MEMORY_BIN;
  tools.push_back(t);
  Block mem; mem.section = "Memory"; mem.kv["store"] = store; mem.kv["top_n"] = "5";

  Agent agent = build_agent(bb, std::make_unique<AnswerProvider>(), tools, {}, "m", mem);
  ASSERT_TRUE(agent.memory != nullptr);

  std::string got = "UNSET";
  bb.subscribe("RETRIEVED_MEMORY", [&](const Entry& e) { got = e.value.get<std::string>(); });
  bb.post("USER_MESSAGE", "tell me about lightning", "chat");
  bb.pump();
  EXPECT_NE(got.find("lightning"), std::string::npos);  // retrieval ran through the built graph
}
