// src/module/skills_module.cpp — scan skills dir, post SKILLS_ANNOUNCE, rescan on save_skill
#include "hades/module/skills_module.h"
#include "hades/blackboard.h"
#include "hades/skills/scan.h"
namespace hades {

void SkillsModule::on_start(const Block& cfg, Blackboard&) { dir_ = resolve_skills_dir(cfg); }

void SkillsModule::post_announce_() {
  std::string ann;
  try {
    ann = format_skills_announce(scan_skills_dir(dir_));
  } catch (...) {
    ann.clear();   // a scan failure must never crash the pump thread; degrade to "no skills"
  }
  bb_->post("SKILLS_ANNOUNCE", ann, "skills");
}

void SkillsModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  // post() updates the latest-value map immediately (pump is only for handlers), so the very
  // first start_turn already sees this announce regardless of attach/pump ordering.
  post_announce_();
  bb.subscribe("TOOL_REQUEST", [this](const Entry& e) {
    if (!e.value.is_object()) return;
    auto t = e.value.find("tool");
    if (t == e.value.end() || !t->is_string() || t->get<std::string>() != "save_skill") return;
    auto id = e.value.find("id");
    if (id == e.value.end() || !id->is_string()) return;
    const std::string s = id->get<std::string>();
    if (!s.empty()) pending_saves_.insert(s);
  });
  bb.subscribe("TOOL_RESULT", [this](const Entry& e) {
    if (!e.value.is_object()) return;
    auto id = e.value.find("id");
    if (id == e.value.end() || !id->is_string()) return;
    if (pending_saves_.erase(id->get<std::string>()) == 0) return;   // not a save_skill result
    auto ok = e.value.find("ok");
    if (ok != e.value.end() && ok->is_boolean() && ok->get<bool>()) post_announce_();
  });
}
}  // namespace hades
