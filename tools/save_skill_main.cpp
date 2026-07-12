// tools/save_skill_main.cpp — bundled save_skill native tool binary
//
// Reads one JSON line ({"call":"describe"|"save_skill","args":{name,description,body,
// old_string,new_string}}) and writes one JSON line. TWO modes on <skills_dir>/<name>/SKILL.md
// (skills dir = argv[1], fallback "skills"), selected by which optional arg is non-empty
// (empty string = absent — weak LLMs fill every schema field):
//   SAVE  (body non-empty):       canonical frontmatter + body; overwrite IS the update path.
//   PATCH (old_string non-empty): exact-substring replace, must match EXACTLY ONCE; the
//     patched file must still parse as a skill (parse_skill_description non-empty — the SAME
//     parse the scanner runs) or the patch is refused, so the agent cannot brick a skill out
//     of its own announce roster. No staleness expect_version (v1): a stale old_string fails
//     the match against LIVE disk content and the error says to re-read — self-healing.
// Both modes write ATOMICALLY (temp + rename) so a concurrent scan never sees a torn skill.
// The NAME is strictly validated (valid_skill_name, shared header) — a traversal name would be
// an arbitrary-file-WRITE escape; DESCRIPTION newlines are folded to spaces so a skill cannot
// inject extra lines into the one-line-per-skill announce list. Fail-closed: malformed input
// returns ok:false, never throws.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/skills/scan.h"   // valid_skill_name + parse_skill_description (both inline; no core link)

namespace {

// Atomic write shared by both modes. Returns "" on success, else the error message.
std::string write_atomic(const std::string& path, const std::string& content) {
  const std::string tmp = path + ".tmp";
  std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
  if (f) {
    f << content;
    f.close();
  }
  if (!f) {
    std::remove(tmp.c_str());
    return "cannot write: " + path;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);   // atomic on POSIX; replaces existing
  if (ec) {
    std::remove(tmp.c_str());
    return "cannot save: " + path;
  }
  return {};
}

}  // namespace

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
    out = {{"ok", true},
           {"result",
            {{"name", "save_skill"},
             {"description",
              "Save or patch a skill in your skills library — a reusable instruction pack "
              "your future self loads with use_skill. TWO modes: (1) SAVE — send name + "
              "description (ONE line shown in your skills list) + body (the full markdown "
              "instructions) to create or overwrite a skill; (2) PATCH — send name + "
              "old_string + new_string to edit part of an EXISTING skill without resending "
              "the whole body; old_string must match exactly once (give more surrounding "
              "context if ambiguous). Use one mode per call, never both."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"name", {{"type", "string"}}},
                 {"description", {{"type", "string"}}},
                 {"body", {{"type", "string"}}},
                 {"old_string", {{"type", "string"}}},
                 {"new_string", {{"type", "string"}}}}},
               {"required", nlohmann::json::array({"name"})}}}}}};
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
    const std::string olds = str("old_string");
    const std::string news = str("new_string");
    auto fail = [&](const std::string& msg) {
      out = {{"ok", false}, {"result", {{"error", msg}}}};
    };
    // Mode select: empty string = absent (weak LLMs fill every schema field).
    const bool save_mode = !body.empty();
    const bool patch_mode = !olds.empty();
    if (!hades::valid_skill_name(name)) {
      fail("invalid skill name");
    } else if (save_mode && patch_mode) {
      fail("provide body (save) OR old_string/new_string (patch), not both");
    } else if (!save_mode && !patch_mode) {
      fail("provide body (to save a full skill) or old_string/new_string (to patch an existing one)");
    } else if (save_mode) {
      if (desc.empty()) {
        fail("missing arg: description and body required");
      } else {
        for (char& c : desc)
          if (c == '\n' || c == '\r') c = ' ';   // one skill = one announce line
        std::error_code ec;
        const std::filesystem::path skill_dir = std::filesystem::path(dir) / name;
        std::filesystem::create_directories(skill_dir, ec);   // best-effort; write reports failure
        const std::string path = (skill_dir / "SKILL.md").string();
        std::string content = "---\nname: " + name + "\ndescription: " + desc + "\n---\n" + body;
        if (body.back() != '\n') content += "\n";
        const std::string err = write_atomic(path, content);
        if (!err.empty())
          fail(err);
        else
          out = {{"ok", true}, {"result", {{"saved", true}, {"name", name}, {"path", path}}}};
      }
    } else {   // patch mode
      if (!desc.empty()) {
        fail("patch edits the file directly — put the description change in old_string/new_string");
      } else if (olds == news) {
        fail("old_string and new_string are identical");
      } else {
        const std::string path = (std::filesystem::path(dir) / name / "SKILL.md").string();
        std::ifstream f(path, std::ios::binary);
        if (!f) {
          fail("no such skill: " + name + " — create it first with a full save (body)");
        } else {
          std::stringstream ss;
          ss << f.rdbuf();
          std::string content = ss.str();
          f.close();
          // Count non-overlapping occurrences (edit_file contract: exactly one, no replace_all
          // — skill files are small; the ambiguity fix is more surrounding context).
          int count = 0;
          for (std::size_t pos = content.find(olds); pos != std::string::npos;
               pos = content.find(olds, pos + olds.size()))
            ++count;
          if (count == 0) {
            fail("old_string not found in skill '" + name +
                 "' — use_skill it to see its current content and retry");
          } else if (count > 1) {
            fail("old_string matches " + std::to_string(count) +
                 " times — give more surrounding context");
          } else {
            content.replace(content.find(olds), olds.size(), news);
            // The scanner's own parse is the validity oracle: if it can no longer extract a
            // description, the skill would silently vanish from the announce roster.
            if (hades::parse_skill_description(content).empty()) {
              fail("patch would break the skill's frontmatter — fix old_string/new_string or "
                   "resend the full skill with body");
            } else {
              const std::string err = write_atomic(path, content);
              if (!err.empty())
                fail(err);
              else
                out = {{"ok", true},
                       {"result", {{"patched", true}, {"name", name}, {"path", path}}}};
            }
          }
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
