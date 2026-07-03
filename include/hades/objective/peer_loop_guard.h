// include/hades/objective/peer_loop_guard.h — no onward delegation from a peer-driven turn
//
// Kills the A↔B ask deadlock: A's ask_agent blocks A's pump holding A's TurnGate; if B's
// peer-driven turn could ask A back, both would wait forever. This standing safety behavior
// hard-vetoes ask_agent whenever the current turn's TURN_ORIGIN is "peer:<name>" (posted by
// the BridgeModule before the USER_MESSAGE). AUTO-REGISTERED by wiring whenever the bridge
// module is rostered — the bridge brings its own guard; it is NOT a manifest objective.
// Absent/malformed TURN_ORIGIN (front-ends post "human") -> allow. The wire `hops` field is
// the belt-and-braces second layer at the HTTP boundary.
#pragma once
#include "hades/objective.h"
namespace hades {
class PeerLoopGuard : public Objective {
 public:
  std::string type() const override { return "peer_loop_guard"; }
  VetoResult veto(const Blackboard& bb, const Action& a) const override;
};
}  // namespace hades
