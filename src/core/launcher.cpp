#include "hades/launcher.h"
#include "hades/blackboard.h"
namespace hades {
struct Launcher::Impl {
  Blackboard& bb; std::map<std::string,Factory> factories;
  std::vector<std::unique_ptr<Module>> mods;
  explicit Impl(Blackboard& b):bb(b){}
};
Launcher::Launcher(Blackboard& bb): p_(std::make_unique<Impl>(bb)) {}
Launcher::~Launcher() = default;
void Launcher::register_factory(const std::string& t, Factory f){ p_->factories[t]=std::move(f); }
void Launcher::build(const Manifest& m){
  try {
    for(const auto& blk : m.of("Module")){
      auto it=p_->factories.find(blk.name);
      if(it==p_->factories.end()) throw MalConfig("unknown module type: "+blk.name);
      auto mod=it->second();
      mod->on_start(blk, p_->bb);
      mod->on_attach(p_->bb);
      p_->mods.push_back(std::move(mod));
    }
  } catch(...) { p_->mods.clear(); throw; }
}
void Launcher::shutdown(){ p_->mods.clear(); }   // module dtors reap their subprocesses
std::vector<Module*> Launcher::modules() const {
  std::vector<Module*> r; for(auto& u:p_->mods) r.push_back(u.get()); return r;
}
}  // namespace hades
