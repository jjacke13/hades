// tests/test_e2e.cpp — end-to-end wiring test: USER_MESSAGE through full module graph
//
// Uses a ScriptProvider stub (first complete() returns a fs_read tool_call,
// second returns a final text answer) wired via build_agent() onto a single
// Blackboard with a live ToolRunner subprocess; asserts TOOL_RESULT contains
// the file payload and ASSISTANT_MESSAGE carries the final answer — exercises
// Arbiter, LLMModule, ToolRunner, and the native fs_read binary together.

#include <gtest/gtest.h>
#include <fstream>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
using namespace hades;
// 1st complete() -> fs_read(path=<temp>); 2nd -> final answer "done reading".
struct ScriptProvider : Provider {
  std::string path; int n=0;
  explicit ScriptProvider(std::string p):path(std::move(p)){}
  LlmResponse complete(const LlmRequest&) override {
    LlmResponse r;
    if(n++==0) r.tool_call = {{"id","c1"},{"name","fs_read"},{"arguments",{{"path",path}}}};
    else r.text = "done reading";
    return r;
  }
};
TEST(E2E, ChatReadsFileViaToolThenAnswers) {
  const std::string path = ::testing::TempDir()+"/e2e.txt";
  { std::ofstream f(path); f << "PAYLOAD42"; }
  Block tool; tool.section="Tool"; tool.name="fs"; tool.kv["native"]=FS_READ_BIN;
  Blackboard bb;
  auto agent = build_agent(bb, std::make_unique<ScriptProvider>(path), {tool}, {}, "m");
  std::string answer, toolContent;
  bb.subscribe("ASSISTANT_MESSAGE",[&](const Entry& e){ if(e.value.is_string()) answer=e.value.get<std::string>(); });
  bb.subscribe("TOOL_RESULT",[&](const Entry& e){ toolContent=e.value.value("content",nlohmann::json::object()).value("content",""); });
  bb.post("USER_MESSAGE","read the file","chat"); bb.pump();
  EXPECT_EQ(toolContent, "PAYLOAD42");   // the LIVE fs-read subprocess read our temp file
  EXPECT_EQ(answer, "done reading");      // arbiter looped tool-result -> llm -> final answer
}
