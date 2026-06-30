// tests/test_arbiter.cpp — unit tests for Arbiter decision loop on the Blackboard
//
// Covers the full per-turn flow: USER_MESSAGE -> LLM_REQUEST; LLM_RESPONSE ->
// ASSISTANT_MESSAGE (plain answer) or TOOL_REQUEST (tool-call round-trip);
// AvoidDestructive confirm-gating via CONFIRM_REQUEST/RESPONSE; history pairing
// (assistant tool_calls + matching tool role entries) for the next LLM call; and
// the max-steps guard that terminates runaway tool loops.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <gtest/gtest.h>
#include "hades/arbiter.h"
#include "hades/blackboard.h"
#include "hades/objective/avoid_destructive.h"
using namespace hades;

namespace {
// An objective that throws from veto() — proves the Arbiter's dispatch_or_gate try/catch
// fails closed instead of unwinding the bus (defense-in-depth, final-review C-fix).
struct ThrowingObjective : Objective {
  std::string type() const override { return "throwing"; }
  VetoResult veto(const Blackboard&, const Action&) const override {
    throw std::runtime_error("boom");
  }
};
}  // namespace
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

// Closes the turn-epoch DISPATCH-ORDERING hole via the abandonment model.
//
// The Arbiter bumps turn_epoch_ only when a USER_MESSAGE is DISPATCHED, so a stale
// LLM_RESPONSE{epoch:E} enqueued AHEAD of the next USER_MESSAGE{->E+1} would (without a
// signal) be dispatched while turn_epoch_ is still E and wrongly answer the new prompt.
// The robust fix: when a turn is abandoned (front-end run_until timeout), the front-end
// posts TURN_ABANDONED; the Arbiter bumps turn_epoch_ on it, so the late worker response
// for the abandoned turn is then dropped by the existing freshness gate (ep != turn_epoch_).
//
// Ordering: at idle-timeout the queue is empty, so TURN_ABANDONED is enqueued first and any
// late worker LLM_RESPONSE{E} lands strictly after it — the Arbiter bumps the epoch before
// the stale response is dispatched. This test models that: abandon turn 1, THEN deliver the
// stale epoch-1 response, and assert it is dropped while a fresh response still works.
TEST(Arbiter, StaleResponseAfterAbandonmentIsDropped) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  std::vector<std::string> answers;
  std::vector<std::uint64_t> req_epochs;
  bb.subscribe("ASSISTANT_MESSAGE",[&](const Entry& e){ answers.push_back(e.value); });
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ req_epochs.push_back(e.value.value("epoch", static_cast<std::uint64_t>(0))); });
  bb.post("USER_MESSAGE","first","chat"); bb.pump();   // turn epoch 1; LLM_REQUEST{1} emitted, left unanswered
  ASSERT_FALSE(req_epochs.empty());
  const std::uint64_t abandoned_epoch = req_epochs.back();   // the live epoch of the turn we abandon
  // The front-end abandons the turn (run_until timed out): it posts TURN_ABANDONED, which the
  // Arbiter must treat as bumping the turn epoch (invalidating the in-flight response).
  bb.post("TURN_ABANDONED", nlohmann::json::object(), "chat"); bb.pump();
  // The abandoned turn's worker finally completes and posts its (now stale) response.
  bb.post("LLM_RESPONSE", {{"text","late answer for turn 1"},{"epoch",abandoned_epoch}}, "llm"); bb.pump();
  for (const auto& s : answers) EXPECT_NE(s, "late answer for turn 1");   // dropped: abandoned epoch != current
  // The next user turn proceeds normally; its fresh response (stamped with the live epoch) IS applied.
  bb.post("USER_MESSAGE","second","chat"); bb.pump();
  const std::uint64_t live_epoch = req_epochs.back();        // epoch the Arbiter now expects
  bb.post("LLM_RESPONSE", {{"text","real"},{"epoch",live_epoch}}, "llm"); bb.pump();
  ASSERT_FALSE(answers.empty());
  EXPECT_EQ(answers.back(), "real");
  // Epoch numbering: ++ on USER_MESSAGE and on TURN_ABANDONED -> 1 (first), 2 (abandon), 3 (second).
  EXPECT_EQ(abandoned_epoch, static_cast<std::uint64_t>(1));
  EXPECT_EQ(live_epoch, static_cast<std::uint64_t>(3));
}

// TURN_ABANDONED must also clear a pending confirm: a confirm-gated action from the abandoned
// turn must NOT execute if a late CONFIRM_RESPONSE for it arrives in the next turn. Modeled on
// DestructiveToolCallIsConfirmGated (an AvoidDestructive objective + a destructive tool_call).
TEST(Arbiter, TurnAbandonedClearsPendingConfirm) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.add_objective(std::make_unique<AvoidDestructive>());
  nlohmann::json confirm; bool tool_called=false;
  bb.subscribe("CONFIRM_REQUEST",[&](const Entry& e){ confirm=e.value; });
  bb.subscribe("TOOL_REQUEST",[&](const Entry&){ tool_called=true; });
  bb.post("USER_MESSAGE","wipe","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text",""},{"epoch",1},{"tool_call",{{"id","c1"},{"name","shell"},
          {"arguments",{{"cmd","rm -rf /"}}}}}}, "llm"); bb.pump();
  ASSERT_FALSE(confirm.is_null());                 // gated -> awaiting confirmation (pending_ set)
  EXPECT_FALSE(tool_called);
  // The turn is abandoned before the user answers the confirm prompt.
  bb.post("TURN_ABANDONED", nlohmann::json::object(), "chat"); bb.pump();
  // A late approval for the abandoned action must NOT resurrect it (pending_ was cleared).
  bb.post("CONFIRM_RESPONSE", {{"id","c1"},{"approved",true}}, "chat"); bb.pump();
  EXPECT_FALSE(tool_called);                        // pending cleared on abandonment -> no execution
}

// Defense-in-depth (final-review C-fix): an objective that THROWS from veto() must not crash the
// single-threaded bus. dispatch_or_gate catches it and FAILS CLOSED (hard block), so the tool
// never runs and a "[blocked: objective error: …]" answer surfaces — pump() never throws.
TEST(Arbiter, ObjectiveExceptionFailsClosedWithoutCrashing) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.add_objective(std::make_unique<ThrowingObjective>());
  a.set_tools({ ToolSpec{"fs_read","",{}} });
  std::string out; bool tool_called=false;
  bb.subscribe("ASSISTANT_MESSAGE",[&](const Entry& e){ out=e.value; });
  bb.subscribe("TOOL_REQUEST",[&](const Entry&){ tool_called=true; });
  bb.post("USER_MESSAGE","read it","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text",""},{"epoch",1},{"tool_call",{{"id","c1"},{"name","fs_read"},
          {"arguments",{{"path","/a"}}}}}}, "llm");
  EXPECT_NO_THROW(bb.pump());                              // objective throw must NOT escape pump
  EXPECT_FALSE(tool_called);                               // failed closed -> tool never ran
  EXPECT_NE(out.find("blocked"), std::string::npos);       // hard block surfaced
  EXPECT_NE(out.find("objective error"), std::string::npos);
}

TEST(Arbiter, NoCoreMemoryWhenPathUnsetAndNoPrompt) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ req=e.value; });
  bb.post("USER_MESSAGE","hi","chat"); bb.pump();
  EXPECT_EQ(req["messages"][0]["role"], "user");   // no leading system message at all
}

// --- Session resume (Task 1): conversation persistence to a session jsonl ---

// With a session path set, append_history both pushes to history_ AND appends one
// JSON line per message (verbatim dump) — the user turn and the assistant answer.
TEST(Arbiter, AppendHistoryWritesFileWhenPathSet) {
  const std::string path = ::testing::TempDir() + "/sess_write.jsonl";
  std::filesystem::remove(path);                   // start clean (TempDir may persist)
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_session_path(path);
  bb.post("USER_MESSAGE","hi","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text","hi there"},{"epoch",1}}, "llm"); bb.pump();
  std::ifstream f(path);
  ASSERT_TRUE(f.good());
  std::vector<std::string> lines; std::string line;
  while (std::getline(f, line)) if (!line.empty()) lines.push_back(line);
  ASSERT_EQ(lines.size(), 2u);                     // user msg + assistant msg, one per line
  const auto l1 = nlohmann::json::parse(lines[0]);
  const auto l2 = nlohmann::json::parse(lines[1]);
  EXPECT_EQ(l1.value("role",""), "user");
  EXPECT_EQ(l1.value("content",""), "hi");
  EXPECT_EQ(l2.value("role",""), "assistant");
  EXPECT_EQ(l2.value("content",""), "hi there");
}

// Fail-soft: a message whose content carries invalid UTF-8 bytes must NOT throw out of
// append_history (msg.dump() would throw json::type_error.316 by default) — it would unwind
// through Blackboard::pump() and abort the turn. The replace error handler emits U+FFFD for
// the bad bytes; the message is still persisted (one line written).
TEST(Arbiter, AppendHistoryDoesNotThrowOnInvalidUtf8) {
  const std::string path = ::testing::TempDir() + "/sess_badutf8.jsonl";
  std::filesystem::remove(path);                   // start clean (TempDir may persist)
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_session_path(path);
  nlohmann::json msg;
  msg["role"] = "user";
  msg["content"] = std::string("bad\xff\xfe" "byte");  // invalid UTF-8 bytes in content
  EXPECT_NO_THROW(a.append_history(msg));           // replace handler: never throws
  std::ifstream f(path);
  ASSERT_TRUE(f.good());
  std::vector<std::string> lines; std::string line;
  while (std::getline(f, line)) if (!line.empty()) lines.push_back(line);
  ASSERT_EQ(lines.size(), 1u);                      // the message was persisted (bad bytes replaced)
  const auto l1 = nlohmann::json::parse(lines[0]);  // line is valid JSON (U+FFFD substituted)
  EXPECT_EQ(l1.value("role",""), "user");
}

// Backward-compat: with NO session path, append_history is push-only — history_ grows
// but NO file is ever created (existing behavior preserved for the 175 prior tests).
TEST(Arbiter, AppendHistoryPushOnlyWhenNoPath) {
  const std::string path = ::testing::TempDir() + "/sess_nopath.jsonl";
  std::filesystem::remove(path);
  Blackboard bb; Arbiter a; a.on_attach(bb);       // no set_session_path
  bb.post("USER_MESSAGE","hi","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text","hi there"},{"epoch",1}}, "llm"); bb.pump();
  EXPECT_EQ(a.history_size(), 2u);                 // both messages held in memory
  EXPECT_FALSE(std::filesystem::exists(path));     // but nothing persisted to disk
}

// --- Session resume (Task 2): per-turn overflow window in start_turn() ---

// With a tiny budget, the LLM request carries only the most-recent SUFFIX of history_ whose
// cumulative serialized size fits the budget (the full history stays in memory/on disk; only the
// REQUEST is bounded). The window must be a recent suffix (oldest turns trimmed), within ~budget
// (over by at most the single always-included first message), and must NOT begin on a {role:tool}.
TEST(Arbiter, HistoryWindowSendsRecentSuffixWithinBudget) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_history_budget_chars(200);
  std::vector<nlohmann::json> reqs;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ reqs.push_back(e.value); });
  // Drive several plain user/assistant round trips so history_ grows well past 200 chars.
  // Distinctive, similarly-sized contents so we can tell the recent suffix from the old head.
  const int turns = 6;
  for (int t = 1; t <= turns; ++t) {
    bb.post("USER_MESSAGE", "user turn number " + std::to_string(t) + " padding xxxxx", "chat");
    bb.pump();
    bb.post("LLM_RESPONSE", {{"text","assistant reply number " + std::to_string(t) + " yyyyy"},
                            {"epoch", t}}, "llm");
    bb.pump();
  }
  // One more user turn; its start_turn LLM_REQUEST carries the largest history (all prior pairs +
  // this user msg). Capture it as the window under test (no response needed for this turn).
  bb.post("USER_MESSAGE", "user turn number 7 padding xxxxx", "chat"); bb.pump();
  ASSERT_FALSE(reqs.empty());
  const auto& msgs = reqs.back()["messages"];
  // No system prompt / no memory set -> messages ARE the windowed history (no leading block).
  ASSERT_FALSE(msgs.empty());
  // (a) recent suffix: the newest user msg is present; the earliest turn-1 messages are trimmed.
  const std::string dump = msgs.dump();
  EXPECT_NE(dump.find("number 7"), std::string::npos);   // most-recent present
  EXPECT_EQ(dump.find("number 1"), std::string::npos);   // earliest trimmed (suffix, not whole)
  // (b) cumulative size within budget, over by at most the first (always-included) message.
  double total = 0.0, first = 0.0;
  for (std::size_t i = 0; i < msgs.size(); ++i) {
    const double sz = static_cast<double>(msgs[i].dump().size());
    if (i == 0) first = sz;
    total += sz;
  }
  EXPECT_LE(total - first, 200.0);
  // (c) the window NEVER begins on an orphan {role:tool} message.
  const std::string r0 = msgs[0].value("role","");
  EXPECT_NE(r0, "tool");
  EXPECT_TRUE(r0 == "user" || r0 == "assistant");
}

// Tool-pairing invariant: when the budget cut would land such that a {role:tool} result becomes
// the window's first message (its assistant tool_calls trimmed out), the advance must drop the
// orphaned tool too, so the window opens on a user or a complete assistant turn. The oldest turn
// here is a tool round-trip whose assistant(tool_calls) carries a LARGE argument; a small budget
// excludes that big assistant message while the following small tool result would otherwise lead.
TEST(Arbiter, HistoryWindowNeverStartsOnOrphanToolMessage) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_tools({ ToolSpec{"fs_read","",{}} });
  a.set_history_budget_chars(300);
  std::vector<nlohmann::json> reqs;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ reqs.push_back(e.value); });
  const std::string big(600, 'Z');   // large tool argument -> large assistant(tool_calls) dump
  bb.post("USER_MESSAGE","u1","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text",""},{"epoch",1},{"tool_call",{{"id","c1"},{"name","fs_read"},
          {"arguments",{{"path",big}}}}}}, "llm"); bb.pump();
  bb.post("TOOL_RESULT", {{"id","c1"},{"ok",true},{"content",{{"content","R"}}}}, "tool_runner"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text","answer after tool"},{"epoch",1}}, "llm"); bb.pump();  // turn 1 ends
  // A small second turn; capture its request: history = u1, asst(LARGE tool_calls), tool, answer, u2.
  bb.post("USER_MESSAGE","u2 small","chat"); bb.pump();
  ASSERT_FALSE(reqs.empty());
  const auto& msgs = reqs.back()["messages"];
  ASSERT_FALSE(msgs.empty());
  // The window must NOT begin on the orphaned tool result.
  const std::string r0 = msgs[0].value("role","");
  EXPECT_NE(r0, "tool");
  EXPECT_TRUE(r0 == "user" || r0 == "assistant");
  // The big assistant(tool_calls) message was trimmed (budget excluded it).
  EXPECT_EQ(msgs.dump().find(big), std::string::npos);
  // Any tool message remaining must still be preceded by its assistant tool_calls (no orphan).
  for (std::size_t i = 0; i < msgs.size(); ++i)
    if (msgs[i].value("role","") == "tool") {
      ASSERT_GT(i, 0u);
      EXPECT_TRUE(msgs[i-1].value("role","") == "assistant" && msgs[i-1].contains("tool_calls"));
    }
}

// Backward-compat: under the default budget (120000) a small history is sent WHOLE — no trimming.
// Locks the no-op-below-budget guarantee that keeps the existing arbiter tests unchanged.
TEST(Arbiter, SmallHistorySentWholeUnderDefaultBudget) {
  Blackboard bb; Arbiter a; a.on_attach(bb);   // default budget kDefaultHistoryBudgetChars
  std::vector<nlohmann::json> reqs;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ reqs.push_back(e.value); });
  bb.post("USER_MESSAGE","alpha one","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text","beta two"},{"epoch",1}}, "llm"); bb.pump();
  bb.post("USER_MESSAGE","gamma three","chat"); bb.pump();   // last request: full history
  ASSERT_FALSE(reqs.empty());
  const auto& msgs = reqs.back()["messages"];
  ASSERT_EQ(msgs.size(), 3u);                   // user(alpha) + assistant(beta) + user(gamma), no trim
  EXPECT_EQ(msgs[0].value("content",""), "alpha one");
  EXPECT_EQ(msgs[1].value("content",""), "beta two");
  EXPECT_EQ(msgs[2].value("content",""), "gamma three");
}

// Edge: the most-recent message IS a {role:tool} result (the Arbiter calls start_turn() right
// after on_tool_result appends one, so during a tool loop the tail is a tool). When the owning
// assistant(tool_calls) + that tool together EXCEED the budget, the minimal VALID unit is the
// [assistant(tool_calls), tool] PAIR — send it over-budget rather than an orphaned [tool] (which
// providers reject with 400). Drive a real tool round-trip so history_ = [user, asst(tc), tool]
// and capture the start_turn LLM_REQUEST fired by on_tool_result (tool is the last message).
TEST(Arbiter, HistoryWindowTrailingToolKeepsItsAssistantPair) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_tools({ ToolSpec{"fs_read","",{}} });
  a.set_history_budget_chars(200);             // small: asst(tc)+tool together exceed it
  std::vector<nlohmann::json> reqs;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ reqs.push_back(e.value); });
  const std::string big(1000, 'D');            // large tool RESULT -> tool message alone > budget
  bb.post("USER_MESSAGE","read it","chat"); bb.pump();
  bb.post("LLM_RESPONSE", {{"text",""},{"epoch",1},{"tool_call",{{"id","c1"},{"name","fs_read"},
          {"arguments",{{"path","/a"}}}}}}, "llm"); bb.pump();
  // TOOL_RESULT appends {role:tool} then start_turn fires with history_=[user, asst(tc), tool];
  // that request is reqs.back() and the tool IS its last history message.
  bb.post("TOOL_RESULT", {{"id","c1"},{"ok",true},{"content",{{"content",big}}}}, "tool_runner"); bb.pump();
  ASSERT_GE(reqs.size(), 2u);
  const auto& msgs = reqs.back()["messages"];   // no system/memory set -> messages ARE the window
  ASSERT_FALSE(msgs.empty());                    // history non-empty -> window must be non-empty
  // The window must open on the owning assistant(tool_calls) — NOT an orphan [tool], NOT empty.
  EXPECT_EQ(msgs[0].value("role",""), "assistant");
  ASSERT_TRUE(msgs[0].contains("tool_calls"));
  EXPECT_FALSE(msgs[0]["tool_calls"].empty());
  EXPECT_EQ(msgs[0]["tool_calls"][0].value("id",""), "c1");
  // The tool result is included right after it -> a valid [assistant(tool_calls), tool] pair.
  ASSERT_GE(msgs.size(), 2u);
  EXPECT_EQ(msgs[1].value("role",""), "tool");
  EXPECT_EQ(msgs[1].value("tool_call_id",""), "c1");
  // Never an orphaned lone tool, and every tool message is preceded by its assistant tool_calls.
  EXPECT_NE(msgs[0].value("role",""), "tool");
  for (std::size_t i = 0; i < msgs.size(); ++i)
    if (msgs[i].value("role","") == "tool") {
      ASSERT_GT(i, 0u);
      EXPECT_TRUE(msgs[i-1].value("role","") == "assistant" && msgs[i-1].contains("tool_calls"));
    }
}

// load_history round-trips a written jsonl into history_ and tolerates a corrupt/truncated
// trailing line (a crash mid-append) — the 3 valid lines load, the garbage tail is skipped.
TEST(Arbiter, LoadHistoryRoundTripsAndToleratesCorruptTail) {
  const std::string path = ::testing::TempDir() + "/sess_load.jsonl";
  {
    std::ofstream f(path, std::ios::trunc);
    f << nlohmann::json({{"role","user"},{"content","one"}}).dump() << "\n";
    f << nlohmann::json({{"role","assistant"},{"content","two"}}).dump() << "\n";
    f << nlohmann::json({{"role","user"},{"content","three"}}).dump() << "\n";
    f << "{\"role\":\"assistant\",\"content\":\"trun";   // truncated tail, no newline
  }
  Arbiter b; b.set_session_path(path);
  EXPECT_NO_THROW(b.load_history());
  EXPECT_EQ(b.history_size(), 3u);                 // 3 valid; corrupt trailing line skipped
}

// load_history sanitizes a LEADING orphan {role:tool}: a resumed jsonl whose first message is a
// tool result (its owning assistant tool_calls lost to a mid-pair crash) is invalid to providers.
// The leading tool line is dropped on load so history_ opens on a user/assistant boundary — and
// the first message of the next LLM request is therefore never a tool.
TEST(Arbiter, LoadHistoryDropsLeadingOrphanTool) {
  const std::string path = ::testing::TempDir() + "/sess_orphan.jsonl";
  {
    std::ofstream f(path, std::ios::trunc);
    f << nlohmann::json({{"role","tool"},{"tool_call_id","x"},{"content","orphan"}}).dump() << "\n";
    f << nlohmann::json({{"role","user"},{"content","hello"}}).dump() << "\n";
    f << nlohmann::json({{"role","assistant"},{"content","hi"}}).dump() << "\n";
  }
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_session_path(path);
  EXPECT_NO_THROW(a.load_history());
  EXPECT_EQ(a.history_size(), 2u);                 // leading orphan tool dropped; user+assistant kept
  // The next LLM request's window must NOT begin on a tool message.
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST",[&](const Entry& e){ req=e.value; });
  bb.post("USER_MESSAGE","next","chat"); bb.pump();
  ASSERT_FALSE(req.is_null());
  const auto& msgs = req["messages"];
  ASSERT_FALSE(msgs.empty());
  EXPECT_NE(msgs[0].value("role",""), "tool");
  EXPECT_EQ(msgs[0].value("role",""), "user");     // opens on the loaded user message
}

// load_history sanitizes a TRAILING orphan {role:assistant, tool_calls:...}: a mid-pair crash that
// loses the tool-result line (the largest line — file/http content) leaves history_ ending on an
// assistant tool_calls with no following tool result. The next user turn would form
// [..., assistant(tool_calls), user] — an interior orphan providers reject (400). The trailing
// orphan is dropped on load so history_ ends on a user/assistant-answer boundary.
TEST(Arbiter, LoadHistoryDropsTrailingOrphanAssistantToolCalls) {
  const std::string path = ::testing::TempDir() + "/sess_trail_orphan.jsonl";
  {
    std::ofstream f(path, std::ios::trunc);
    f << nlohmann::json({{"role","user"},{"content","hi"}}).dump() << "\n";
    f << nlohmann::json({{"role","assistant"},
                         {"tool_calls", nlohmann::json::array({{{"id","c1"}}})}}).dump() << "\n";
    // NO tool result line — simulating the lost (largest) tool-result line of a mid-pair crash.
  }
  Arbiter a; a.set_session_path(path);
  EXPECT_NO_THROW(a.load_history());
  EXPECT_EQ(a.history_size(), 1u);                 // orphan assistant(tool_calls) dropped; user kept
}

// NON-REGRESSION: a normal resume whose tail is an assistant ANSWER (content, NO tool_calls) is
// UNTOUCHED — the trailing-orphan strip must only target assistant-with-tool_calls.
TEST(Arbiter, LoadHistoryKeepsTrailingAssistantAnswer) {
  const std::string path = ::testing::TempDir() + "/sess_trail_answer.jsonl";
  {
    std::ofstream f(path, std::ios::trunc);
    f << nlohmann::json({{"role","user"},{"content","hi"}}).dump() << "\n";
    f << nlohmann::json({{"role","assistant"},{"content","answer"}}).dump() << "\n";
  }
  Arbiter a; a.set_session_path(path);
  EXPECT_NO_THROW(a.load_history());
  EXPECT_EQ(a.history_size(), 2u);                 // normal answer tail kept
}

// NON-REGRESSION: a COMPLETE pair (tail = {role:tool} following the assistant tool_calls) is
// UNTOUCHED — the assistant tool_calls has its matching result, so nothing is an orphan.
TEST(Arbiter, LoadHistoryKeepsCompleteToolPair) {
  const std::string path = ::testing::TempDir() + "/sess_complete_pair.jsonl";
  {
    std::ofstream f(path, std::ios::trunc);
    f << nlohmann::json({{"role","user"},{"content","hi"}}).dump() << "\n";
    f << nlohmann::json({{"role","assistant"},
                         {"tool_calls", nlohmann::json::array({{{"id","c1"}}})}}).dump() << "\n";
    f << nlohmann::json({{"role","tool"},{"tool_call_id","c1"},{"content","result"}}).dump() << "\n";
  }
  Arbiter a; a.set_session_path(path);
  EXPECT_NO_THROW(a.load_history());
  EXPECT_EQ(a.history_size(), 3u);                 // complete pair untouched
}

// --- Session resume (Task 4): /new -> NEW_SESSION clears history_ + rotates the session file ---

// NEW_SESSION (posted by the /new REPL command) clears history_ AND rotates session_path_ to
// dir/<id>.jsonl via the INJECTED id generator (deterministic, no clock). The OLD session file is
// left intact on disk; the next appended message lands in the NEW file. Bumping the turn epoch is
// covered indirectly: the post-/new turn's LLM_RESPONSE must carry the fresh epoch to be applied.
TEST(Arbiter, NewSessionClearsHistoryAndRotatesFile) {
  const std::string dir = ::testing::TempDir() + "/newsess";
  std::filesystem::create_directories(dir);
  const std::string old_path = dir + "/old.jsonl";
  const std::string new_path = dir + "/NEWID.jsonl";
  std::filesystem::remove(old_path);
  std::filesystem::remove(new_path);
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_session_dir(dir);
  a.set_id_generator([] { return std::string("NEWID"); });   // deterministic rotation target
  a.set_session_path(old_path);
  // Drive a turn: history_ becomes non-empty and old.jsonl gets the user + assistant lines.
  bb.post("USER_MESSAGE","hi","chat"); bb.pump();                       // turn epoch -> 1
  bb.post("LLM_RESPONSE", {{"text","hi there"},{"epoch",1}}, "llm"); bb.pump();
  ASSERT_EQ(a.history_size(), 2u);
  // /new -> NEW_SESSION: clears history_ and rotates session_path_ to dir/NEWID.jsonl.
  bb.post("NEW_SESSION", nlohmann::json::object(), "chat"); bb.pump(); // turn epoch -> 2
  EXPECT_EQ(a.history_size(), 0u);                                      // history cleared
  // Drive ANOTHER turn: the new messages must land in NEWID.jsonl, NOT old.jsonl. The fresh epoch
  // (3 = ++ on USER_MESSAGE after the NEW_SESSION bump) proves NEW_SESSION advanced the epoch.
  bb.post("USER_MESSAGE","fresh start","chat"); bb.pump();             // turn epoch -> 3
  bb.post("LLM_RESPONSE", {{"text","reply two"},{"epoch",3}}, "llm"); bb.pump();
  auto read_lines = [](const std::string& p) {
    std::vector<std::string> lines; std::ifstream f(p); std::string l;
    while (std::getline(f, l)) if (!l.empty()) lines.push_back(l);
    return lines;
  };
  // old.jsonl: still exactly the two original lines (left intact, not appended to).
  const auto old_lines = read_lines(old_path);
  ASSERT_EQ(old_lines.size(), 2u);
  EXPECT_EQ(nlohmann::json::parse(old_lines[0]).value("content",""), "hi");
  EXPECT_EQ(nlohmann::json::parse(old_lines[1]).value("content",""), "hi there");
  // NEWID.jsonl: the post-/new turn's two messages (user + assistant).
  const auto new_lines = read_lines(new_path);
  ASSERT_EQ(new_lines.size(), 2u);
  EXPECT_EQ(nlohmann::json::parse(new_lines[0]).value("content",""), "fresh start");
  EXPECT_EQ(nlohmann::json::parse(new_lines[1]).value("content",""), "reply two");
}

// /new rotation must be COLLISION-SAFE like the initial resolve_session_path: if dir/<id>.jsonl
// already exists (e.g. two `/new` within one wall-clock second resolve to the same id), the Arbiter
// rotates to the first free `-N` suffix (dir/<id>-1.jsonl) rather than appending into the existing
// file. Here the injected id generator always returns "DUP" and dir/DUP.jsonl is pre-created; the
// post-/new turn must land in dir/DUP-1.jsonl, leaving the pre-existing dir/DUP.jsonl untouched.
TEST(Arbiter, NewSessionRotationIsCollisionSafe) {
  const std::string dir = ::testing::TempDir() + "/newsess_collision";
  std::filesystem::create_directories(dir);
  const std::string dup_path = dir + "/DUP.jsonl";
  const std::string dup1_path = dir + "/DUP-1.jsonl";
  std::filesystem::remove(dup_path);
  std::filesystem::remove(dup1_path);
  { std::ofstream touch(dup_path); touch << "PREEXISTING\n"; }   // collide: dir/DUP.jsonl exists
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_session_dir(dir);
  a.set_id_generator([] { return std::string("DUP"); });        // every rotation targets id "DUP"
  // /new -> NEW_SESSION: rotation must AVOID the existing DUP.jsonl and pick DUP-1.jsonl.
  bb.post("NEW_SESSION", nlohmann::json::object(), "chat"); bb.pump();   // turn epoch -> 1
  // Drive a turn: the new messages must land in DUP-1.jsonl (collision avoided), NOT DUP.jsonl.
  bb.post("USER_MESSAGE","collide test","chat"); bb.pump();             // turn epoch -> 2
  bb.post("LLM_RESPONSE", {{"text","reply"},{"epoch",2}}, "llm"); bb.pump();
  auto read_lines = [](const std::string& p) {
    std::vector<std::string> lines; std::ifstream f(p); std::string l;
    while (std::getline(f, l)) if (!l.empty()) lines.push_back(l);
    return lines;
  };
  // DUP.jsonl: untouched — still exactly the single pre-existing sentinel line.
  const auto dup_lines = read_lines(dup_path);
  ASSERT_EQ(dup_lines.size(), 1u);
  EXPECT_EQ(dup_lines[0], "PREEXISTING");
  // DUP-1.jsonl: the post-/new turn's two messages landed here (the -N suffix proves collision-safe).
  const auto dup1_lines = read_lines(dup1_path);
  ASSERT_EQ(dup1_lines.size(), 2u);
  EXPECT_EQ(nlohmann::json::parse(dup1_lines[0]).value("content",""), "collide test");
  EXPECT_EQ(nlohmann::json::parse(dup1_lines[1]).value("content",""), "reply");
}

// /new with NO sessions_dir set (test/no-dir path): there is nowhere to rotate to, so NEW_SESSION
// must CLEAR session_path_ — otherwise post-/new turns would silently keep appending to the OLD
// session file. Here a session path is set directly (no dir); after /new the post-/new turn must
// NOT append to the old file (history is in-memory only because session_path_ was cleared).
TEST(Arbiter, NewSessionEmptyDirClearsPath) {
  const std::string dir = ::testing::TempDir() + "/newsess_emptydir";
  std::filesystem::create_directories(dir);
  const std::string old_path = dir + "/old.jsonl";
  std::filesystem::remove(old_path);
  Blackboard bb; Arbiter a; a.on_attach(bb);
  a.set_session_path(old_path);                                  // path set, but NO set_session_dir
  // Drive a turn: old.jsonl gets the user + assistant lines.
  bb.post("USER_MESSAGE","hi","chat"); bb.pump();                       // turn epoch -> 1
  bb.post("LLM_RESPONSE", {{"text","hi there"},{"epoch",1}}, "llm"); bb.pump();
  auto read_lines = [](const std::string& p) {
    std::vector<std::string> lines; std::ifstream f(p); std::string l;
    while (std::getline(f, l)) if (!l.empty()) lines.push_back(l);
    return lines;
  };
  ASSERT_EQ(read_lines(old_path).size(), 2u);                    // old.jsonl has the first turn
  // /new -> NEW_SESSION: no sessions_dir, so session_path_ is cleared (in-memory-only thereafter).
  bb.post("NEW_SESSION", nlohmann::json::object(), "chat"); bb.pump();   // turn epoch -> 2
  // Drive ANOTHER turn: it must NOT append to old.jsonl (path was cleared).
  bb.post("USER_MESSAGE","fresh start","chat"); bb.pump();             // turn epoch -> 3
  bb.post("LLM_RESPONSE", {{"text","reply two"},{"epoch",3}}, "llm"); bb.pump();
  EXPECT_EQ(read_lines(old_path).size(), 2u);                    // old.jsonl untouched (still 2 lines)
}

TEST(Arbiter, MergesKeywordAndSemanticMemoryDeduped) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  std::vector<nlohmann::json> reqs;
  bb.subscribe("LLM_REQUEST", [&](const Entry& e) { reqs.push_back(e.value); });
  bb.post("RETRIEVED_MEMORY", "- shared fact\n- keyword only", "memory");
  bb.post("RETRIEVED_MEMORY_SEMANTIC", "- shared fact\n- semantic only", "embedding_memory");
  bb.post("USER_MESSAGE", "hi", "chat");
  bb.pump();
  ASSERT_FALSE(reqs.empty());
  // find the injected system "Relevant memories:" block
  std::string block;
  for (const auto& m : reqs[0]["messages"])
    if (m.value("role", "") == "system" && m.value("content", "").rfind("Relevant memories:", 0) == 0)
      block = m["content"];
  ASSERT_FALSE(block.empty());
  // shared fact appears once; both unique lines present
  EXPECT_NE(block.find("shared fact"), std::string::npos);
  EXPECT_NE(block.find("keyword only"), std::string::npos);
  EXPECT_NE(block.find("semantic only"), std::string::npos);
  EXPECT_EQ(block.find("shared fact"), block.rfind("shared fact"));  // exactly once
}
TEST(Arbiter, SemanticAbsentIsUnchanged) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  std::vector<nlohmann::json> reqs;
  bb.subscribe("LLM_REQUEST", [&](const Entry& e) { reqs.push_back(e.value); });
  bb.post("RETRIEVED_MEMORY", "- only keyword", "memory");
  bb.post("USER_MESSAGE", "hi", "chat");
  bb.pump();
  std::string block;
  for (const auto& m : reqs[0]["messages"])
    if (m.value("role", "") == "system" && m.value("content", "").rfind("Relevant memories:", 0) == 0)
      block = m["content"];
  EXPECT_EQ(block, "Relevant memories:\n- only keyword");  // identical to pre-embedding behavior
}
TEST(Arbiter, SemanticOnlyWithAbsentKeywordStillInjectsBlock) {
  // Valid path: embedding_memory present but keyword RETRIEVED_MEMORY absent -> the semantic block
  // is still injected (merge_memory_blocks("", sem) == sem).
  Blackboard bb; Arbiter a; a.on_attach(bb);
  std::vector<nlohmann::json> reqs;
  bb.subscribe("LLM_REQUEST", [&](const Entry& e) { reqs.push_back(e.value); });
  bb.post("RETRIEVED_MEMORY_SEMANTIC", "- embedding result", "embedding_memory");
  bb.post("USER_MESSAGE", "hi", "chat");
  bb.pump();
  ASSERT_FALSE(reqs.empty());
  std::string block;
  for (const auto& m : reqs[0]["messages"])
    if (m.value("role", "") == "system" && m.value("content", "").rfind("Relevant memories:", 0) == 0)
      block = m["content"];
  EXPECT_EQ(block, "Relevant memories:\n- embedding result");
}
