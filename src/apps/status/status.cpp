// src/apps/status/status.cpp — aggregate turn stats onto AGENT_STATUS (see the header)
#include "hades/module/status_module.h"
#include <cstdio>
#include "hades/blackboard.h"
namespace hades {

std::string format_status(long long ctx_tokens, double spent_usd, long long turn,
                          const std::string& model) {
  std::string tok;
  if (ctx_tokens >= 1000) {
    char t[32];
    std::snprintf(t, sizeof t, "%.1fk", static_cast<double>(ctx_tokens) / 1000.0);
    tok = t;
  } else {
    tok = std::to_string(ctx_tokens);
  }
  char buf[160];
  std::snprintf(buf, sizeof buf, "[ctx %s tok \xC2\xB7 $%.4f \xC2\xB7 turn %lld", tok.c_str(),
                spent_usd, turn);
  std::string out = buf;
  if (!model.empty()) out += " \xC2\xB7 " + model;
  return out + "]";
}

void StatusModule::post_status_() {
  bb_->post("AGENT_STATUS",
            {{"ctx_tokens", ctx_tokens_},
             {"spent_usd", spent_usd_},
             {"turn", turn_},
             {"model", model_},
             {"line", format_status(ctx_tokens_, spent_usd_, turn_, model_)}},
            "status");
}

void StatusModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  // Every handler is type-defensive: pump-thread handlers must never throw, and any of
  // these payloads can arrive malformed off the bus.
  bb.subscribe("USER_MESSAGE", [this](const Entry&) { ++turn_; });
  bb.subscribe("LLM_REQUEST", [this](const Entry& e) {
    if (e.value.is_object() && e.value.contains("model") && e.value["model"].is_string())
      model_ = e.value["model"].get<std::string>();
  });
  bb.subscribe("LLM_RESPONSE", [this](const Entry& e) {
    if (!e.value.is_object()) return;
    const auto& pt = e.value.contains("prompt_tokens") ? e.value["prompt_tokens"]
                                                       : nlohmann::json();
    const auto& ct = e.value.contains("completion_tokens") ? e.value["completion_tokens"]
                                                           : nlohmann::json();
    if (pt.is_number() || ct.is_number())
      ctx_tokens_ = (pt.is_number() ? pt.get<long long>() : 0) +
                    (ct.is_number() ? ct.get<long long>() : 0);
    post_status_();
  });
  // Spend accrues in the LLMModule's own LLM_RESPONSE handler and is posted after it —
  // subscribing it directly (and re-posting) makes the line current regardless of the
  // sibling subscriber order.
  bb.subscribe("BUDGET_SPENT_USD", [this](const Entry& e) {
    if (!e.value.is_number()) return;
    spent_usd_ = e.value.get<double>();
    post_status_();
  });
  // /new rotates the conversation: context + turn describe the conversation and reset;
  // spend is process-cumulative (the budget objective's view) and the model is unchanged.
  bb.subscribe("NEW_SESSION", [this](const Entry&) {
    ctx_tokens_ = 0;
    turn_ = 0;
    post_status_();
  });
}
}  // namespace hades
