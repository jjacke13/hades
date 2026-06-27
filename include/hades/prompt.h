// include/hades/prompt.h — assemble the layered system prompt from Session *_file keys
//
// Reads system_prompt_file (SOUL) -> user_file (USER) -> memory_file (MEMORY) in order
// from a Session Block, joining non-empty contents with a blank line into one system
// message. Throws MalConfig if a configured file cannot be read; returns "" if no keys
// are set. Loaded once at startup by agent_wiring and handed to Arbiter::set_system_prompt.

#pragma once
#include <string>
#include "hades/config.h"
namespace hades {
std::string assemble_system_prompt(const Block& session);
}  // namespace hades
