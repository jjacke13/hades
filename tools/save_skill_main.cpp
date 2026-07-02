// tools/save_skill_main.cpp — bundled save_skill native tool binary
//
// Reads one JSON line ({"call":"describe"|"save_skill","args":{name,description,body}}),
// writes <skills_dir>/<name>/SKILL.md (skills dir = argv[1], fallback "skills") with the
// canonical frontmatter, and writes one JSON line. Overwrite IS the update path. The write
// is atomic (temp file + rename) so a concurrent scan never sees a torn skill. The NAME is
// strictly validated (valid_skill_name, shared header) — a traversal name would be an
// arbitrary-file-WRITE escape; DESCRIPTION newlines are folded to spaces so a skill cannot
// inject extra lines into the one-line-per-skill announce list. Fail-closed: malformed input
// returns ok:false, never throws.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/skills/scan.h"   // valid_skill_name (header-only; no core link)

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "skills";
  std::string line;
  std::getline(std::cin, line);
  auto in = nlohmann::json::parse(line, nullptr, false);

  nlohmann::json out;
  std::string call;
  if (in.is_object() && in.contains("call") && in["call"].is_string())
    call = in["call"].get<std::string>();

  if (call == "describe") {
    nlohmann::json required = nlohmann::json::array({"name", "description", "body"});
    out = {{"ok", true},
           {"result",
            {{"name", "save_skill"},
             {"description",
              "Save (or overwrite) a skill in your skills library — a reusable instruction "
              "pack your future self loads with use_skill. name: short id (letters/digits/-/_)"
              "; description: ONE line shown in your skills list; body: the full markdown "
              "instructions."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"name", {{"type", "string"}}},
                 {"description", {{"type", "string"}}},
                 {"body", {{"type", "string"}}}}},
               {"required", required}}}}}};
  } else if (call == "save_skill") {
    nlohmann::json args = (in.is_object() && in.contains("args") && in["args"].is_object())
                              ? in["args"]
                              : nlohmann::json::object();
    auto str = [&](const char* k) {
      return args.contains(k) && args[k].is_string() ? args[k].get<std::string>()
                                                     : std::string{};
    };
    const std::string name = str("name");
    std::string desc = str("description");
    const std::string body = str("body");
    if (!hades::valid_skill_name(name)) {
      out = {{"ok", false}, {"result", {{"error", "invalid skill name"}}}};
    } else if (desc.empty() || body.empty()) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: description and body required"}}}};
    } else {
      for (char& c : desc)
        if (c == '\n' || c == '\r') c = ' ';   // one skill = one announce line
      std::error_code ec;
      const std::filesystem::path skill_dir = std::filesystem::path(dir) / name;
      std::filesystem::create_directories(skill_dir, ec);   // best-effort; ofstream reports failure
      const std::string path = (skill_dir / "SKILL.md").string();
      const std::string tmp = path + ".tmp";
      std::ofstream f(tmp, std::ios::trunc);
      if (f) {
        f << "---\nname: " << name << "\ndescription: " << desc << "\n---\n" << body;
        if (body.back() != '\n') f << "\n";
        f.close();
      }
      if (!f) {
        std::remove(tmp.c_str());
        out = {{"ok", false}, {"result", {{"error", "cannot write: " + path}}}};
      } else {
        std::filesystem::rename(tmp, path, ec);   // atomic on POSIX; replaces existing
        if (ec) {
          std::remove(tmp.c_str());
          out = {{"ok", false}, {"result", {{"error", "cannot save: " + path}}}};
        } else {
          out = {{"ok", true}, {"result", {{"saved", true}, {"name", name}, {"path", path}}}};
        }
      }
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }
  // replace-handler dump: never let an invalid-UTF-8 byte in a message throw and kill the tool
  std::cout << out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << std::endl;
  return 0;
}
