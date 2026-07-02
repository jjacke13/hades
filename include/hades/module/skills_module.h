// include/hades/module/skills_module.h — skills roster app (MOOS behavior-library analogue)
//
// Announces the skills library on the bus: scans the skills dir at attach and posts
// SKILLS_ANNOUNCE (a preformatted block the Arbiter folds into the leading system message via
// get() — latest-value, no subscription). Event-driven refresh: tracks in-flight save_skill
// TOOL_REQUEST ids and rescans when the matching TOOL_RESULT lands ok — NO per-turn scanning.
// Empty/missing dir -> posts "" (the Arbiter injects nothing; feature costs zero when unused).
#pragma once
#include <set>
#include <string>
#include "hades/module.h"
namespace hades {
class Blackboard;
class SkillsModule : public Module {
public:
  std::string type() const override { return "skills"; }
  void on_start(const Block& cfg, Blackboard& bb) override;
  void on_attach(Blackboard& bb) override;

private:
  void post_announce_();
  std::string dir_ = "skills";
  Blackboard* bb_ = nullptr;
  std::set<std::string> pending_saves_;   // in-flight save_skill TOOL_REQUEST ids
};
}  // namespace hades
