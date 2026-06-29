// src/config/prompt.cpp — assemble_system_prompt (SOUL+USER) + read_memory_layer (live core memory)
//
// assemble_system_prompt iterates the static persona keys (system_prompt_file, user_file), reads each
// configured file (MalConfig on an unreadable path — fail visibly), joins non-empty parts with a blank
// line. The MEMORY layer is NOT assembled here; it is read live per-turn via read_memory_layer.
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
  static constexpr std::array<const char*, 2> kKeys = {"system_prompt_file", "user_file"};
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

std::string read_memory_layer(const std::string& path) {
  if (path.empty()) return "";
  std::ifstream f(path);
  if (!f) return "";  // core file may not exist until the first pin_fact — not an error
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}
}  // namespace hades
