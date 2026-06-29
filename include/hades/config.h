// include/hades/config.h — MOOS-style block manifest parser and types
//
// Defines Block (section/name/kv) and Manifest (ordered Block list + warnings).
// parse_manifest() is pure and never throws; unknown syntax emits warnings.
// The Launcher consumes a Manifest to instantiate Module and Objective blocks;
// Arbiter and LLMModule read their config from a Block's kv map.

#pragma once
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace hades {
struct Block {
  std::string section;                     // Session|Module|Tool|Arbiter|Objective
  std::string name;                        // text after "= "; "" if none
  std::map<std::string,std::string> kv;    // lower-cased keys
};
struct Manifest {
  std::vector<Block> blocks;
  std::vector<std::string> warnings;
  std::optional<Block> session() const;
  std::vector<Block> of(const std::string& section) const;   // case-insensitive section match
};
// Stable prefix for the "two key=value pairs packed on one physical line" warning.
// fatal_warnings() classifies by this prefix; the boundary (enforce_manifest) promotes
// matching warnings to a hard MalConfig so the binary refuses to start on a corrupt manifest.
inline constexpr std::string_view kMultiKvWarning = "multiple key=value pairs on one line";
Manifest parse_manifest(const std::string& text);   // pure; never throws; collects warnings
// Subset of m.warnings that are the fatal multi-kv class (start with kMultiKvWarning).
// Pure; no MalConfig dependency (keeps config.h decoupled from launcher.h).
std::vector<std::string> fatal_warnings(const Manifest& m);
bool set_double_on_string(const std::string& v, double& out);
bool set_bool_on_string(const std::string& v, bool& out);
bool set_pos_double_on_string(const std::string& v, double& out);
}  // namespace hades
