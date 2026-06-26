#include "hades/objective/avoid_destructive.h"
#include <array>
namespace hades {
VetoResult AvoidDestructive::veto(const Blackboard&, const Action& a) const {
  if(a.kind!=Action::Kind::ToolCall) return {};
  std::string hay=a.tool+" "+a.args.dump();
  static const std::array<const char*,8> pat={
    "rm -rf","rm -r","mkfs","dd if=",":(){","> /dev/","shutdown","reboot"};
  for(auto p: pat) if(hay.find(p)!=std::string::npos)
    return {true, std::string("possibly destructive action: ")+p, true};
  return {};
}
}  // namespace hades
