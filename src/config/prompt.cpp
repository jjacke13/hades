// src/config/prompt.cpp — implementation of assemble_system_prompt (layered SOUL/USER/MEMORY)
//
// Iterates the fixed file-key order, reads each configured file (MalConfig on an
// unreadable path — fail visibly, never silently run without a configured persona),
// and joins non-empty parts with a blank line. See hades/prompt.h.

#include "hades/prompt.h"
#include <array>
#include <fstream>
#include <sstream>
#include "hades/launcher.h"  // MalConfig
namespace hades {
namespace {
std::string read_or_throw(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw MalConfig("system prompt file not readable: " + path);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}
}  // namespace

std::string assemble_system_prompt(const Block& session) {
  static constexpr std::array<const char*, 3> kKeys = {
      "system_prompt_file", "user_file", "memory_file"};
  std::string out;
  for (const char* key : kKeys) {
    auto it = session.kv.find(key);
    if (it == session.kv.end() || it->second.empty()) continue;
    std::string content = read_or_throw(it->second);
    if (content.empty()) continue;
    if (!out.empty()) out += "\n\n";
    out += content;
  }
  return out;
}
}  // namespace hades
