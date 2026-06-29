// tests/test_arbiter.cpp — unit tests for Arbiter decision loop on the Blackboard
//
// Covers the full per-turn flow: USER_MESSAGE -> LLM_REQUEST; LLM_RESPONSE ->
// ASSISTANT_MESSAGE (plain answer) or TOOL_REQUEST (tool-call round-trip);
// AvoidDestructive confirm-gating via CONFIRM_REQUEST/RESPONSE; history pairing
// (assistant tool_calls + matching tool role entries) for the next LLM call; and
// the max-steps guard that terminates runaway tool loops.

#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include "hades/arbiter.h"
#include "hades/blackboard.h"
#include "hades/objective/avoid_destructive.h"
using namespace hades;
TEST(Arbiter, PrependsSystemPromptToLlmRequest) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_system_prompt("you are hades");
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ req=e.value; });
  bb.post("USER_MESSAGE","hi","chat"); bb.pump();
  ASSERT_FALSE(req.is_null());
  const auto& msgs = req["messages"];
  ASSERT_GE(msgs.size(), 2u);
  EXPECT_EQ(msgs[0]["role"], "system");
  EXPECT_EQ(msgs[0]["content"], "you are hades");
  EXPECT_EQ(msgs[1]["role"], "user");
}
TEST(Arbiter, NoSystemMessageWhenPromptEmpty) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ req=e.value; });
  bb.post("USER_MESSAGE","hi","chat"); bb.pump();
  EXPECT_EQ(req["messages"][0]["role"], "user");   // no leading system message
}
TEST(Arbiter, PlainAnswerReachesChat) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  std::string out; std::vector<nlohmann::json> reqs;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ reqs.push_back(e.value); });
  bb.subscribe("ASSISTANT_MESSAGE",[&](const Entry& e){ out=e.value; });
  bb.post("USER_MESSAGE","hello","chat"); bb.pump();
  ASSERT_EQ(reqs.size(),1u);                       // turn started
  bb.post("LLM_RESPONSE", {{"text","hi there"},{"epoch",1}}, "llm"); bb.pump();
  EXPECT_EQ(out,"hi there");
}
TEST(Arbiter, ToolCallRoundTrips) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_tools({ ToolSpec{"fs_read","",{}} });
  nlohmann::json toolreq;
  bb.subscribe("TOOL_REQUEST",[&](const Entry& e){ toolreq=e.value; });
  std::string out; bb.subscribe("ASSISTANT_MESSAGE",[&](const Entry& e){ out=e.value; });
  bb.post("USER_MESSAGE","read it","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text",""},{"epoch",1},{"tool_call",{{"id","c1"},{"name","fs_read"},
          {"arguments",{{"path","/a"}}}}}}, "llm"); bb.pump();
  ASSERT_EQ(toolreq["tool"],"fs_read");
  bb.post("TOOL_RESULT", {{"id","c1"},{"ok",true},{"content",{{"content","FILE"}}}}, "tool_runner"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text","the file says FILE"},{"epoch",1}}, "llm"); bb.pump();
  EXPECT_EQ(out,"the file says FILE");
}
TEST(Arbiter, DestructiveToolCallIsConfirmGated) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.add_objective(std::make_unique<AvoidDestructive>());
  nlohmann::json confirm; bool tool_called=false;
  bb.subscribe("CONFIRM_REQUEST",[&](const Entry& e){ confirm=e.value; });
  bb.subscribe("TOOL_REQUEST",[&](const Entry&){ tool_called=true; });
  bb.post("USER_MESSAGE","wipe","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text",""},{"epoch",1},{"tool_call",{{"id","c1"},{"name","shell"},
          {"arguments",{{"cmd","rm -rf /"}}}}}}, "llm"); bb.pump();
  ASSERT_FALSE(confirm.is_null());
  EXPECT_FALSE(tool_called);                       // gated, not executed
  bb.post("CONFIRM_RESPONSE", {{"id","c1"},{"approved",false}}, "chat"); bb.pump();
  EXPECT_FALSE(tool_called);                       // declined → still not executed
}
TEST(Arbiter, ConfirmApproveExecutesStashedTool) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.add_objective(std::make_unique<AvoidDestructive>());
  nlohmann::json toolreq; bool tool_called=false;
  bb.subscribe("TOOL_REQUEST",[&](const Entry& e){ tool_called=true; toolreq=e.value; });
  bb.post("USER_MESSAGE","wipe","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text",""},{"epoch",1},{"tool_call",{{"id","c1"},{"name","shell"},
          {"arguments",{{"cmd","rm -rf /"}}}}}}, "llm"); bb.pump();
  ASSERT_FALSE(tool_called);                       // gated until approval
  bb.post("CONFIRM_RESPONSE", {{"id","c1"},{"approved",true}}, "chat"); bb.pump();
  EXPECT_TRUE(tool_called);                         // approved -> executes
  EXPECT_EQ(toolreq["tool"],"shell");
  EXPECT_EQ(toolreq["args"]["cmd"],"rm -rf /");
}
TEST(Arbiter, ToolRoundTripPairsHistoryForNextLlmCall) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_tools({ ToolSpec{"fs_read","",{}} });
  std::vector<nlohmann::json> reqs;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ reqs.push_back(e.value); });
  bb.post("USER_MESSAGE","read it","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text",""},{"epoch",1},{"tool_call",{{"id","c1"},{"name","fs_read"},
          {"arguments",{{"path","/a"}}}}}}, "llm"); bb.pump();
  bb.post("TOOL_RESULT", {{"id","c1"},{"ok",true},{"content",{{"content","DATA"}}}}, "tool_runner"); bb.pump();
  ASSERT_GE(reqs.size(), 2u);
  const auto& msgs = reqs.back()["messages"];
  bool paired=false;
  for (std::size_t i=0;i+1<msgs.size();++i){
    if (msgs[i].value("role","")=="assistant" && msgs[i].contains("tool_calls")
        && !msgs[i]["tool_calls"].empty() && msgs[i]["tool_calls"][0].value("id","")=="c1"
        && msgs[i+1].value("role","")=="tool" && msgs[i+1].value("tool_call_id","")=="c1")
      paired=true;
  }
  EXPECT_TRUE(paired);   // assistant tool_calls immediately followed by matching tool result
}
TEST(Arbiter, StopsAfterMaxToolSteps) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_tools({ ToolSpec{"fs_read","",{}} });
  std::string last;
  bb.subscribe("ASSISTANT_MESSAGE",[&](const Entry& e){ last=e.value; });
  bb.post("USER_MESSAGE","loop","chat"); bb.pump();
  for(int i=0;i<30;i++){
    bb.post("LLM_RESPONSE", {{"text",""},{"epoch",1},{"tool_call",{{"id","c"},{"name","fs_read"},{"arguments",{{"path","/a"}}}}}}, "llm"); bb.pump();
    bb.post("TOOL_RESULT", {{"id","c"},{"ok",true},{"content",nlohmann::json::object()}}, "tool_runner"); bb.pump();
  }
  EXPECT_NE(last.find("max tool steps"), std::string::npos);
}

TEST(Arbiter, InjectsRetrievedMemoryBeforeUserMessage) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ req=e.value; });
  bb.post("RETRIEVED_MEMORY","- user likes tea","memory");
  bb.post("USER_MESSAGE","what do i like","chat"); bb.pump();
  const auto& msgs = req["messages"];
  int memIdx=-1, userIdx=-1;
  for (int i=0;i<static_cast<int>(msgs.size());++i){
    if (msgs[i].value("role","")=="system" &&
        msgs[i].value("content","").rfind("Relevant memories:",0)==0) memIdx=i;
    if (msgs[i].value("role","")=="user") userIdx=i;
  }
  ASSERT_GE(memIdx,0); ASSERT_GE(userIdx,0);
  EXPECT_LT(memIdx,userIdx);                                  // memory block precedes the user turn
  EXPECT_NE(msgs[memIdx]["content"].get<std::string>().find("tea"), std::string::npos);
}

TEST(Arbiter, NoMemoryBlockWhenRetrievedEmpty) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ req=e.value; });
  bb.post("RETRIEVED_MEMORY","","memory");
  bb.post("USER_MESSAGE","hi","chat"); bb.pump();
  for (const auto& m : req["messages"])
    EXPECT_FALSE(m.value("role","")=="system" &&
                 m.value("content","").rfind("Relevant memories:",0)==0);
}

TEST(Arbiter, MemoryBlockIsEphemeralNotInHistory) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  std::vector<nlohmann::json> reqs;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ reqs.push_back(e.value); });
  bb.post("RETRIEVED_MEMORY","- ephemeral fact","memory");
  bb.post("USER_MESSAGE","first","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text","ok"},{"epoch",1}}, "llm"); bb.pump();   // turn 1 ends
  bb.post("RETRIEVED_MEMORY","","memory");                       // nothing relevant now
  bb.post("USER_MESSAGE","second","chat"); bb.pump();           // turn 2
  bool leaked=false;
  for (const auto& m : reqs.back()["messages"])
    if (m.value("role","")=="system" && m.value("content","").rfind("Relevant memories:",0)==0)
      leaked=true;
  EXPECT_FALSE(leaked);   // prior turn's memory block did not persist into history_
}

TEST(Arbiter, MemoryBlockReflectsOnlyCurrentTurnValue) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  std::vector<nlohmann::json> reqs;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ reqs.push_back(e.value); });
  bb.post("RETRIEVED_MEMORY","- OLDFACT alpha","memory");
  bb.post("USER_MESSAGE","first","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text","ok"},{"epoch",1}}, "llm"); bb.pump();   // turn 1 ends
  bb.post("RETRIEVED_MEMORY","- NEWFACT beta","memory");        // different memory now
  bb.post("USER_MESSAGE","second","chat"); bb.pump();           // turn 2
  std::string dump = reqs.back()["messages"].dump();
  EXPECT_NE(dump.find("NEWFACT"), std::string::npos);   // current turn's memory present
  EXPECT_EQ(dump.find("OLDFACT"), std::string::npos);   // prior turn's memory did NOT persist
}

TEST(Arbiter, FoldsLiveCoreMemoryIntoSystemMessage) {
  const std::string f = ::testing::TempDir() + "/core_arb.md";
  { std::ofstream(f) << "- user prefers metric units\n"; }
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_system_prompt("you are hades");
  a.set_memory_path(f);
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ req=e.value; });
  bb.post("USER_MESSAGE","hi","chat"); bb.pump();
  ASSERT_FALSE(req.is_null());
  const auto& sys = req["messages"][0];
  EXPECT_EQ(sys["role"], "system");
  EXPECT_NE(sys["content"].get<std::string>().find("you are hades"), std::string::npos);
  EXPECT_NE(sys["content"].get<std::string>().find("metric units"), std::string::npos);
}

TEST(Arbiter, CoreMemoryIsLiveReloadedEachTurn) {
  const std::string f = ::testing::TempDir() + "/core_live.md";
  { std::ofstream(f) << "- fact one\n"; }
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_memory_path(f);
  std::vector<nlohmann::json> reqs;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ reqs.push_back(e.value); });
  bb.post("USER_MESSAGE","first","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text","ok"},{"epoch",1}}, "llm"); bb.pump();   // turn 1 ends
  { std::ofstream(f, std::ios::app) << "- fact two\n"; }        // agent pins something mid-session
  bb.post("USER_MESSAGE","second","chat"); bb.pump();           // turn 2
  std::string dump = reqs.back()["messages"][0]["content"].get<std::string>();
  EXPECT_NE(dump.find("fact one"), std::string::npos);
  EXPECT_NE(dump.find("fact two"), std::string::npos);          // new pin visible same session
}

TEST(Arbiter, StaleEpochResponseIsDropped) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  std::vector<std::string> answers;
  std::vector<std::uint64_t> req_epochs;
  bb.subscribe("ASSISTANT_MESSAGE",[&](const Entry& e){ answers.push_back(e.value); });
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ req_epochs.push_back(e.value.value("epoch", static_cast<std::uint64_t>(0))); });
  bb.post("USER_MESSAGE","first","chat");  bb.pump();   // turn epoch 1
  bb.post("USER_MESSAGE","second","chat"); bb.pump();   // turn epoch 2
  // A stale response from the first (timed-out) turn must be dropped, never applied.
  bb.post("LLM_RESPONSE", {{"text","late answer"},{"epoch",1}}, "llm"); bb.pump();
  for (const auto& s : answers) EXPECT_NE(s, "late answer");   // dropped on epoch mismatch
  // The fresh response for the current turn is applied.
  bb.post("LLM_RESPONSE", {{"text","real"},{"epoch",2}}, "llm"); bb.pump();
  ASSERT_FALSE(answers.empty());
  EXPECT_EQ(answers.back(), "real");
  // The emitted LLM_REQUESTs carried increasing per-turn epochs.
  ASSERT_GE(req_epochs.size(), 2u);
  EXPECT_EQ(req_epochs[0], static_cast<std::uint64_t>(1));
  EXPECT_EQ(req_epochs[1], static_cast<std::uint64_t>(2));
}

TEST(Arbiter, NoCoreMemoryWhenPathUnsetAndNoPrompt) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ req=e.value; });
  bb.post("USER_MESSAGE","hi","chat"); bb.pump();
  EXPECT_EQ(req["messages"][0]["role"], "user");   // no leading system message at all
}
