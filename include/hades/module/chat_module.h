#pragma once
#include <iosfwd>
#include "hades/module.h"
namespace hades {
class Blackboard;
class ChatModule : public Module {
public:
  std::string type() const override { return "chat"; }
  void on_attach(Blackboard& bb) override;
  void run_repl(std::istream& in, std::ostream& out);  // blocks until EOF or /quit
private:
  Blackboard* bb_  = nullptr;
  std::ostream* out_ = nullptr;
  std::istream* in_  = nullptr;
};
}  // namespace hades
