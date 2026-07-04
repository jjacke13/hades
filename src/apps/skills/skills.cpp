// src/apps/skills/skills.cpp — the skills-roster app: module + library scan
//
// Merged (2026-07-04 src reorg): module/skills_module (SKILLS_ANNOUNCE at attach +
// event-driven rescan on save_skill) + skills/scan (frontmatter parse, dir scan,
// announce format; valid_skill_name stays header-inline in include/hades/skills/scan.h).

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "hades/module/skills_module.h"
#include "hades/blackboard.h"
#include "hades/skills/scan.h"

// ── SkillsModule: SKILLS_ANNOUNCE at attach + rescan on save_skill (was src/module/skills_module.cpp) ──────────────
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

// ── skills dir scan + frontmatter parse (pure, tolerant, never throws) (was src/skills/scan.cpp) ──────────────
namespace hades {
namespace {
std::string trim(std::string s) {
  auto ns = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), ns));
  s.erase(std::find_if(s.rbegin(), s.rend(), ns).base(), s.end());
  return s;
}
}  // namespace

std::string parse_skill_description(const std::string& text) {
  if (text.rfind("---", 0) != 0) return "";                 // must open with a fence
  const std::size_t first_nl = text.find('\n');
  if (first_nl == std::string::npos) return "";
  const std::size_t close = text.find("\n---", first_nl);   // closing fence line
  if (close == std::string::npos) return "";
  std::istringstream fm(text.substr(first_nl + 1, close - first_nl - 1));
  std::string line;
  while (std::getline(fm, line))
    if (line.rfind("description:", 0) == 0) return trim(line.substr(12));
  return "";
}

std::vector<SkillInfo> scan_skills_dir(const std::string& dir) {
  std::vector<SkillInfo> out;
  std::error_code ec;
  std::filesystem::directory_iterator it(dir, ec), end;
  if (ec) return out;                                       // missing dir is not an error
  for (; it != end; it.increment(ec)) {
    if (ec) break;                                          // unreadable continuation: keep what we have
    std::error_code dec;
    if (!it->is_directory(dec) || dec) continue;
    const std::string name = it->path().filename().string();
    if (!valid_skill_name(name)) continue;   // announce only what use_skill can load
    std::ifstream f(it->path() / "SKILL.md");
    if (!f) continue;                                       // not a skill dir
    std::stringstream s;
    s << f.rdbuf();
    std::string desc = parse_skill_description(s.str());
    if (desc.empty()) continue;                             // unparseable skill: skip, never crash
    out.push_back({name, std::move(desc)});
  }
  std::sort(out.begin(), out.end(),
            [](const SkillInfo& a, const SkillInfo& b) { return a.name < b.name; });
  return out;
}

std::string format_skills_announce(const std::vector<SkillInfo>& skills) {
  if (skills.empty()) return "";
  std::string out = "Available skills (call use_skill with a name to load its full instructions):";
  for (const auto& s : skills) out += "\n- " + s.name + ": " + s.description;
  return out;
}

std::string resolve_skills_dir(const Block& skills_cfg) {
  auto it = skills_cfg.kv.find("dir");
  return (it != skills_cfg.kv.end() && !it->second.empty()) ? it->second : "skills";
}
}  // namespace hades
