// src/skills/scan.cpp — skills dir scan + frontmatter parse (pure, tolerant, never throws)
#include "hades/skills/scan.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
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
