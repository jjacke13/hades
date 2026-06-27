// src/objective/stay_on_budget.cpp — hard-veto Objective: USD budget cap
//
// Implements StayOnBudget::veto(): reads BUDGET_SPENT_USD from the Blackboard
// and returns a non-confirm hard veto once cumulative spend reaches the cap.
// Consulted by Arbiter before every Action; posts nothing to the Blackboard itself.

#include "hades/objective/stay_on_budget.h"
#include "hades/blackboard.h"
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
