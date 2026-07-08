// include/hades/arbiter.h — the "helm"; drives the per-turn agent decision loop
//
// Arbiter subscribes USER_MESSAGE -> posts LLM_REQUEST; on LLM_RESPONSE it
// constructs an Action, runs it through each Objective's veto(), then either
// posts TOOL_REQUEST / ASSISTANT_MESSAGE or a CONFIRM_REQUEST (soft veto).
// Tool results loop back via TOOL_RESULT; the turn ends when the LLM emits an
// Answer or the max-steps guard fires. Event-driven via the Blackboard; no threads.

#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "hades/entry.h"
#include "hades/history_budget.h"   // kDefaultHistoryBudgetChars
#include "hades/module.h"
#include "hades/objective.h"
#include "hades/llm/provider.h"   // ToolSpec
namespace hades {
class Blackboard;
class Arbiter : public Module {
public:
  std::string type() const override { return "arbiter"; }
  void on_attach(Blackboard& bb) override;
  void add_objective(std::unique_ptr<Objective> o) { objectives_.push_back(std::move(o)); }
  void set_tools(std::vector<ToolSpec> t) { tools_ = std::move(t); }
  void set_model(std::string m) { model_ = std::move(m); }
  // Assembled system prompt (SOUL/USER/MEMORY); prepended as messages[0] each turn.
  void set_system_prompt(std::string s) { system_prompt_ = std::move(s); }
  // Path to the always-on core-memory file (memory_file). Re-read every turn so pins are live.
  void set_memory_path(std::string p) { memory_path_ = std::move(p); }
  // Path to the per-session conversation jsonl (one history_ message per line). When set,
  // append_history persists each message as it is added; load_history reloads it on resume.
  void set_session_path(std::string p) { session_path_ = std::move(p); }
  // Directory under which a `/new` rotation creates the fresh session jsonl (dir/<id>.jsonl). Set
  // alongside the initial session path so NEW_SESSION can rotate within the same dir; empty -> no
  // rotation (session_path_ stays put, history still clears).
  void set_session_dir(std::string d) { sessions_dir_ = std::move(d); }
  // Inject the id generator used by NEW_SESSION to name the rotated file (test seam: deterministic
  // ids without a clock). Unset -> the handler falls back to make_session_id() (prod default).
  void set_id_generator(std::function<std::string()> g) { id_gen_ = std::move(g); }
  // Reload a session jsonl into history_ (tolerant: skip blank/corrupt lines). No-op if unset.
  void load_history();
  // Cap (chars) on the cumulative serialized size of history_ sent in ONE LLM request. The full
  // history stays in memory + on disk; only the per-turn request is bounded (default 120000).
  // Ignores non-positive values so a misparsed/absent config can't disable the window.
  void set_history_budget_chars(double c) { if (c > 0) history_budget_chars_ = c; }
  // Push a message onto history_ and, if a session path is set, durably append it to disk.
  void append_history(const nlohmann::json& msg);
  // Test observability: number of messages currently held in history_.
  std::size_t history_size() const { return history_.size(); }

private:
  void start_turn();
  // Most-recent suffix of history_ within history_budget_chars_, beginning on a valid (non-orphan
  // {role:tool}) boundary. Built fresh each turn; the leading system/memory messages are added by
  // start_turn() OUTSIDE this budget (they are not part of history_).
  std::vector<nlohmann::json> windowed_history_() const;
  void on_llm_response(const Entry&);
  void on_tool_result(const Entry&);
  void on_confirm(const Entry&);
  void dispatch_or_gate(const Action&, const nlohmann::json& assistant_msg);
  // Staleness guard: record a tracked file op (fs_read/edit_file/write_file) so its TOOL_RESULT
  // version can be harvested. Called just before BOTH TOOL_REQUEST post sites (direct + confirm).
  void track_file_op_(const std::string& id, const std::string& tool, const nlohmann::json& args);
  // Reset the single pending-confirm slot. Shared by on_confirm (confirm resolved) and the
  // TURN_ABANDONED handler (turn dropped) so the two resets can never drift apart.
  void clear_pending();

  Blackboard* bb_ = nullptr;
  std::vector<nlohmann::json> history_;
  std::vector<std::unique_ptr<Objective>> objectives_;
  std::vector<ToolSpec> tools_;
  std::string model_;
  std::string system_prompt_;   // prepended as a {role:system} message each turn (may be empty)
  std::string memory_path_;     // live core-memory file; re-read each turn into the system message
  std::string session_path_;    // per-session conversation jsonl; append-per-message when set
  std::string sessions_dir_;    // dir for a `/new` rotation (dir/<id>.jsonl); empty -> no rotation
  std::function<std::string()> id_gen_;  // NEW_SESSION id source (test seam); null -> make_session_id
  double history_budget_chars_ = kDefaultHistoryBudgetChars;  // per-turn LLM-request size cap
  // single pending confirm slot; the turn is suspended until it resolves (no second pending can form).
  nlohmann::json pending_;      // action awaiting confirm
  nlohmann::json pending_msg_;  // assistant tool_calls msg awaiting confirm
  std::map<std::string, nlohmann::json> peer_vars_;   // PEER.* latest values (bridge discovery/share)
  // Staleness guard: last-observed content version per file (canonical path -> 16-hex hash),
  // harvested from fs_read/edit_file/write_file TOOL_RESULTs; injected as expect_version into
  // edit_file/write_file dispatches. In-memory only (a restart clears it — degrade to unguarded).
  std::map<std::string, std::string> file_versions_;
  std::map<std::string, std::string> pending_file_ops_;   // tool-call id -> canonical path
  int steps_ = 0;               // tool-call steps within the current turn (reset on USER_MESSAGE)
  // Per-user-turn freshness stamp: bumped on each USER_MESSAGE (NOT on tool-loop continuations),
  // stamped onto every LLM_REQUEST, and matched on LLM_RESPONSE so a timed-out turn's late
  // response is dropped instead of answering the next prompt.
  std::uint64_t turn_epoch_ = 0;
};
}  // namespace hades
