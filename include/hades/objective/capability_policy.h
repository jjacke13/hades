// include/hades/objective/capability_policy.h — capability/scope safety objective
//
// CapabilityPolicy is a standing safety behavior (MOOS-IvP BHV_OpRegion analogue): it bounds
// which files a tool may read/write and which hosts it may reach. It classifies a ToolCall by a
// built-in, code-reviewed tool->capability table (a tool is NOT trusted to declare its own
// permission) and maps it to allow / confirm / hard-veto using operator-supplied scopes. Reuses
// the Objective VetoResult{vetoed, reason, needs_confirm} consumed by Arbiter::dispatch_or_gate.

#pragma once
#include <string>
#include <vector>
#include "hades/objective.h"
namespace hades {

// The kinds of authority a tool action can exercise. Authoritative source is the built-in
// capability_of() table; an unknown tool maps to Unknown (-> confirm-gated).
enum class Capability { FsRead, FsWrite, Net, Exec, MemoryAppend, Unknown };

// Operator-supplied bounds (from the `Objective = capability_policy { … }` manifest block).
// Path lists are literal prefixes; host checks are string/range based (no DNS resolution in v1).
struct CapabilityScope {
  std::vector<std::string> fs_read_allow;     // prefixes fs_read/list_dir may read silently
  std::vector<std::string> fs_read_deny;      // prefixes hard-vetoed (key file, /etc, secrets)
  std::vector<std::string> net_deny_hosts;    // extra explicit host substrings hard-vetoed
  bool block_private_net = true;              // hard-veto loopback + RFC1918 + link-local
  bool confirm_unscoped  = true;              // out-of-allow-scope read -> confirm (else hard-veto)
};

class CapabilityPolicy : public Objective {
public:
  explicit CapabilityPolicy(CapabilityScope scope);
  std::string type() const override { return "capability_policy"; }
  VetoResult  veto(const Blackboard&, const Action&) const override;

  // Built-in trusted table + URL helpers (static so tests can exercise them directly).
  static Capability  capability_of(const std::string& tool);
  static std::string parse_host(const std::string& url);     // lower-cased host, userinfo/port stripped
  static bool        is_private_host(const std::string& host);

private:
  CapabilityScope scope_;
};

}  // namespace hades
