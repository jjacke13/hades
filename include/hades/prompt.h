// include/hades/prompt.h — assemble the layered system prompt + read the live core-memory layer
//
// assemble_system_prompt reads system_prompt_file (SOUL) -> user_file (USER) in order from a
// Session Block, joining non-empty contents with a blank line. Throws MalConfig if a configured
// file cannot be read; "" if neither key is set. Loaded once at startup (Arbiter::set_system_prompt).
//
// read_memory_layer reads the always-on MEMORY file (memory_file) fresh — the Arbiter calls it
// every turn so the agent's core_memory edits are live. Tolerant: ""/missing path -> "", never throws.
#pragma once
#include <string>
#include "hades/config.h"
namespace hades {
std::string assemble_system_prompt(const Block& session);
std::string read_memory_layer(const std::string& path);
}  // namespace hades
