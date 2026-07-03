// src/objective/peer_loop_guard.cpp — hard-veto ask_agent when TURN_ORIGIN is peer:*
#include "hades/objective/peer_loop_guard.h"
#include "hades/blackboard.h"
namespace hades {
VetoResult PeerLoopGuard::veto(const Blackboard& bb, const Action& a) const {
  if (a.kind != Action::Kind::ToolCall || a.tool != "ask_agent") return {};
  auto e = bb.get("TURN_ORIGIN");
  if (e && e->value.is_string() && e->value.get<std::string>().rfind("peer:", 0) == 0)
    return {true, "peer-driven turn cannot ask another agent (loop guard)", false};
  return {};
}
}  // namespace hades
