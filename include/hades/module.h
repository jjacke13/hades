#pragma once
#include <string>
#include "hades/config.h"
namespace hades {
class Blackboard;
class Module {
public:
  virtual ~Module() = default;
  virtual std::string type() const = 0;
  virtual void on_start(const Block& cfg, Blackboard& bb) {}
  virtual void on_attach(Blackboard& bb) {}
  virtual std::string build_report() const { return ""; }
};
}  // namespace hades
