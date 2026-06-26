#include <gtest/gtest.h>
#include "hades/arbiter.h"
#include "hades/blackboard.h"
#include "hades/objective/avoid_destructive.h"
using namespace hades;
TEST(Arbiter, PlainAnswerReachesChat) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  std::string out; std::vector<nlohmann::json> reqs;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ reqs.push_back(e.value); });
  bb.subscribe("ASSISTANT_MESSAGE",[&](const Entry& e){ out=e.value; });
  bb.post("USER_MESSAGE","hello","chat"); bb.pump();
  ASSERT_EQ(reqs.size(),1u);                       // turn started
  bb.post("LLM_RESPONSE", {{"text","hi there"}}, "llm"); bb.pump();
  EXPECT_EQ(out,"hi there");
}
TEST(Arbiter, ToolCallRoundTrips) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_tools({ ToolSpec{"fs_read","",{}} });
  nlohmann::json toolreq;
  bb.subscribe("TOOL_REQUEST",[&](const Entry& e){ toolreq=e.value; });
  std::string out; bb.subscribe("ASSISTANT_MESSAGE",[&](const Entry& e){ out=e.value; });
  bb.post("USER_MESSAGE","read it","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text",""},{"tool_call",{{"id","c1"},{"name","fs_read"},
          {"arguments",{{"path","/a"}}}}}}, "llm"); bb.pump();
  ASSERT_EQ(toolreq["tool"],"fs_read");
  bb.post("TOOL_RESULT", {{"id","c1"},{"ok",true},{"content",{{"content","FILE"}}}}, "tool_runner"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text","the file says FILE"}}, "llm"); bb.pump();
  EXPECT_EQ(out,"the file says FILE");
}
TEST(Arbiter, DestructiveToolCallIsConfirmGated) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.add_objective(std::make_unique<AvoidDestructive>());
  nlohmann::json confirm; bool tool_called=false;
  bb.subscribe("CONFIRM_REQUEST",[&](const Entry& e){ confirm=e.value; });
  bb.subscribe("TOOL_REQUEST",[&](const Entry&){ tool_called=true; });
  bb.post("USER_MESSAGE","wipe","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text",""},{"tool_call",{{"id","c1"},{"name","shell"},
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
  bb.post("LLM_RESPONSE", {{"text",""},{"tool_call",{{"id","c1"},{"name","shell"},
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
  bb.post("LLM_RESPONSE", {{"text",""},{"tool_call",{{"id","c1"},{"name","fs_read"},
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
    bb.post("LLM_RESPONSE", {{"text",""},{"tool_call",{{"id","c"},{"name","fs_read"},{"arguments",{{"path","/a"}}}}}}, "llm"); bb.pump();
    bb.post("TOOL_RESULT", {{"id","c"},{"ok",true},{"content",nlohmann::json::object()}}, "tool_runner"); bb.pump();
  }
  EXPECT_NE(last.find("max tool steps"), std::string::npos);
}
