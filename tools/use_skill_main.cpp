// tools/use_skill_main.cpp — bundled use_skill native tool binary
//
// Reads one JSON line ({"call":"describe"|"use_skill","args":{name}}) and writes one JSON
// line. Loads <skills_dir>/<name>/SKILL.md (skills dir = argv[1], fallback "skills") and
// returns its full content; the Arbiter loops it back to the LLM as a tool result, so the
// skill's instructions persist in the conversation. The skill NAME is strictly validated
// (valid_skill_name, shared header) — a traversal name would otherwise be an arbitrary-file-
// read escape. Fail-closed: malformed/adversarial input returns ok:false, never throws.
#include <fstream>
#include <iostream>
#include <sstream>
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
    nlohmann::json required = nlohmann::json::array();
    required.push_back("name");
    out = {{"ok", true},
           {"result",
            {{"name", "use_skill"},
             {"description",
              "Load one of your skills: returns the named skill's full SKILL.md instructions "
              "from your skills library. Call this before doing a task a skill covers."},
             {"schema",
              {{"type", "object"},
               {"properties", {{"name", {{"type", "string"}}}}},
               {"required", required}}}}}};
  } else if (call == "use_skill") {
    nlohmann::json args = (in.is_object() && in.contains("args") && in["args"].is_object())
                              ? in["args"]
                              : nlohmann::json::object();
    const bool has_name = args.contains("name") && args["name"].is_string();
    const std::string name = has_name ? args["name"].get<std::string>() : "";
    if (!hades::valid_skill_name(name)) {
      out = {{"ok", false}, {"result", {{"error", "invalid skill name"}}}};
    } else {
      std::ifstream f(dir + "/" + name + "/SKILL.md");
      if (!f) {
        out = {{"ok", false}, {"result", {{"error", "no such skill: " + name}}}};
      } else {
        std::stringstream s;
        s << f.rdbuf();
        out = {{"ok", true}, {"result", {{"name", name}, {"content", s.str()}}}};
      }
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }
  // replace-handler dump: raw SKILL.md bytes may be invalid UTF-8 — degrade to U+FFFD, never throw
  std::cout << out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << std::endl;
  return 0;
}
