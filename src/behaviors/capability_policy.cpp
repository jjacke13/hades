// src/behaviors/capability_policy.cpp — CapabilityPolicy: scoped allow/confirm/deny per tool
//
// Implements the capability taxonomy table + the veto mapping: fs_read/list_dir are path-scoped
// (deny-prefix -> hard veto; allow-prefix -> allow; else -> confirm); write_file/shell escalate
// (confirm); http_fetch hard-vetoes private/loopback hosts (SSRF guard) + configured deny hosts;
// memory_append tools pass; an unknown tool confirm-gates. Allowlist-oriented and fail-closed:
// the path match is LEXICALLY normalized (canon_path collapses "./" and "." — NOT realpath, so
// symlinks are a documented v2 gap), an empty/unparseable fetch host is hard-vetoed, and a packed
// numeric host (decimal/hex/octal) is treated as private. See the header for the remaining v2 gaps.

#include "hades/objective/capability_policy.h"
#include <array>
#include <cctype>
#include <vector>
namespace hades {
namespace {

// DENY match: plain prefix. Over-matching on deny is the SAFE direction (denying slightly more
// is fine), so a bare starts_with is intentional here.
bool any_prefix(const std::string& s, const std::vector<std::string>& ps) {
  for (const auto& p : ps)
    if (!p.empty() && s.starts_with(p)) return true;
  return false;
}
// ALLOW match: path-component-boundary aware. An allow entry matches only if the (canon'd)
// request path EQUALS the entry or sits UNDER it (entry + "/"), so allow "./workspace" does NOT
// also allow the siblings "./workspace-backup" / "./workspaceX" (a bare starts_with would). An
// entry that already ends in '/' (e.g. the "./" root) is used as the prefix verbatim.
bool allow_match(const std::string& s, const std::vector<std::string>& ps) {
  for (const auto& p : ps) {
    if (p.empty()) continue;
    if (s == p) return true;
    const std::string pref = (p.back() == '/') ? p : p + "/";
    if (s.starts_with(pref)) return true;
  }
  return false;
}
// Type-safe string-arg extraction. nlohmann::json::value(key, default) THROWS type_error.302 when
// the key exists but is NOT a string (the LLM can emit fs_read {"path":123} / {"url":null}); this
// returns "" in that case so the caller's fail-closed handling (empty path -> confirm; empty url
// -> deny) applies. NEVER throws — a non-string arg must not crash the bus.
std::string str_arg(const nlohmann::json& args, const char* key) {
  auto it = args.find(key);
  return (it != args.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}
std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Lexical path canonicalization for scope matching. Collapses interior "." and "/./" components
// and unifies the relative-path prefix so "./.env", ".env" and "././.env" all map to one key
// ("./.env") — that closes the "./"-prefix deny bypass (fs_read ./.env must hit a ".env" deny).
// Absolute paths keep their leading '/'. ".." components are KEPT (the caller treats a surviving
// ".." as a base escape -> confirm). This is LEXICAL only: it does NOT resolve ".." against the
// real filesystem and does NOT follow symlinks (documented v2 gap in the header).
std::string canon_path(const std::string& in) {
  if (in.empty()) return in;
  const bool absolute = in.front() == '/';
  const bool trailing_slash = in.back() == '/';
  std::vector<std::string> parts;
  std::size_t i = 0;
  while (i < in.size()) {
    std::size_t s = in.find('/', i);
    std::string seg = (s == std::string::npos) ? in.substr(i) : in.substr(i, s - i);
    if (!seg.empty() && seg != ".") parts.push_back(seg);  // drop "" and "." segments
    if (s == std::string::npos) break;
    i = s + 1;
  }
  std::string out = absolute ? "/" : "./";
  for (std::size_t k = 0; k < parts.size(); ++k) {
    out += parts[k];
    if (k + 1 < parts.size()) out += '/';
  }
  if (trailing_slash && !parts.empty()) out += '/';
  return out;
}

// A surviving ".." component means the lexical path may escape its base (cwd / an allow root).
bool escapes_base(const std::string& canon) {
  std::size_t i = 0;
  while (i < canon.size()) {
    std::size_t s = canon.find('/', i);
    std::string seg = (s == std::string::npos) ? canon.substr(i) : canon.substr(i, s - i);
    if (seg == "..") return true;
    if (s == std::string::npos) break;
    i = s + 1;
  }
  return false;
}

// Parse "172.16.0.0/12" membership without inet_pton: octet1==172 && 16<=octet2<=31.
bool in_172_16_12(const std::string& h) {
  if (!h.starts_with("172.")) return false;
  std::size_t dot = h.find('.', 4);
  if (dot == std::string::npos) return false;
  int o2 = -1;
  try { o2 = std::stoi(h.substr(4, dot - 4)); } catch (const std::exception&) { return false; }
  return o2 >= 16 && o2 <= 31;
}

// Private IPv4 (dotted-quad): loopback, RFC1918, link-local and the unspecified address.
bool is_private_ipv4(const std::string& h) {
  if (h == "0.0.0.0") return true;
  static const std::array<const char*, 4> pre = {"127.", "10.", "192.168.", "169.254."};
  for (const char* p : pre)
    if (h.starts_with(p)) return true;
  return in_172_16_12(h);
}

// Fail-closed heuristic for a packed/obfuscated numeric host that is NOT a clean dotted-decimal
// IPv4: all-digits (2130706433), 0x/0X hex (0x7f000001), or any octet with a leading zero (octal,
// 0177.0.0.1). Such forms are near-always SSRF obfuscation, so classify them as private. A clean
// dotted-quad (93.184.216.34) and ordinary hostnames are deliberately left untouched (-> public).
bool looks_numeric_obfuscated(const std::string& h) {
  if (h.size() > 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) return true;  // hex packed
  bool has_dot = false, all_digit_or_dot = true;
  for (char c : h) {
    if (c == '.') has_dot = true;
    else if (!std::isdigit(static_cast<unsigned char>(c))) all_digit_or_dot = false;
  }
  if (!all_digit_or_dot) return false;        // contains a non-digit, non-dot -> a real hostname
  if (!has_dot) return true;                  // all digits, no dots -> packed decimal IP
  // dotted + all-numeric octets: flag a leading-zero octet (octal obfuscation)
  std::size_t start = 0;
  while (true) {
    std::size_t dot = h.find('.', start);
    std::string oct = (dot == std::string::npos) ? h.substr(start) : h.substr(start, dot - start);
    if (oct.size() > 1 && oct[0] == '0') return true;
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  return false;
}

// run_command never invokes a shell (whitespace-split argv + execvp), so these characters have
// no meaning to IT — but their presence signals shell-idiom intent the operator's prefix list
// cannot have vetted; fail closed to confirm.
bool has_shell_metachar(const std::string& c) {
  return c.find_first_of(";|&$`()<>\n") != std::string::npos;
}
// Token-boundary prefix match: "ctest" matches "ctest" and "ctest --x", never "ctest-evil".
bool exec_allow_match(const std::string& cmd, const std::vector<std::string>& prefixes) {
  for (const auto& p : prefixes) {
    if (p.empty()) continue;
    if (cmd == p) return true;
    if (cmd.size() > p.size() && cmd.compare(0, p.size(), p) == 0 && cmd[p.size()] == ' ')
      return true;
  }
  return false;
}

VetoResult allow()                          { return {}; }
VetoResult deny(const std::string& why)     { return {true, why, false}; }
VetoResult confirm(const std::string& why)  { return {true, why, true}; }

}  // namespace

CapabilityPolicy::CapabilityPolicy(CapabilityScope scope) : scope_(std::move(scope)) {
  // Canonicalize the path scopes once so prefix matching is consistent with the canon'd
  // request path (e.g. allow "./" and deny ".env" -> "./" / "./.env"). Hosts are untouched.
  for (auto& p : scope_.fs_read_allow)  p = canon_path(p);
  for (auto& p : scope_.fs_write_allow) p = canon_path(p);  // exec_allow are commands, NOT paths
  for (auto& p : scope_.fs_deny)        p = canon_path(p);
}

Capability CapabilityPolicy::capability_of(const std::string& tool) {
  if (tool == "fs_read" || tool == "list_dir")          return Capability::FsRead;
  if (tool == "write_file")                              return Capability::FsWrite;
  if (tool == "http_fetch")                              return Capability::Net;
  if (tool == "shell")                                   return Capability::Exec;
  if (tool == "save_memory" || tool == "pin_fact")       return Capability::MemoryAppend;
  if (tool == "use_skill")                               return Capability::SkillRead;
  if (tool == "save_skill")                              return Capability::SkillWrite;
  if (tool == "ask_agent")                               return Capability::PeerAsk;
  if (tool == "grep" || tool == "glob")                  return Capability::FsRead;
  if (tool == "edit_file")                               return Capability::FsWrite;
  if (tool == "git_read")                                return Capability::GitRead;
  if (tool == "run_command")                             return Capability::ExecScoped;
  if (tool == "schedule_task" || tool == "list_tasks" || tool == "cancel_task")
                                                         return Capability::SelfSchedule;
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
  if (!h.empty() && h.back() == '.') h.pop_back();         // FQDN-absolute trailing dot: localhost.
  if (h.empty()) return false;                             // emptiness is fail-closed at veto()
  if (h == "localhost" || h == "::1" || h == "::") return true;  // loopback + unspecified IPv6
  if (h.find(':') != std::string::npos) {                  // IPv6 literal (hostnames have no colon)
    if (h.starts_with("fe8") || h.starts_with("fe9") ||
        h.starts_with("fea") || h.starts_with("feb")) return true;   // fe80::/10 link-local
    if (h.starts_with("fc") || h.starts_with("fd")) return true;     // fc00::/7 unique-local
    // IPv4-mapped IPv6 (::ffff:0:0/96): ANY ::ffff:<addr> is a mapped IPv4 address, so fail closed
    // on the WHOLE range. This denies mapped-private (the SSRF bypass) AND the rare mapped-public
    // form — and crucially catches the HEX-COMPRESSED variants (::ffff:7f00:1 == 127.0.0.1,
    // ::ffff:a00:1 == 10.0.0.1) that the dotted-tail extraction below misses. h is already
    // lower-cased here, so ::FFFF: matches too. (A real hostname can't contain "::".)
    if (h.starts_with("::ffff:") || h.starts_with("0:0:0:0:0:ffff:")) return true;
    if (auto pos = h.rfind("::ffff:"); pos != std::string::npos) {   // IPv4-mapped ::ffff:a.b.c.d
      std::string tail = h.substr(pos + 7);
      if (tail.find('.') != std::string::npos && is_private_ipv4(tail)) return true;
    }
  }
  if (is_private_ipv4(h)) return true;
  return looks_numeric_obfuscated(h);                      // packed numeric host -> fail closed
}

VetoResult CapabilityPolicy::veto(const Blackboard&, const Action& a) const {
  if (a.kind != Action::Kind::ToolCall) return allow();
  switch (capability_of(a.tool)) {
    case Capability::MemoryAppend:
      return allow();                                   // append-only to the agent's own files
    case Capability::Exec:
      return confirm("exec capability (" + a.tool + "): runs an arbitrary command");
    case Capability::FsWrite: {
      std::string path = canon_path(str_arg(a.args, "path"));
      if (any_prefix(path, scope_.fs_deny))
        return deny("write to a denied path: " + path);
      if (escapes_base(path))                           // '..' may escape the allow root
        return confirm("fs_write escapes its base via '..': " + path);
      if (allow_match(path, scope_.fs_write_allow))     // boundary-aware (no sibling over-match)
        return allow();
      return confirm("fs_write (" + a.tool + ") modifies a file outside fs_write_allow: " + path);
    }
    case Capability::FsRead: {
      std::string path = canon_path(str_arg(a.args, "path"));
      if (any_prefix(path, scope_.fs_deny))
        return deny("read of a denied path: " + path);
      if (escapes_base(path))                           // surviving ".." -> never silently allow
        return confirm("fs_read escapes its base via '..': " + path);
      if (allow_match(path, scope_.fs_read_allow))      // boundary-aware (no sibling over-match)
        return allow();
      return scope_.confirm_unscoped
                 ? confirm("fs_read outside allowed scope: " + path)
                 : deny("fs_read outside allowed scope: " + path);
    }
    case Capability::Net: {
      std::string host = parse_host(str_arg(a.args, "url"));
      if (host.empty())                                 // a gate must not allow what it can't classify
        return deny("net fetch with an unparseable/empty host");
      if (scope_.block_private_net && is_private_host(host))
        return deny("net fetch to a private/loopback host: " + host);
      for (const auto& d : scope_.net_deny_hosts)
        if (!d.empty() && host.find(lower(d)) != std::string::npos)
          return deny("net fetch to a denied host: " + host);
      return allow();
    }
    case Capability::SkillRead:
    case Capability::SkillWrite:
      // The agent's own skills library: the directory is fixed by wiring argv (never chosen by
      // the LLM) and the skill name is strictly gated in the tools. pin_fact precedent —
      // unconfirmed writes to the agent's own files; a saved skill is WEAKER than a pin (its
      // body only enters context on an explicit use_skill). Distinct capabilities (not
      // MemoryAppend) keep the table honest so a future policy can confirm-gate SkillWrite
      // without code changes.
      return allow();
    case Capability::PeerAsk:
      // Delegation to a rostered peer agent: the roster/urls are fixed by wiring argv (never
      // chosen by the LLM) and the RECEIVING agent's own objectives/confirm gates evaluate
      // whatever the request causes — those are the real protection. Distinct capability so a
      // future policy can confirm-gate outbound asks with zero code (SkillWrite precedent).
      return allow();
    case Capability::GitRead:
      // Read-only by construction: the tool runs `git` with a FIXED argv per op (status/diff/
      // log), never a shell, rejects leading-dash paths and passes `--` before pathspecs.
      return allow();
    case Capability::ExecScoped: {
      const std::string cmd = str_arg(a.args, "command");
      if (cmd.empty())
        return confirm("run_command with a missing/non-string command");
      if (has_shell_metachar(cmd))
        return confirm("run_command with shell metacharacters: " + cmd);
      if (exec_allow_match(cmd, scope_.exec_allow))
        return allow();
      return confirm("run_command outside exec_allow: " + cmd);
    }
    case Capability::SelfSchedule:
      // The agent's own task store: the store path + caps are fixed by wiring argv (never
      // chosen by the LLM), and SelfScheduleGuard already contains the heartbeat-origin
      // recursion risk. Distinct capability (SkillWrite precedent) so a future policy can
      // confirm-gate scheduling with zero code.
      return allow();
    case Capability::Unknown:
    default:
      return confirm("unknown tool '" + a.tool + "': capability undeclared");
  }
}

}  // namespace hades
