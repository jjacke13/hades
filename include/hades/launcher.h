// include/hades/launcher.h — pAntler analog; builds modules from the Manifest
//
// Launcher holds a type->Factory registry; build() walks the Manifest's Module
// blocks, calls the matching Factory, then drives on_start() and on_attach()
// for each Module on the shared Blackboard. Throws MalConfig on an unknown
// block type or missing required key. agent_wiring registers all concrete
// factories (LLMModule, ToolRunner, ChatModule, Arbiter) before calling build().

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
  // pAntler roster: instantiate the manifest's `Module =` blocks (in order) via the
  // registered factories; throws MalConfig on an unknown type. Does NOT call on_start/
  // on_attach — the caller drives lifecycle + cross-wiring. Roster validation only.
  void instantiate(const Manifest& m);
  bool has(const std::string& type) const;                  // a module of this type was instantiated (and not yet taken)
  std::unique_ptr<Module> take(const std::string& type);     // transfer the first module of `type` out; nullptr if absent
  void shutdown();                 // reap subprocess-owning modules
  std::vector<Module*> modules() const;
private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};
}  // namespace hades
