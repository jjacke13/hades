// include/hades/module/status_module.h — turn-stats aggregator (uProcessWatch analogue)
//
// Watches the traffic every turn already produces (LLM_REQUEST/RESPONSE, BUDGET_SPENT_USD,
// USER_MESSAGE, NEW_SESSION) and posts one AGENT_STATUS latest-value: {ctx_tokens, spent_usd,
// turn, model, line}. ctx_tokens = prompt+completion tokens of the LAST LLM call — the real
// measure of the conversation the model is carrying. Front-ends render `line` (chat prints it
// dim after each reply); the raw fields are the seam for a web/telegram status later. Data
// producer stays decoupled from the surface — the terminal has ONE writer (ChatModule), this
// module never touches stdout.
#pragma once
#include <string>
#include "hades/module.h"
namespace hades {
class Blackboard;

// Canonical one-line rendering: "[ctx 12.4k tok · $0.0372 · turn 9 · gpt-5.5]"
// (>=1000 tokens shown as one-decimal k; model segment omitted when unknown).
std::string format_status(long long ctx_tokens, double spent_usd, long long turn,
                          const std::string& model);

class StatusModule : public Module {
public:
  std::string type() const override { return "status"; }
  void on_attach(Blackboard& bb) override;

private:
  void post_status_();
  Blackboard* bb_ = nullptr;
  long long ctx_tokens_ = 0;
  double spent_usd_ = 0.0;
  long long turn_ = 0;
  std::string model_;
};
}  // namespace hades
