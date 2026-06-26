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
