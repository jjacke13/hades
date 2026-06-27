// include/hades/objective/stay_on_budget.h — hard-veto Objective over USD cap
//
// StayOnBudget reads BUDGET_SPENT_USD from the Blackboard and issues a hard
// veto when cumulative spend meets or exceeds the configured cap_usd.
// Consulted by the Arbiter alongside AvoidDestructive before every action.

#pragma once
#include "hades/objective.h"
namespace hades {
class StayOnBudget : public Objective {
public:
  explicit StayOnBudget(double cap_usd);
  std::string type() const override { return "stay_on_budget"; }
  VetoResult  veto(const Blackboard& bb, const Action& a) const override;
private:
  double cap_;
};
}  // namespace hades
