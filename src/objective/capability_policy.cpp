// src/objective/capability_policy.cpp — CapabilityPolicy: scoped allow/confirm/deny per tool
//
// Implements the capability taxonomy table + the veto mapping: fs_read/list_dir are path-scoped
// (deny-prefix -> hard veto; allow-prefix -> allow; else -> confirm); write_file/shell escalate
// (confirm); http_fetch hard-vetoes private/loopback hosts (SSRF guard) + configured deny hosts;
// memory_append tools pass; an unknown tool confirm-gates. Best-effort, allowlist-oriented; the
// path match is literal-prefix (no symlink/.. canonicalization in v1 — see the design doc).

#include "hades/objective/capability_policy.h"
#include <algorithm>
#include <array>
#include <cctype>
namespace hades {
namespace {

bool starts_with(const std::string& s, const std::string& p) {
  return s.size() >= p.size() && std::equal(p.begin(), p.end(), s.begin());
}
bool any_prefix(const std::string& s, const std::vector<std::string>& ps) {
  for (const auto& p : ps) if (!p.empty() && starts_with(s, p)) return true;
  return false;
}
std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}
// Parse "172.16.0.0/12" membership without inet_pton: octet1==172 && 16<=octet2<=31.
bool in_172_16_12(const std::string& h) {
  if (!starts_with(h, "172.")) return false;
  std::size_t dot = h.find('.', 4);
  if (dot == std::string::npos) return false;
  int o2 = -1;
  try { o2 = std::stoi(h.substr(4, dot - 4)); } catch (...) { return false; }
  return o2 >= 16 && o2 <= 31;
}

VetoResult allow()                          { return {}; }
VetoResult deny(const std::string& why)     { return {true, why, false}; }
VetoResult confirm(const std::string& why)  { return {true, why, true}; }

}  // namespace

CapabilityPolicy::CapabilityPolicy(CapabilityScope scope) : scope_(std::move(scope)) {}

Capability CapabilityPolicy::capability_of(const std::string& tool) {
  if (tool == "fs_read" || tool == "list_dir")          return Capability::FsRead;
  if (tool == "write_file")                              return Capability::FsWrite;
  if (tool == "http_fetch")                              return Capability::Net;
  if (tool == "shell")                                   return Capability::Exec;
  if (tool == "save_memory" || tool == "pin_fact")       return Capability::MemoryAppend;
  return Capability::Unknown;
}

std::string CapabilityPolicy::parse_host(const std::string& url) {
  auto p = url.find("://");
  std::string rest = (p == std::string::npos) ? url : url.substr(p + 3);
  // The authority component ends at the first '/', '?' or '#' (RFC 3986). Bound userinfo
  // stripping to within the authority so a path like "/@host" cannot masquerade as userinfo
  // and smuggle a different host past the loopback/private classifier (SSRF guard).
  std::size_t auth_end = rest.find_first_of("/?#");
  std::string authority = (auth_end == std::string::npos) ? rest : rest.substr(0, auth_end);
  if (auto at = authority.rfind('@'); at != std::string::npos)
    authority = authority.substr(at + 1);                  // strip userinfo (last '@' wins)
  // bracketed IPv6 literal: keep inner address (so "[::1]:8080" -> "::1")
  if (!authority.empty() && authority.front() == '[') {
    auto rb = authority.find(']');
    return lower(authority.substr(1, rb == std::string::npos ? std::string::npos : rb - 1));
  }
  std::size_t colon = authority.find(':');                 // strip :port (IPv4/hostname)
  std::string host = (colon == std::string::npos) ? authority : authority.substr(0, colon);
  return lower(host);
}

bool CapabilityPolicy::is_private_host(const std::string& raw) {
  std::string h = lower(raw);
  if (h == "localhost" || h == "::1" || h == "0.0.0.0") return true;
  static const std::array<const char*, 4> pre = {"127.", "10.", "192.168.", "169.254."};
  for (const char* p : pre) if (starts_with(h, p)) return true;
  return in_172_16_12(h);
}

VetoResult CapabilityPolicy::veto(const Blackboard&, const Action& a) const {
  if (a.kind != Action::Kind::ToolCall) return allow();
  switch (capability_of(a.tool)) {
    case Capability::MemoryAppend:
      return allow();                                   // append-only to the agent's own files
    case Capability::Exec:
      return confirm("exec capability (" + a.tool + "): runs an arbitrary command");
    case Capability::FsWrite: {
      std::string path = a.args.value("path", "");
      if (any_prefix(path, scope_.fs_read_deny))
        return deny("write to a denied path: " + path);
      return confirm("fs_write (" + a.tool + ") overwrites a file: " + path);
    }
    case Capability::FsRead: {
      std::string path = a.args.value("path", "");
      if (any_prefix(path, scope_.fs_read_deny))
        return deny("read of a denied path: " + path);
      if (any_prefix(path, scope_.fs_read_allow))
        return allow();
      return scope_.confirm_unscoped
                 ? confirm("fs_read outside allowed scope: " + path)
                 : deny("fs_read outside allowed scope: " + path);
    }
    case Capability::Net: {
      std::string host = parse_host(a.args.value("url", ""));
      if (scope_.block_private_net && is_private_host(host))
        return deny("net fetch to a private/loopback host: " + host);
      for (const auto& d : scope_.net_deny_hosts)
        if (!d.empty() && host.find(lower(d)) != std::string::npos)
          return deny("net fetch to a denied host: " + host);
      return allow();
    }
    case Capability::Unknown:
    default:
      return confirm("unknown tool '" + a.tool + "': capability undeclared");
  }
}

}  // namespace hades
