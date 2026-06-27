// include/hades/module.h — abstract base for all Blackboard participants
//
// Module is the lifecycle interface the Launcher calls: on_start() receives the
// config Block and a Blackboard reference for setup; on_attach() is called after
// all modules are constructed so subscriptions see sibling posts. Concrete
// modules — LLMModule, ToolRunner, ChatModule, Arbiter — implement this base.

#pragma once
#include <string>
#include "hades/config.h"
namespace hades {
class Blackboard;
class Module {
public:
  virtual ~Module() = default;
  virtual std::string type() const = 0;
  virtual void on_start(const Block&, Blackboard&) {}
  virtual void on_attach(Blackboard&) {}
  virtual std::string build_report() const { return ""; }
};
}  // namespace hades
