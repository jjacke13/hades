// src/core/launcher.cpp — Launcher module-graph construction implementation
//
// Implements Launcher::build(): iterates Manifest Module blocks, resolves each by name
// in the registered factory map, instantiates the Module, and calls on_start(blk, bb)
// then on_attach(bb) to wire it onto the Blackboard. Throws MalConfig for unknown
// module types and rolls back (clears mods) on any error, matching pAntler semantics.

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
void Launcher::instantiate(const Manifest& m){
  try {
    for(const auto& blk : m.of("Module")){
      auto it=p_->factories.find(blk.name);
      if(it==p_->factories.end()) throw MalConfig("unknown module type: "+blk.name);
      p_->mods.push_back(it->second());
    }
  } catch(...) { p_->mods.clear(); throw; }
}
bool Launcher::has(const std::string& type) const {
  for(const auto& u : p_->mods) if(u && u->type()==type) return true;
  return false;
}
std::unique_ptr<Module> Launcher::take(const std::string& type){
  for(auto& u : p_->mods)
    if(u && u->type()==type) return std::move(u);   // leaves a null hole; has()/take() skip it
  return nullptr;
}
void Launcher::shutdown(){ p_->mods.clear(); }   // module dtors reap their subprocesses
std::vector<Module*> Launcher::modules() const {
  std::vector<Module*> r; for(auto& u:p_->mods) r.push_back(u.get()); return r;
}
}  // namespace hades
