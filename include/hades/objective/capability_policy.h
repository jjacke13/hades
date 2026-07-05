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
enum class Capability { FsRead, FsWrite, Net, Exec, MemoryAppend, SkillRead, SkillWrite, PeerAsk, GitRead, ExecScoped, Unknown };

// Operator-supplied bounds (from the `Objective = capability_policy { … }` manifest block).
// Path lists are lexically-normalized prefixes (leading "./" and "." components collapsed before
// matching — see canon_path); host checks are string/range based (no DNS resolution in v1).
struct CapabilityScope {
  std::vector<std::string> fs_read_allow;     // prefixes fs_read/list_dir may read silently
  std::vector<std::string> fs_deny;           // prefixes hard-vetoed for BOTH fs_read AND fs_write
                                              // (key file, /etc, secrets) — was fs_read_deny
  std::vector<std::string> fs_write_allow;    // prefixes write_file/edit_file may write WITHOUT
                                              // confirm (fs_deny still hard-vetoes; empty ->
                                              // every write confirms, the pre-scope behavior)
  std::vector<std::string> exec_allow;        // run_command COMMAND PREFIXES allowed without
                                              // confirm. COMMA-separated in the manifest
                                              // (prefixes contain spaces). Matched at a token
                                              // boundary; commands with shell metacharacters
                                              // always confirm (run_command never uses a shell,
                                              // and prefix matching is only sound that way).
  std::vector<std::string> net_deny_hosts;    // extra explicit host substrings hard-vetoed
  bool block_private_net = true;              // hard-veto loopback + RFC1918 + link-local
  bool confirm_unscoped  = true;              // out-of-allow-scope read -> confirm (else hard-veto)
};

// ── v1 security posture: what this gate does NOT close (deferred to v2, must stay visible) ──
//  • DNS rebinding / TOCTOU: is_private_host() classifies the URL's host string, but cpr resolves
//    and connects LATER — a name that resolves public at check-time and private at connect-time
//    (or round-robins) bypasses the gate. Real fix needs connect-time enforcement (resolve+pin, or
//    a SOCKS/HTTP egress proxy that re-checks the connected IP).
//  • Symlink path-deny bypass: path matching is LEXICAL (canon_path), not realpath() — a symlink
//    under an allowed root pointing at /etc/shadow still reads the secret. Needs realpath + an
//    O_NOFOLLOW-style resolution in the fs tool.
//  • No positive net egress allowlist: default-allow-public still permits exfiltration to ANY
//    public host (only private + explicit deny-substrings are blocked). Needs an opt-in net_allow.

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
