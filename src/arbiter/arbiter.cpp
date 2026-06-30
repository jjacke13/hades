// src/arbiter/arbiter.cpp — decision core: USER_MESSAGE -> LLM -> act -> gate
//
// Subscribes to USER_MESSAGE, LLM_RESPONSE, TOOL_RESULT, and CONFIRM_RESPONSE
// on the Blackboard. Per turn: posts LLM_REQUEST with conversation history and
// tool specs; on LLM_RESPONSE builds an Action, runs it through registered
// Objectives (veto / confirm gate), then drives TOOL_REQUEST or
// ASSISTANT_MESSAGE; loops tool results back via start_turn() up to kMaxSteps.

#include "hades/arbiter.h"
#include "hades/blackboard.h"
#include "hades/prompt.h"   // read_memory_layer
#include "hades/session_id.h"   // make_session_id + unique_fresh_path (NEW_SESSION rotation)
#include "hades/session_history.h"   // read_session_jsonl (shared tolerant parse)
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
namespace hades {

static constexpr int kMaxSteps = 25;

// Push a message onto the in-memory conversation AND, when a session path is set, durably
// append it as one JSON line. IO errors are swallowed (a disk hiccup must not crash the turn —
// the message still lives in history_; resume just won't have the latest line). With no path
// set this is pure push_back, identical to the pre-resume behavior.
void Arbiter::append_history(const nlohmann::json& msg) {
  history_.push_back(msg);
  if (session_path_.empty()) return;
  std::error_code ec;
  const std::filesystem::path p(session_path_);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream f(session_path_, std::ios::app);
  // dump(indent=-1, indent_char=' ', ensure_ascii=false, error_handler=replace): invalid UTF-8
  // bytes in content become U+FFFD instead of throwing json::type_error.316 (which would unwind
  // through Blackboard::pump() and abort the turn). Compact one-line output for valid UTF-8.
  if (f) f << msg.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
}

// Reload a session jsonl into history_ on resume. Tolerant like load_memories: a blank or
// corrupt line (e.g. a truncated trailing line from a mid-append crash) is skipped, never thrown.
// windowed_history_() assumes well-formed history; load_history therefore sanitizes BOTH boundary
// orphans from a mid-pair crash (invalid to providers): a LEADING {role:tool} (a tool result whose
// owning assistant tool_calls was lost) and a TRAILING {role:assistant, tool_calls} (an assistant
// tool-call whose following tool result was lost) — so history_ opens AND closes on a clean
// user/assistant-answer boundary.
void Arbiter::load_history() {
  if (session_path_.empty()) return;
  // Tolerant parse shared with the GET /history display reader (skip blank/corrupt/partial lines).
  auto loaded = read_session_jsonl(session_path_);
  history_.insert(history_.end(), loaded.begin(), loaded.end());
  // Drop leading orphan tool message(s); the window must begin on user/assistant (or be empty).
  while (!history_.empty() && history_.front().value("role", "") == "tool")
    history_.erase(history_.begin());
  // Mid-pair crash: a truncated/lost tool-result line can leave a trailing assistant(tool_calls)
  // (an assistant tool-call with no following tool result). The next user turn would form
  // [assistant(tool_calls), user], which providers reject — drop the trailing orphan.
  while (!history_.empty() &&
         history_.back().value("role", "") == "assistant" &&
         history_.back().contains("tool_calls"))
    history_.pop_back();
}

void Arbiter::on_attach(Blackboard& bb) {
  // SAFETY: bb_ is non-owning; Arbiter outlives the Blackboard subscription
  // (Launcher clears modules before bb destruct).
  bb_ = &bb;
  bb.subscribe("USER_MESSAGE", [this](const Entry& e) {
    if (!e.value.is_string()) return;  // ignore malformed user input
    append_history({{"role", "user"}, {"content", e.value}});
    steps_ = 0;
    ++turn_epoch_;   // a new user turn: later LLM_RESPONSEs stamped with prior epochs are stale
    bb_->post("MODE", "EXECUTING", "arbiter");
    start_turn();
  });
  bb.subscribe("LLM_RESPONSE", [this](const Entry& e) { on_llm_response(e); });
  bb.subscribe("TOOL_RESULT", [this](const Entry& e) { on_tool_result(e); });
  bb.subscribe("CONFIRM_RESPONSE", [this](const Entry& e) { on_confirm(e); });
  // A front-end abandons a turn (run_until timeout) by posting TURN_ABANDONED. Bumping the
  // epoch invalidates the abandoned turn's in-flight LLM_RESPONSE (dropped by on_llm_response's
  // freshness gate when it lands after this), and clearing the pending confirm prevents a
  // confirm-gated action from the abandoned turn surviving into the next one (same reset as
  // on_confirm). NOTE: tools run SYNCHRONOUSLY today so no stale TOOL_RESULT can exist; when
  // tool-offload lands, extend this epoch+abandonment pattern to TOOL_RESULT as well.
  bb.subscribe("TURN_ABANDONED", [this](const Entry&) {
    ++turn_epoch_;
    clear_pending();
  });
  // `/new` (REPL) posts NEW_SESSION to start a FRESH session mid-run: drop the in-memory
  // conversation and rotate session_path_ to a brand-new file so subsequent appends land there,
  // leaving the prior session intact on disk. Bumping turn_epoch_ (like TURN_ABANDONED) makes a
  // brand-new turn context: any in-flight LLM_RESPONSE stamped with the old epoch is dropped by
  // the freshness gate instead of landing in the fresh session. The id generator is injectable
  // (test seam); in prod it is unset and falls back to make_session_id().
  bb.subscribe("NEW_SESSION", [this](const Entry&) {
    history_.clear();
    clear_pending();
    const std::string id = id_gen_ ? id_gen_() : make_session_id();
    // Collision-safe rotation (same as the initial resolve_session_path): if dir/<id>.jsonl already
    // exists — e.g. two `/new` within one wall-clock second — take the first free `-N` suffix so the
    // fresh session never merges into an existing file. With no sessions_dir (test/no-dir path) there
    // is nowhere to rotate to: clear session_path_ so post-/new turns are in-memory-only rather than
    // silently appending to the OLD session file.
    if (!sessions_dir_.empty())
      session_path_ = unique_fresh_path(sessions_dir_, id);
    else
      session_path_.clear();
    ++turn_epoch_;
  });
}

void Arbiter::clear_pending() {
  pending_ = nullptr;
  pending_msg_ = nullptr;
}

// Bound the per-turn LLM request to history_budget_chars_: return the most-recent suffix of
// history_ whose cumulative serialized size fits the budget. The FULL history stays in memory and
// on disk — only the request is trimmed (this also caps an otherwise-unbounded history_ in a long
// live session). Two invariants: (1) never send ZERO history when history exists — the single
// most-recent message is always included even if it alone exceeds the budget; (2) never begin the
// window on an orphaned {role:tool} message (a tool result with no preceding assistant tool_calls
// is invalid to most providers) — advance past any leading tool message(s) onto a user/assistant
// boundary. Sizing uses the same fail-soft dump as append_history so a bad-UTF-8 message can't
// throw here.
std::vector<nlohmann::json> Arbiter::windowed_history_() const {
  const std::size_t n = history_.size();
  if (n == 0) return {};
  // Walk newest -> oldest, accumulating dump sizes; include while within budget. s is the window
  // start; it stays at n (the sentinel) until the first message is included, which guarantees the
  // most-recent message is always kept even if it alone exceeds the budget.
  std::size_t s = n;
  double total = 0.0;
  for (std::size_t i = n; i-- > 0;) {
    const double sz = static_cast<double>(
        history_[i].dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).size());
    if (s != n && total + sz > history_budget_chars_) break;  // adding this would overflow; stop
    total += sz;
    s = i;
  }
  // Never begin on an orphaned tool result (provider-invalid). Advance past leading
  // tool messages; if that consumes everything (tail is a tool), back up onto the
  // owning assistant(tool_calls) so the window is the valid [assistant(tc), tool] pair.
  while (s < n && history_[s].value("role", "") == "tool") ++s;
  if (s >= n) {
    s = n - 1;
    while (s > 0 && history_[s].value("role", "") == "tool") --s;
  }
  return std::vector<nlohmann::json>(history_.begin() + static_cast<std::ptrdiff_t>(s),
                                     history_.end());
}

void Arbiter::start_turn() {
  nlohmann::json tools = nlohmann::json::array();
  for (auto& t : tools_)
    tools.push_back({{"name", t.name}, {"description", t.description}, {"schema", t.schema}});
  // Leading system message = static SOUL/USER prompt + the live core-memory layer, re-read
  // from disk every turn (so the agent's pin_fact edits show up the same session). Built fresh
  // each turn; never stored in history_.
  nlohmann::json messages = nlohmann::json::array();
  std::string sys = system_prompt_;
  if (!memory_path_.empty()) {
    std::string core = read_memory_layer(memory_path_);
    if (!core.empty()) {
      if (!sys.empty()) sys += "\n\n";
      sys += core;
    }
  }
  if (!sys.empty())
    messages.push_back({{"role", "system"}, {"content", sys}});
  for (const auto& m : windowed_history_()) messages.push_back(m);

  // Inject dynamically retrieved memory as an ephemeral {role:system} block immediately
  // before the last user message. Recomputed from the Blackboard each turn and never
  // stored in history_, so it refreshes per turn and never accumulates stale memory.
  if (auto mem = bb_->get("RETRIEVED_MEMORY");
      mem && mem->value.is_string() && !mem->value.get<std::string>().empty()) {
    nlohmann::json block = {{"role", "system"},
                            {"content", "Relevant memories:\n" + mem->value.get<std::string>()}};
    int last_user = -1;
    for (int i = 0; i < static_cast<int>(messages.size()); ++i)
      if (messages[i].value("role", "") == "user") last_user = i;
    if (last_user >= 0) messages.insert(messages.begin() + last_user, block);
  }

  // The epoch is carried unchanged through tool-loop continuations (start_turn is re-called from
  // on_tool_result WITHOUT bumping it), so a within-turn LLM round-trip keeps the current epoch.
  bb_->post("LLM_REQUEST",
            {{"messages", messages}, {"tools", tools}, {"model", model_}, {"epoch", turn_epoch_}},
            "arbiter");
}

void Arbiter::on_llm_response(const Entry& e) {
  const auto& v = e.value;
  if (!v.is_object()) return;  // malformed response: ignore, never throw
  // Freshness gate: a response stamped with a superseded turn epoch (e.g. a timed-out turn's
  // worker completing late) is dropped, never applied to the current turn. Tool-loop
  // continuations share the current turn's epoch and so are NOT dropped.
  const std::uint64_t ep = v.value("epoch", static_cast<std::uint64_t>(0));
  if (ep != turn_epoch_) {
    bb_->post("DROPPED_STALE_LLM_RESPONSE", {{"epoch", ep}, {"current", turn_epoch_}}, "arbiter");
    return;
  }
  Action act;
  nlohmann::json assistant_msg;
  if (v.contains("tool_call") && v["tool_call"].is_object()) {
    const auto& tc = v["tool_call"];
    act.kind = Action::Kind::ToolCall;
    act.tool = tc.value("name", "");
    act.args = tc.contains("arguments") && tc["arguments"].is_object()
                   ? tc["arguments"]
                   : nlohmann::json::object();
    act.tool_id = tc.value("id", "");
    assistant_msg = {
        {"role", "assistant"},
        {"content", nullptr},
        {"tool_calls",
         nlohmann::json::array(
             {{{"id", act.tool_id},
               {"type", "function"},
               {"function", {{"name", act.tool}, {"arguments", act.args.dump()}}}}})}};
  } else {
    act.kind = Action::Kind::Answer;
    act.text = v.value("text", "");
    assistant_msg = {{"role", "assistant"}, {"content", act.text}};
  }
  bb_->post("NEXT_ACTION",
            {{"kind", static_cast<int>(act.kind)}, {"tool", act.tool}, {"text", act.text}},
            "arbiter");
  dispatch_or_gate(act, assistant_msg);
}

void Arbiter::dispatch_or_gate(const Action& act, const nlohmann::json& assistant_msg) {
  // Objectives are consulted in registration order; the first to demand a
  // confirm (needs_confirm) or hard-veto wins and short-circuits dispatch.
  for (auto& o : objectives_) {
    if (!o->active(*bb_)) continue;
    // FAIL CLOSED on any objective exception (e.g. a non-string LLM arg reaching a buggy
    // veto): treat it as a hard block rather than letting it unwind pump() and crash the bus.
    VetoResult v;
    try {
      v = o->veto(*bb_, act);
    } catch (const std::exception& ex) {
      v = VetoResult{true, std::string("objective error: ") + ex.what(), false};
    } catch (...) {
      v = VetoResult{true, "objective error: unknown exception", false};
    }
    if (v.vetoed && v.needs_confirm) {
      pending_ = nlohmann::json{{"kind", static_cast<int>(act.kind)},
                                {"tool", act.tool},
                                {"args", act.args},
                                {"tool_id", act.tool_id}};
      pending_msg_ = assistant_msg;
      bb_->post("CONFIRM_REQUEST",
                {{"id", act.tool_id}, {"prompt", v.reason}, {"action", pending_}}, "arbiter");
      return;
    }
    if (v.vetoed) {
      bb_->post("ASSISTANT_MESSAGE", "[blocked: " + v.reason + "]", "arbiter");
      return;
    }
  }
  append_history(assistant_msg);
  if (act.kind == Action::Kind::ToolCall) {
    bb_->post("TOOL_REQUEST", {{"id", act.tool_id}, {"tool", act.tool}, {"args", act.args}},
              "arbiter");
  } else {
    bb_->post("ASSISTANT_MESSAGE", act.text, "arbiter");
  }
}

void Arbiter::on_tool_result(const Entry& e) {
  const auto& v = e.value;
  if (!v.is_object()) return;  // malformed tool result: ignore
  const auto content = v.contains("content") ? v["content"] : nlohmann::json::object();
  // History push BEFORE the guard so the assistant/tool message pair is always preserved.
  append_history({{"role", "tool"},
                  {"tool_call_id", v.value("id", "")},
                  {"content", content.dump()}});
  if (++steps_ > kMaxSteps) {
    bb_->post("ASSISTANT_MESSAGE", "[stopped: reached max tool steps]", "arbiter");
    return;
  }
  start_turn();  // feed the result back to the LLM, continuing the turn
}

void Arbiter::on_confirm(const Entry& e) {
  if (pending_.is_null()) return;  // no action awaiting confirmation
  if (pending_.value("tool_id","") != e.value.value("id","")) return;  // stale/mismatched confirm
  const auto& v = e.value;
  bool approved = v.is_object() && v.value("approved", false);
  if (approved) {
    append_history(pending_msg_);
    bb_->post("TOOL_REQUEST",
              {{"id", pending_.value("tool_id", "")},
               {"tool", pending_.value("tool", "")},
               {"args", pending_.contains("args") ? pending_["args"] : nlohmann::json::object()}},
              "arbiter");
  } else {
    bb_->post("ASSISTANT_MESSAGE", "[declined by user]", "arbiter");
  }
  clear_pending();
}

}  // namespace hades
