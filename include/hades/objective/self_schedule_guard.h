// include/hades/objective/self_schedule_guard.h — no self-scheduling from a heartbeat-driven turn
//
// Contains the runaway-recursion risk: a heartbeat tick (TURN_ORIGIN "heartbeat:<name>") may create
// new scheduled tasks ONLY when the operator set allow_self_schedule=true. A human-origin turn is
// never blocked (you set a goal, the agent schedules its own monitors). Guards ONLY the create path
// (schedule_task); list_tasks/cancel_task are always allowed. PeerLoopGuard sibling; auto-registered
// by wiring when heartbeat + the schedule_task tool are both present.
#pragma once
#include "hades/objective.h"
namespace hades {
class SelfScheduleGuard : public Objective {
 public:
  explicit SelfScheduleGuard(bool allow_self_schedule) : allow_(allow_self_schedule) {}
  std::string type() const override { return "self_schedule_guard"; }
  VetoResult veto(const Blackboard& bb, const Action& a) const override;

 private:
  bool allow_ = false;
};
}  // namespace hades
