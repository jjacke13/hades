// src/behaviors/standard_behaviors.cpp — the small standing Objectives (helm behaviors)
//
// Merged (2026-07-04 src reorg): stay_on_budget (USD hard cap) + avoid_destructive
// (destructive-pattern confirm backstop) + peer_loop_guard (no onward ask_agent in a
// peer-driven turn). Objectives are competing goals of ONE agent — MOOS behaviors;
// the big scoped one (capability_policy) keeps its own file next door.

#include <array>
#include "hades/objective/stay_on_budget.h"
#include "hades/blackboard.h"
#include "hades/objective/avoid_destructive.h"
#include "hades/objective/peer_loop_guard.h"

// ── USD budget hard cap (was src/objective/stay_on_budget.cpp) ──────────────────────────
namespace hades {
StayOnBudget::StayOnBudget(double cap): cap_(cap) {}
VetoResult StayOnBudget::veto(const Blackboard& bb, const Action&) const {
  double spent = 0.0;
  if (auto e = bb.get("BUDGET_SPENT_USD"); e && e->value.is_number())
    spent = e->value.get<double>();
  if(spent>=cap_) return {true, "budget cap $"+std::to_string(cap_)+" reached", false};
  return {};
}
}  // namespace hades

// ── destructive-pattern confirm backstop (was src/objective/avoid_destructive.cpp) ──────────────────────────
namespace hades {
VetoResult AvoidDestructive::veto(const Blackboard&, const Action& a) const {
  if(a.kind!=Action::Kind::ToolCall) return {};
  // File-mutating tools always need confirmation: an overwrite is data loss, and the
  // args (path/content) won't match the shell-command patterns below.
  static const std::array<const char*,1> mutating_tools={"write_file"};
  for(const char* t: mutating_tools) if(a.tool==t)
    return {true, std::string("writes/overwrites a file via tool: ")+a.tool, true};
  std::string hay=a.tool+" "+a.args.dump();
  static const std::array<const char*,9> pat={
    "rm -rf","rm -r","mkfs","dd if=",":(){","> /dev/","shutdown","reboot","chmod -R 000"};
  // Best-effort heuristic against naive/accidental destructive commands; paired with a human confirm gate. Not an adversarial security boundary.
  for(auto p: pat) if(hay.find(p)!=std::string::npos)
    return {true, std::string("possibly destructive action: ")+p, true};
  return {};
}
}  // namespace hades

// ── no onward ask_agent in a peer-driven turn (was src/objective/peer_loop_guard.cpp) ──────────────────────────
namespace hades {
VetoResult PeerLoopGuard::veto(const Blackboard& bb, const Action& a) const {
  if (a.kind != Action::Kind::ToolCall || a.tool != "ask_agent") return {};
  auto e = bb.get("TURN_ORIGIN");
  if (e && e->value.is_string() && e->value.get<std::string>().rfind("peer:", 0) == 0)
    return {true, "peer-driven turn cannot ask another agent (loop guard)", false};
  return {};
}
}  // namespace hades
