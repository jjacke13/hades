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
  s.fs_read_deny  = {"/etc", ".env", "secrets/"};
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
