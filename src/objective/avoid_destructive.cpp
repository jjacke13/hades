#include "hades/objective/avoid_destructive.h"
#include <array>
namespace hades {
VetoResult AvoidDestructive::veto(const Blackboard&, const Action& a) const {
  if(a.kind!=Action::Kind::ToolCall) return {};
  std::string hay=a.tool+" "+a.args.dump();
  static const std::array<const char*,9> pat={
    "rm -rf","rm -r","mkfs","dd if=",":(){","> /dev/","shutdown","reboot","chmod -R 000"};
  // Best-effort heuristic against naive/accidental destructive commands; paired with a human confirm gate. Not an adversarial security boundary.
  for(auto p: pat) if(hay.find(p)!=std::string::npos)
    return {true, std::string("possibly destructive action: ")+p, true};
  return {};
}
}  // namespace hades
