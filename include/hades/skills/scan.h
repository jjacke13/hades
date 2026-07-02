// include/hades/skills/scan.h — skills library scan: frontmatter parse + roster announce
//
// Pure helpers behind the SkillsModule and the skills wiring. A skill is skills/<name>/SKILL.md
// with a "---"-fenced frontmatter carrying a one-line `description:`; the DIRECTORY name is the
// canonical skill id (frontmatter `name:` is display-only). valid_skill_name is header-only so
// the standalone tool binaries (use_skill/save_skill) share the exact same security-critical
// validation without linking hades_core.
#pragma once
#include <string>
#include <vector>
#include "hades/config.h"
namespace hades {

struct SkillInfo {
  std::string name;         // directory name — the canonical id use_skill resolves
  std::string description;  // frontmatter description — one announce line
};

// Strict skill-name gate: 1..64 chars of [A-Za-z0-9_-]. Anything else (path separators, dots,
// whitespace, empty) is rejected — a traversal name would be an arbitrary read/write escape.
// The range check is deliberately explicit ASCII (NOT std::isalnum): security-critical and shared
// by future tool binaries, it must be locale-INDEPENDENT.
inline bool valid_skill_name(const std::string& n) {
  if (n.empty() || n.size() > 64) return false;
  for (char c : n) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

// Extract the frontmatter `description:` from a SKILL.md. Returns "" when the text has no
// leading "---" fence, no closing fence, or no description line (tolerant, never throws).
std::string parse_skill_description(const std::string& text);

// Scan a skills dir: one SkillInfo per subdirectory whose SKILL.md yields a non-empty
// description, sorted by name (deterministic announce). Missing/unreadable dir -> {}.
std::vector<SkillInfo> scan_skills_dir(const std::string& dir);

// Render the announce block posted as SKILLS_ANNOUNCE. Empty list -> "" (no block injected).
std::string format_skills_announce(const std::vector<SkillInfo>& skills);

// Resolve the skills dir from a `Skills { dir = ... }` block (empty/absent key -> "skills").
// Single source of truth for the module AND the tool argv wiring.
std::string resolve_skills_dir(const Block& skills_cfg);

}  // namespace hades
