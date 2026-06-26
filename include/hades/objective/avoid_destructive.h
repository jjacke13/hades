#pragma once
#include "hades/objective.h"
namespace hades {
class AvoidDestructive : public Objective {
public:
  std::string type() const override { return "avoid_destructive"; }
  VetoResult  veto(const Blackboard& bb, const Action& a) const override;
};
}  // namespace hades
