// tests/test_capability_wiring.cpp — wiring of the capability_policy Objective (Task 2).
//
// Task 2 wires CapabilityPolicy into make_objective (app/agent_wiring.cpp): an
// `Objective = capability_policy { … }` manifest block (space-split list values for the
// path/host scopes) becomes a CapabilityPolicy added to the Arbiter alongside (and BEFORE)
// avoid_destructive, and manifests/dev.hades ships that block to close the ungated
// fs_read/http_fetch holes on the live path.
//
// make_objective is file-local (anonymous namespace), so it cannot be called directly.
// These tests lock the two halves of the contract instead:
//   (1) the SHIPPED dev.hades parses a well-formed, MULTI-LINE capability_policy block with
//       NO warnings — a regression guard vs the one-kv-per-line manifest gotcha (a packed
//       line would make enforce_manifest reject the whole manifest at boot);
//   (2) the parse->scope->veto path the make_objective branch performs (space-split list
//       values into a CapabilityScope) gates exactly as configured.

#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/objective/capability_policy.h"
using namespace hades;

namespace {
// Mirror of the whitespace-split the make_objective branch uses for scope list values.
std::vector<std::string> split_ws(const std::string& v) {
  std::vector<std::string> out;
  std::istringstream is(v);
  std::string w;
  while (is >> w) out.push_back(w);
  return out;
}
}  // namespace

// (1) The shipped dev.hades must carry a MULTI-LINE capability_policy objective whose keys all
// parsed (non-empty list values) and produced NO warnings (single-kv-per-line). DEV_MANIFEST is
// the absolute source path (a compile definition), so this is independent of the test cwd.
TEST(CapabilityWiring, DevManifestShipsMultilineCapabilityBlock) {
  std::ifstream f(DEV_MANIFEST);
  ASSERT_TRUE(f.good());
  std::stringstream ss;
  ss << f.rdbuf();
  Manifest m = parse_manifest(ss.str());
  EXPECT_TRUE(m.warnings.empty()) << (m.warnings.empty() ? "" : m.warnings.front());
  EXPECT_TRUE(fatal_warnings(m).empty());
  bool found = false;
  for (const auto& b : m.of("Objective"))
    if (b.name == "capability_policy") {
      found = true;
      ASSERT_TRUE(b.kv.count("fs_read_allow"));
      ASSERT_TRUE(b.kv.count("fs_deny"));
      EXPECT_FALSE(b.kv.at("fs_read_allow").empty());
      EXPECT_FALSE(b.kv.at("fs_deny").empty());
      // Multi-line parse worked: each key holds a whitespace-separated list, not a packed kv.
      EXPECT_GT(split_ws(b.kv.at("fs_read_allow")).size(), 1u);
      EXPECT_GT(split_ws(b.kv.at("fs_deny")).size(), 1u);
    }
  EXPECT_TRUE(found) << "dev.hades must ship a capability_policy objective";
}

// (2) Reproduce the parse->scope the make_objective branch performs (space-split list values)
// and assert the resulting objective hard-vetoes a denied read and a loopback fetch but allows
// an in-scope read — the exact contract the wiring branch must honor.
TEST(CapabilityWiring, ScopeFromSpaceSplitListGatesAsConfigured) {
  Manifest m = parse_manifest(
      "Objective = capability_policy\n{\n"
      "  fs_read_allow = ./ ./workspace\n"
      "  fs_deny       = /etc secrets/\n"
      "}\n");
  ASSERT_TRUE(fatal_warnings(m).empty());
  const std::vector<Block> objs = m.of("Objective");  // of() returns by value — keep it alive
  ASSERT_FALSE(objs.empty());
  const Block& b = objs.front();
  EXPECT_EQ(b.name, "capability_policy");
  CapabilityScope sc;
  sc.fs_read_allow = split_ws(b.kv.at("fs_read_allow"));
  sc.fs_deny       = split_ws(b.kv.at("fs_deny"));
  CapabilityPolicy p(std::move(sc));
  Blackboard bb;
  Action denied{Action::Kind::ToolCall};
  denied.tool = "fs_read";
  denied.args = {{"path", "/etc/shadow"}};
  Action ok{Action::Kind::ToolCall};
  ok.tool = "fs_read";
  ok.args = {{"path", "./workspace/n"}};
  Action ssrf{Action::Kind::ToolCall};
  ssrf.tool = "http_fetch";
  ssrf.args = {{"url", "http://127.0.0.1/"}};
  EXPECT_TRUE(p.veto(bb, denied).vetoed);
  EXPECT_FALSE(p.veto(bb, denied).needs_confirm);
  EXPECT_FALSE(p.veto(bb, ok).vetoed);
  EXPECT_TRUE(p.veto(bb, ssrf).vetoed);
  EXPECT_FALSE(p.veto(bb, ssrf).needs_confirm);
}
