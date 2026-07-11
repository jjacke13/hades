// include/hades/memory_limit.h — default char cap for the always-on core memory
//
// The core-memory file is folded into EVERY turn's system prompt, so every char is paid on
// every LLM call; the cap is the forcing function that makes the agent consolidate instead of
// hoarding (Hermes-agent precedent: 2200). Header-only so the standalone core_memory tool
// binary shares the exact default without linking hades_core (file_version.h precedent).
// Overridden per-manifest via Session { memory_char_limit }.
#pragma once
namespace hades {
inline constexpr long long kDefaultMemoryCharLimit = 2400;
}  // namespace hades
