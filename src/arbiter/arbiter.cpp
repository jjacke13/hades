// src/arbiter/arbiter.cpp — decision core: USER_MESSAGE -> LLM -> act -> gate
//
// Subscribes to USER_MESSAGE, LLM_RESPONSE, TOOL_RESULT, and CONFIRM_RESPONSE
// on the Blackboard. Per turn: posts LLM_REQUEST with conversation history and
// tool specs; on LLM_RESPONSE builds an Action, runs it through registered
// Objectives (veto / confirm gate), then drives TOOL_REQUEST or
// ASSISTANT_MESSAGE; loops tool results back via start_turn() up to kMaxSteps.

#include "hades/arbiter.h"
#include "hades/blackboard.h"
namespace hades {

static constexpr int kMaxSteps = 25;

void Arbiter::on_attach(Blackboard& bb) {
  // SAFETY: bb_ is non-owning; Arbiter outlives the Blackboard subscription
  // (Launcher clears modules before bb destruct).
  bb_ = &bb;
  bb.subscribe("USER_MESSAGE", [this](const Entry& e) {
    if (!e.value.is_string()) return;  // ignore malformed user input
    history_.push_back({{"role", "user"}, {"content", e.value}});
    steps_ = 0;
    bb_->post("MODE", "EXECUTING", "arbiter");
    start_turn();
  });
  bb.subscribe("LLM_RESPONSE", [this](const Entry& e) { on_llm_response(e); });
  bb.subscribe("TOOL_RESULT", [this](const Entry& e) { on_tool_result(e); });
  bb.subscribe("CONFIRM_RESPONSE", [this](const Entry& e) { on_confirm(e); });
}

void Arbiter::start_turn() {
  nlohmann::json tools = nlohmann::json::array();
  for (auto& t : tools_)
    tools.push_back({{"name", t.name}, {"description", t.description}, {"schema", t.schema}});
  bb_->post("LLM_REQUEST",
            {{"messages", history_}, {"tools", tools}, {"model", model_}}, "arbiter");
}

void Arbiter::on_llm_response(const Entry& e) {
  const auto& v = e.value;
  if (!v.is_object()) return;  // malformed response: ignore, never throw
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
    auto v = o->veto(*bb_, act);
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
  history_.push_back(assistant_msg);
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
  history_.push_back({{"role", "tool"},
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
    history_.push_back(pending_msg_);
    bb_->post("TOOL_REQUEST",
              {{"id", pending_.value("tool_id", "")},
               {"tool", pending_.value("tool", "")},
               {"args", pending_.contains("args") ? pending_["args"] : nlohmann::json::object()}},
              "arbiter");
  } else {
    bb_->post("ASSISTANT_MESSAGE", "[declined by user]", "arbiter");
  }
  pending_ = nullptr;
  pending_msg_ = nullptr;
}

}  // namespace hades
