// src/objective/avoid_destructive.cpp — confirm-veto Objective: destructive tool patterns
//
// Implements AvoidDestructive::veto(): pattern-matches Action.tool+args against a
// set of destructive shell idioms (rm -rf, mkfs, dd, …) and returns a confirm-veto
// so the Arbiter routes a CONFIRM_REQUEST to ChatModule before proceeding.
// Best-effort heuristic; not an adversarial security boundary.

#include "hades/objective/avoid_destructive.h"
#include <array>
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
