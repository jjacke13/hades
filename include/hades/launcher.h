#pragma once
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "hades/config.h"
#include "hades/module.h"
namespace hades {
class Blackboard;
struct MalConfig : std::runtime_error { using std::runtime_error::runtime_error; };
class Launcher {
public:
  explicit Launcher(Blackboard& bb);
  ~Launcher();
  using Factory = std::function<std::unique_ptr<Module>()>;
  void register_factory(const std::string& type, Factory f);
  void build(const Manifest& m);   // instantiate Module blocks + on_start + on_attach
  void shutdown();                 // reap subprocess-owning modules
  std::vector<Module*> modules() const;
private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};
}  // namespace hades
