#include "hades/objective/stay_on_budget.h"
#include "hades/blackboard.h"
namespace hades {
StayOnBudget::StayOnBudget(double cap): cap_(cap) {}
VetoResult StayOnBudget::veto(const Blackboard& bb, const Action&) const {
  auto e=bb.get("BUDGET_SPENT_USD"); double spent=e? e->value.get<double>():0.0;
  if(spent>=cap_) return {true, "budget cap $"+std::to_string(cap_)+" reached", false};
  return {};
}
}  // namespace hades
