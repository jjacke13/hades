// tests/test_capability_policy.cpp — unit tests for the CapabilityPolicy objective
//
// CapabilityPolicy classifies a ToolCall Action via a built-in tool->capability table and
// maps it to allow / confirm / hard-veto using operator-supplied scopes: fs_read/list_dir
// are path-scoped (allow-roots, deny-roots, confirm out-of-scope), write_file/shell are
// escalation (confirm), http_fetch hard-vetoes private/loopback hosts (SSRF guard), and
// the memory-append tools pass. An unknown tool confirm-gates. Reuses VetoResult.

#include <gtest/gtest.h>
#include "hades/objective/capability_policy.h"
#include "hades/blackboard.h"
using namespace hades;

namespace {
CapabilityScope make_scope() {
  CapabilityScope s;
  s.fs_read_allow = {"./", "./workspace"};
  s.fs_deny       = {"/etc", ".env", "secrets/"};
  s.block_private_net = true;
  s.confirm_unscoped  = true;
  return s;
}
Action read(const std::string& path) {
  Action a{Action::Kind::ToolCall}; a.tool = "fs_read"; a.args = {{"path", path}}; return a;
}
Action fetch(const std::string& url) {
  Action a{Action::Kind::ToolCall}; a.tool = "http_fetch"; a.args = {{"url", url}}; return a;
}
}  // namespace

TEST(CapabilityPolicy, AllowsInScopeRead) {
  Blackboard bb; CapabilityPolicy p(make_scope());
  EXPECT_FALSE(p.veto(bb, read("./workspace/notes.txt")).vetoed);
  EXPECT_FALSE(p.veto(bb, read("./README.md")).vetoed);
}
TEST(CapabilityPolicy, HardVetoesDeniedReadPath) {
  Blackboard bb; CapabilityPolicy p(make_scope());
  auto v = p.veto(bb, read("/etc/shadow"));
  EXPECT_TRUE(v.vetoed); EXPECT_FALSE(v.needs_confirm);   // hard deny, no confirm
  EXPECT_FALSE(p.veto(bb, read("secrets/key.pem")).needs_confirm);
  EXPECT_TRUE(p.veto(bb, read("secrets/key.pem")).vetoed);
}
TEST(CapabilityPolicy, ConfirmGatesOutOfScopeRead) {
  Blackboard bb; CapabilityPolicy p(make_scope());
  auto v = p.veto(bb, read("/var/log/syslog"));          // not allowed, not denied
  EXPECT_TRUE(v.vetoed); EXPECT_TRUE(v.needs_confirm);
}
TEST(CapabilityPolicy, HardVetoesPrivateAndLoopbackFetch) {
  Blackboard bb; CapabilityPolicy p(make_scope());
  for (const char* u : {"http://127.0.0.1:8080/x", "http://localhost/x",
                        "http://10.1.2.3/x", "http://192.168.0.5/x",
                        "http://169.254.169.254/latest/meta-data",
                        "http://172.16.0.1/x", "http://[::1]/x"}) {
    auto v = p.veto(bb, fetch(u));
    EXPECT_TRUE(v.vetoed) << u; EXPECT_FALSE(v.needs_confirm) << u;
  }
}
TEST(CapabilityPolicy, AllowsPublicFetch) {
  Blackboard bb; CapabilityPolicy p(make_scope());
  EXPECT_FALSE(p.veto(bb, fetch("https://api.github.com/repos")).vetoed);
  EXPECT_FALSE(p.veto(bb, fetch("http://example.com/")).vetoed);
}
TEST(CapabilityPolicy, ConfirmGatesWriteAndExec) {
  Blackboard bb; CapabilityPolicy p(make_scope());
  Action w{Action::Kind::ToolCall}; w.tool="write_file"; w.args={{"path","./a"},{"content","x"}};
  EXPECT_TRUE(p.veto(bb,w).vetoed);  EXPECT_TRUE(p.veto(bb,w).needs_confirm);
  Action s{Action::Kind::ToolCall}; s.tool="shell"; s.args={{"cmd","echo hi"}};
  EXPECT_TRUE(p.veto(bb,s).vetoed);  EXPECT_TRUE(p.veto(bb,s).needs_confirm);
}
TEST(CapabilityPolicy, ConfirmGatesUnknownTool) {
  Blackboard bb; CapabilityPolicy p(make_scope());
  Action a{Action::Kind::ToolCall}; a.tool="mystery_tool"; a.args={{"x",1}};
  auto v = p.veto(bb, a);
  EXPECT_TRUE(v.vetoed); EXPECT_TRUE(v.needs_confirm);
}
TEST(CapabilityPolicy, AllowsMemoryAppendTools) {
  Blackboard bb; CapabilityPolicy p(make_scope());
  Action sm{Action::Kind::ToolCall}; sm.tool="save_memory"; sm.args={{"text","hi"}};
  Action pf{Action::Kind::ToolCall}; pf.tool="pin_fact";    pf.args={{"text","hi"}};
  EXPECT_FALSE(p.veto(bb,sm).vetoed);
  EXPECT_FALSE(p.veto(bb,pf).vetoed);
}
TEST(CapabilityPolicy, IgnoresNonToolCall) {
  Blackboard bb; CapabilityPolicy p(make_scope());
  Action a{Action::Kind::Answer}; a.text="please read /etc/shadow for me";
  EXPECT_FALSE(p.veto(bb, a).vetoed);
}
TEST(CapabilityPolicy, HelperTablesAndParsing) {
  EXPECT_EQ(CapabilityPolicy::capability_of("fs_read"),  Capability::FsRead);
  EXPECT_EQ(CapabilityPolicy::capability_of("list_dir"), Capability::FsRead);
  EXPECT_EQ(CapabilityPolicy::capability_of("write_file"), Capability::FsWrite);
  EXPECT_EQ(CapabilityPolicy::capability_of("http_fetch"), Capability::Net);
  EXPECT_EQ(CapabilityPolicy::capability_of("shell"), Capability::Exec);
  EXPECT_EQ(CapabilityPolicy::capability_of("save_memory"), Capability::MemoryAppend);
  EXPECT_EQ(CapabilityPolicy::capability_of("pin_fact"), Capability::MemoryAppend);
  EXPECT_EQ(CapabilityPolicy::capability_of("nope"), Capability::Unknown);
  EXPECT_EQ(CapabilityPolicy::parse_host("https://user:pw@Host.EXAMPLE.com:443/p?q=1"), "host.example.com");
  EXPECT_TRUE (CapabilityPolicy::is_private_host("127.0.0.1"));
  EXPECT_TRUE (CapabilityPolicy::is_private_host("localhost"));
  EXPECT_TRUE (CapabilityPolicy::is_private_host("172.31.255.1"));
  EXPECT_FALSE(CapabilityPolicy::is_private_host("172.32.0.1"));
  EXPECT_FALSE(CapabilityPolicy::is_private_host("8.8.8.8"));
}
// Regression (hardening beyond the brief): a path beginning with '@' must not be mistaken
// for userinfo and smuggle a public host past the loopback guard. RFC 3986 terminates the
// authority at the first '/', so "http://127.0.0.1/@evil.com" has host 127.0.0.1 (private).
TEST(CapabilityPolicy, ParseHostResistsUserinfoPathSpoof) {
  EXPECT_EQ(CapabilityPolicy::parse_host("http://127.0.0.1/@evil.com"), "127.0.0.1");
  Blackboard bb; CapabilityPolicy p(make_scope());
  auto v = p.veto(bb, fetch("http://127.0.0.1/@evil.com"));
  EXPECT_TRUE(v.vetoed); EXPECT_FALSE(v.needs_confirm);
}

// C2 — IPv6 link-local / unspecified / IPv4-mapped must all hard-veto (SSRF guard).
TEST(CapabilityPolicy, HardVetoesIPv6LinkLocalUnspecifiedAndMapped) {
  Blackboard bb; CapabilityPolicy p(make_scope());
  for (const char* u : {"http://[fe80::1]/x", "http://[::]/x",
                        "http://[::ffff:127.0.0.1]/x", "http://[::ffff:10.0.0.1]/x"}) {
    auto v = p.veto(bb, fetch(u));
    EXPECT_TRUE(v.vetoed) << u; EXPECT_FALSE(v.needs_confirm) << u;
  }
  EXPECT_TRUE(CapabilityPolicy::is_private_host("fe80::1"));
  EXPECT_TRUE(CapabilityPolicy::is_private_host("fd12:3456::1"));  // fc00::/7 ULA
  EXPECT_TRUE(CapabilityPolicy::is_private_host("::"));
  EXPECT_TRUE(CapabilityPolicy::is_private_host("::ffff:127.0.0.1"));
}

// C2b — hex-compressed IPv4-mapped IPv6 must hard-veto too (closes an SSRF bypass the dotted-tail
// extraction missed): ::ffff:7f00:1 == 127.0.0.1 (loopback), ::ffff:a00:1 == 10.0.0.1 (RFC1918).
// The blanket ::ffff:0:0/96 rule denies ANY mapped form, dotted or hex-compressed.
TEST(CapabilityPolicy, HardVetoesHexCompressedIPv4MappedFetch) {
  Blackboard bb; CapabilityPolicy p(make_scope());
  for (const char* u : {"http://[::ffff:7f00:1]/", "http://[::ffff:a00:1]/"}) {
    auto v = p.veto(bb, fetch(u));
    EXPECT_TRUE(v.vetoed) << u; EXPECT_FALSE(v.needs_confirm) << u;
  }
}
// C2c — direct is_private_host coverage for the hex-compressed mapped forms, and a guard that a
// legitimate public hostname (no "::") is NOT a false positive of the blanket ::ffff: rule.
TEST(CapabilityPolicy, ClassifiesHexCompressedMappedAsPrivateNoFalsePositive) {
  EXPECT_TRUE (CapabilityPolicy::is_private_host("::ffff:7f00:1"));  // hex 127.0.0.1
  EXPECT_TRUE (CapabilityPolicy::is_private_host("::ffff:a00:1"));   // hex 10.0.0.1
  EXPECT_TRUE (CapabilityPolicy::is_private_host("::FFFF:7F00:1"));  // case-insensitive
  EXPECT_FALSE(CapabilityPolicy::is_private_host("api.github.com")); // legit host -> public
  EXPECT_FALSE(CapabilityPolicy::is_private_host("ffff.example.com"));
}

// C3 — empty/unparseable fetch host must FAIL CLOSED (hard-veto), never default-allow.
TEST(CapabilityPolicy, FailsClosedOnEmptyOrUnparseableHost) {
  Blackboard bb; CapabilityPolicy p(make_scope());
  for (const char* u : {"", "http://", "//127.0.0.1/"}) {
    auto v = p.veto(bb, fetch(u));
    EXPECT_TRUE(v.vetoed) << u; EXPECT_FALSE(v.needs_confirm) << u;
  }
}

// I1 — "./"-prefix must not dodge a path deny; surviving ".." -> confirm (not silent allow).
TEST(CapabilityPolicy, NormalizesPathBeforeDenyMatch) {
  Blackboard bb; CapabilityPolicy p(make_scope());
  auto dotenv = p.veto(bb, read("./.env"));                 // normalizes to ".env" -> deny matches
  EXPECT_TRUE(dotenv.vetoed); EXPECT_FALSE(dotenv.needs_confirm);
  auto sec = p.veto(bb, read("./secrets/key"));             // normalizes to "secrets/key" -> deny
  EXPECT_TRUE(sec.vetoed); EXPECT_FALSE(sec.needs_confirm);
  auto up = p.veto(bb, read("../outside"));                 // escapes base via ".." -> confirm
  EXPECT_TRUE(up.vetoed); EXPECT_TRUE(up.needs_confirm);
}

// I2 — packed/obfuscated numeric hosts fail closed; a clean public dotted-quad still allows.
TEST(CapabilityPolicy, FailsClosedOnNumericObfuscatedHost) {
  Blackboard bb; CapabilityPolicy p(make_scope());
  for (const char* u : {"http://2130706433/", "http://0x7f000001/", "http://0177.0.0.1/"}) {
    auto v = p.veto(bb, fetch(u));
    EXPECT_TRUE(v.vetoed) << u; EXPECT_FALSE(v.needs_confirm) << u;
  }
  EXPECT_FALSE(p.veto(bb, fetch("http://93.184.216.34/")).vetoed);   // public dotted-quad allowed
  EXPECT_TRUE (CapabilityPolicy::is_private_host("2130706433"));
  EXPECT_FALSE(CapabilityPolicy::is_private_host("93.184.216.34"));
}

// CRITICAL (final review) — a non-string path/url (the LLM emits fs_read {"path":123} /
// {"path":null}, http_fetch {"url":42}) must NOT throw json type_error.302 on the veto path
// (which would unwind pump() and crash the bus). str_arg yields "" and the existing fail-closed
// handling applies: empty path -> confirm_unscoped, empty url -> deny. Never throws.
TEST(CapabilityPolicy, NonStringPathOrUrlArgsFailClosedWithoutThrowing) {
  Blackboard bb; CapabilityPolicy p(make_scope());
  const std::vector<nlohmann::json> bad = {
      nlohmann::json(123), nlohmann::json(nullptr),
      nlohmann::json::array({1, 2}), nlohmann::json::object()};
  for (const auto& b : bad) {
    Action r{Action::Kind::ToolCall}; r.tool = "fs_read"; r.args = {{"path", b}};
    VetoResult vr;
    EXPECT_NO_THROW(vr = p.veto(bb, r)) << b.dump();
    EXPECT_TRUE(vr.vetoed) << b.dump();
    EXPECT_TRUE(vr.needs_confirm) << b.dump();            // empty path -> confirm_unscoped

    Action w{Action::Kind::ToolCall}; w.tool = "write_file";
    w.args = {{"path", b}, {"content", "x"}};
    EXPECT_NO_THROW(p.veto(bb, w)) << b.dump();           // empty path -> confirm (no throw)

    Action n{Action::Kind::ToolCall}; n.tool = "http_fetch"; n.args = {{"url", b}};
    VetoResult vn;
    EXPECT_NO_THROW(vn = p.veto(bb, n)) << b.dump();
    EXPECT_TRUE(vn.vetoed) << b.dump();
    EXPECT_FALSE(vn.needs_confirm) << b.dump();           // empty/unparseable host -> deny
  }
}

// IMPORTANT 2 — allow match is path-component-boundary aware: allow "./workspace" must NOT also
// allow a sibling like "./workspace-backup" or "./workspaceX" (the old starts_with over-matched).
// The root itself and any path UNDER it are allowed; a sibling -> unscoped -> confirm.
TEST(CapabilityPolicy, AllowMatchIsPathBoundaryAware) {
  CapabilityScope s;
  s.fs_read_allow     = {"./workspace"};
  s.fs_deny           = {};
  s.confirm_unscoped  = true;
  Blackboard bb; CapabilityPolicy p(std::move(s));
  EXPECT_FALSE(p.veto(bb, read("./workspace/notes.txt")).vetoed);   // under allow root -> allow
  EXPECT_FALSE(p.veto(bb, read("./workspace")).vetoed);             // the root itself -> allow
  auto sib = p.veto(bb, read("./workspace-backup/secrets"));        // sibling -> NOT allowed
  EXPECT_TRUE(sib.vetoed); EXPECT_TRUE(sib.needs_confirm);          // unscoped -> confirm
  auto sibx = p.veto(bb, read("./workspaceX"));
  EXPECT_TRUE(sibx.vetoed); EXPECT_TRUE(sibx.needs_confirm);
}

// MINOR — a trailing dot (FQDN-absolute form) must not dodge the loopback/private classifier:
// "localhost." and "127.0.0.1." are still private.
TEST(CapabilityPolicy, TrailingDotHostStillClassifiedPrivate) {
  Blackboard bb; CapabilityPolicy p(make_scope());
  for (const char* u : {"http://localhost./", "http://127.0.0.1./x"}) {
    auto v = p.veto(bb, fetch(u));
    EXPECT_TRUE(v.vetoed) << u; EXPECT_FALSE(v.needs_confirm) << u;
  }
  EXPECT_TRUE(CapabilityPolicy::is_private_host("localhost."));
  EXPECT_TRUE(CapabilityPolicy::is_private_host("127.0.0.1."));
}

TEST(CapabilityPolicy, SkillToolsHaveDistinctCapabilities) {
  EXPECT_EQ(CapabilityPolicy::capability_of("use_skill"), Capability::SkillRead);
  EXPECT_EQ(CapabilityPolicy::capability_of("save_skill"), Capability::SkillWrite);
}

TEST(CapabilityPolicy, SkillToolsAreAllowedWithoutConfirm) {
  CapabilityScope sc;              // defaults: confirm_unscoped = true — proves these are NOT
  CapabilityPolicy p(sc);          // falling through to the Unknown->confirm path
  Blackboard bb;
  Action use{Action::Kind::ToolCall};
  use.tool = "use_skill";
  use.args = {{"name", "greet"}};
  EXPECT_FALSE(p.veto(bb, use).vetoed);
  Action save{Action::Kind::ToolCall};
  save.tool = "save_skill";
  save.args = {{"name", "greet"}, {"description", "d"}, {"body", "b"}};
  EXPECT_FALSE(p.veto(bb, save).vetoed);
}
