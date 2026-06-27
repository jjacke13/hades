// include/hades/objective/avoid_destructive.h — veto+confirm for destructive tools
//
// AvoidDestructive inspects the pending Action and issues a veto-with-confirm
// when the tool call is classified as destructive (e.g. writes, deletes).
// The Arbiter gates the action through ChatModule's CONFIRM_REQUEST flow.

#pragma once
#include "hades/objective.h"
namespace hades {
class AvoidDestructive : public Objective {
public:
  std::string type() const override { return "avoid_destructive"; }
  VetoResult  veto(const Blackboard& bb, const Action& a) const override;
};
}  // namespace hades
