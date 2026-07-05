// include/hades/bridge/registry.h — standardized bridge card schema + share-type vocabulary
//
// The "standardized blackboard variables" of the bridge protocol: the canonical A2A-shaped
// agent-card built by build_card (served at GET /card, pushed as a type=card share, and stored
// on a receiver as PEER.<peer>.card), plus the share `type` constants. Pure/tolerant helpers —
// build_skills_from_announce reverse-parses the SkillsModule's SKILLS_ANNOUNCE text; caps_summary
// renders a capability_policy block as CATEGORIES ONLY (never literal paths — a peer learns the
// shape of what an agent may do, not its allowlist contents).
#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include "hades/config.h"   // Block
namespace hades {

inline constexpr const char* kShareTypeCard = "card";  // value is an agent-card -> PEER.<from>.card
inline constexpr const char* kShareTypeFact = "fact";  // a content claim -> PEER.<from>.fact.<key>
inline constexpr const char* kShareTypeRaw  = "raw";   // legacy opaque -> PEER.<from>.<key>
inline constexpr const char* kCardKey       = "card";  // share key + PEER.<from>.card suffix

// Reverse-parse the SkillsModule announce ("- <id>: <desc>" lines) into [{"id","description"}].
// Tolerant: lines without the "- id: desc" shape (incl. the header) are skipped; "" -> [].
nlohmann::json build_skills_from_announce(const std::string& announce);

// Summarize a capability_policy block as categories: fs_read/fs_write -> "scoped" if an allow
// list is set else "none"; exec -> "scoped" if exec_allow set else "none"; net ->
// "private-blocked" if block_private_net is truthy else "public". NEVER emits literal paths.
nlohmann::json caps_summary(const Block& capability_policy_cfg);

// Assemble the A2A-shaped card. A2A field names (name/description/url/version/capabilities/skills)
// keep a future A2A client zero-rework; tools/caps are hades extension fields (A2A ignores unknown).
nlohmann::json build_card(const std::string& name, const std::string& url, int version,
                          const std::string& description, const std::string& skills_announce,
                          const nlohmann::json& tools, const nlohmann::json& caps);

}  // namespace hades
