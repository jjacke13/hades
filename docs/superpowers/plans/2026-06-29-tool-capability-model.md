# Tool Capability Model (v1 minimal slice) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement
> this plan task-by-task. Steps use checkbox (`- [ ]`) syntax. RED → GREEN → COMMIT per task.

**Goal:** Close the two open tool holes (`fs_read` reads any file; `http_fetch` reaches any host incl.
loopback/RFC1918) by adding a `CapabilityPolicy` Objective: a built-in tool→capability table + scoped
allow/deny + confirm-on-escalation, enforced at the existing Arbiter veto seam. Keep the 7 tools and
119 tests green (the objective only acts when present in the manifest).

**Architecture:** A new `Objective` (`src/objective/capability_policy.{h,cpp}`) classifies a `ToolCall`
Action by a code-reviewed `capability_of(tool)` table, then maps it to allow / confirm / hard-veto using
operator-supplied scopes (path-prefix allow/deny for `FsRead`/`FsWrite`; private-host hard-veto for
`Net`). It reuses `VetoResult{vetoed, reason, needs_confirm}` and plugs into `Arbiter::dispatch_or_gate`
unchanged. `make_objective` (`app/agent_wiring.cpp`) parses an `Objective = capability_policy { … }`
block into it; `dev.hades` adds the block. Full design:
`docs/superpowers/specs/2026-06-29-tool-capability-model-design.md`.

**Tech Stack:** C++20 · CMake + Ninja · Nix dev shell · GoogleTest · nlohmann_json.

## Global Constraints

- **C++20**, g++ 15.2. **Every build/test command runs inside `nix develop`.**
- **TDD:** RED → GREEN → COMMIT per task. Commit `<type>: <desc>`, **no attribution footer**.
- **Immutability / copy-then-modify** for any config block handling (match existing `wire_agent` style).
- **Backward compat is load-bearing:** `capability_policy` MUST only affect agents whose manifest lists
  it. The test-overload `build_agent` callers pass objective lists that omit it → unchanged. Do **not**
  alter `AvoidDestructive` or `StayOnBudget`. Register `capability_policy` **before** `avoid_destructive`
  when both are present (hard-veto short-circuits first; first `needs_confirm` wins → no double-confirm).
- **Manifest gotcha:** the `capability_policy` block MUST be multi-line (one kv per line). Single-line
  multi-kv mis-parses. The wiring lock test guards this.

---

## File Structure

```
include/hades/objective/capability_policy.h   T1 (new)   Capability enum, CapabilityScope, CapabilityPolicy
src/objective/capability_policy.cpp            T1 (new)   capability_of table, parse_host, is_private_host, veto()
tests/test_capability_policy.cpp               T1 (new)   unit tests (objective + helpers)
app/agent_wiring.cpp                           T2 (modify) make_objective: build CapabilityPolicy from block
manifests/dev.hades                            T2 (modify) add Objective = capability_policy { … }
tests/test_capability_wiring.cpp               T2 (new)   make_objective builds it; dev.hades parse lock
CMakeLists.txt                                 T1+T2       register src + both test files
```

---

## Task 1: `CapabilityPolicy` objective (taxonomy table + scopes + veto)

**Files:** Create `include/hades/objective/capability_policy.h`, `src/objective/capability_policy.cpp`,
`tests/test_capability_policy.cpp`. Modify `CMakeLists.txt`.

**Interfaces — Produces:** `hades::Capability` (enum), `hades::CapabilityScope` (config struct),
`hades::CapabilityPolicy : Objective` with `veto()`, and the static helpers `CapabilityPolicy::
capability_of(tool)`, `CapabilityPolicy::parse_host(url)`, `CapabilityPolicy::is_private_host(host)`.
**Consumes:** `hades/objective.h` (`Objective`, `Action`, `VetoResult`).

- [ ] **Step 1: Write the failing tests** — `tests/test_capability_policy.cpp`:

```cpp
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
```

`CMakeLists.txt` — register the source + test (near the other objective lines ~77-79):
```cmake
target_sources(hades_core PRIVATE src/objective/capability_policy.cpp)
target_sources(hades_tests PRIVATE tests/test_capability_policy.cpp)
```

- [ ] **Step 2: Run, expect FAIL** — `nix develop --command cmake -S . -B build -G Ninja && nix develop
  --command cmake --build build` → fails: `hades/objective/capability_policy.h` not found.

- [ ] **Step 3: Implement.**

`include/hades/objective/capability_policy.h`:
```cpp
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
```

`src/objective/capability_policy.cpp`:
```cpp
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
  if (auto at = rest.find('@'); at != std::string::npos) rest = rest.substr(at + 1);  // strip userinfo
  std::size_t end = rest.find_first_of("/:?#");
  std::string host = (end == std::string::npos) ? rest : rest.substr(0, end);
  // bracketed IPv6 literal: keep inner address (so "[::1]" -> "::1")
  if (!host.empty() && host.front() == '[') {
    auto rb = host.find(']');
    host = host.substr(1, rb == std::string::npos ? std::string::npos : rb - 1);
  }
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
```

- [ ] **Step 4: Build + test** — `nix develop --command cmake --build build && nix develop --command
  ctest --test-dir build --output-on-failure -R CapabilityPolicy` → all new tests PASS.

- [ ] **Step 5: Commit**
```bash
git add include/hades/objective/capability_policy.h src/objective/capability_policy.cpp \
        tests/test_capability_policy.cpp CMakeLists.txt
git commit -m "feat: CapabilityPolicy objective (scoped fs_read/http_fetch allow-confirm-deny)"
```

---

## Task 2: wire `capability_policy` into `make_objective` + `dev.hades`

**Files:** Modify `app/agent_wiring.cpp`, `manifests/dev.hades`, `CMakeLists.txt`. Create
`tests/test_capability_wiring.cpp`.

**Interfaces — Consumes:** `CapabilityPolicy` / `CapabilityScope` (T1), `make_objective` (existing),
`parse_manifest`. **Produces:** `make_objective` builds a `CapabilityPolicy` from an
`Objective = capability_policy { … }` block (space-split list values); `dev.hades` ships the block.

**Read `app/agent_wiring.cpp` lines 24-37 (the `make_objective` anonymous-namespace helper) before
editing.** You are adding one branch; do not touch `wire_agent` ordering or the existing objective
branches. The objective is built and added to the Arbiter by the unchanged loop at lines ~129-130.

- [ ] **Step 1: Write the failing test** — `tests/test_capability_wiring.cpp`:

```cpp
// tests/test_capability_wiring.cpp — make_objective builds CapabilityPolicy from a manifest block,
// and the shipped dev.hades parses into a capability_policy Objective (regression lock vs the
// one-kv-per-line manifest gotcha: the block must stay multi-line).
#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include "hades/config.h"
#include "app/agent_wiring.h"           // make_objective is file-local; test via the public seam below
#include "hades/objective/capability_policy.h"
#include "hades/blackboard.h"
using namespace hades;

// Mirror of make_objective's capability_policy branch is NOT accessible (anonymous ns); instead this
// test asserts the manifest the wiring consumes is well-formed AND drives the objective end-to-end by
// constructing the scope the same way the branch will. The dev.hades lock is the load-bearing check.

TEST(CapabilityWiring, DevManifestShipsMultilineCapabilityBlock) {
  std::ifstream f("manifests/dev.hades");
  ASSERT_TRUE(f.good()) << "run from repo root";
  std::stringstream ss; ss << f.rdbuf();
  Manifest m = parse_manifest(ss.str());
  EXPECT_TRUE(m.warnings.empty()) << (m.warnings.empty() ? "" : m.warnings.front());
  bool found = false;
  for (const auto& b : m.of("Objective"))
    if (b.name == "capability_policy") {
      found = true;
      // multi-line parse worked: each declared key is present and non-empty
      EXPECT_FALSE(b.kv.at("fs_read_allow").empty());
      EXPECT_FALSE(b.kv.at("fs_read_deny").empty());
    }
  EXPECT_TRUE(found) << "dev.hades must ship a capability_policy objective";
}

TEST(CapabilityWiring, ScopeFromSpaceSplitListGatesAsConfigured) {
  // Reproduce the parse the make_objective branch performs (space-split list values) and assert the
  // resulting objective hard-vetoes a denied read and a loopback fetch but allows an in-scope read.
  Manifest m = parse_manifest(
      "Objective = capability_policy\n{\n"
      "  fs_read_allow = ./ ./workspace\n"
      "  fs_read_deny  = /etc secrets/\n"
      "}\n");
  const Block& b = m.of("Objective").front();
  auto split = [](const std::string& v) {
    std::vector<std::string> out; std::istringstream is(v); std::string w;
    while (is >> w) out.push_back(w); return out;
  };
  CapabilityScope sc;
  sc.fs_read_allow = split(b.kv.at("fs_read_allow"));
  sc.fs_read_deny  = split(b.kv.at("fs_read_deny"));
  CapabilityPolicy p(sc);
  Blackboard bb;
  Action denied{Action::Kind::ToolCall};  denied.tool="fs_read"; denied.args={{"path","/etc/shadow"}};
  Action ok{Action::Kind::ToolCall};      ok.tool="fs_read";     ok.args={{"path","./workspace/n"}};
  Action ssrf{Action::Kind::ToolCall};    ssrf.tool="http_fetch";ssrf.args={{"url","http://127.0.0.1/"}};
  EXPECT_TRUE (p.veto(bb,denied).vetoed); EXPECT_FALSE(p.veto(bb,denied).needs_confirm);
  EXPECT_FALSE(p.veto(bb,ok).vetoed);
  EXPECT_TRUE (p.veto(bb,ssrf).vetoed);   EXPECT_FALSE(p.veto(bb,ssrf).needs_confirm);
}
```

`CMakeLists.txt`:
```cmake
target_sources(hades_tests PRIVATE tests/test_capability_wiring.cpp)
```

- [ ] **Step 2: Run, expect FAIL** — `nix develop --command cmake -S . -B build -G Ninja && nix develop
  --command cmake --build build && nix develop --command ctest --test-dir build -R CapabilityWiring`.
  `DevManifestShipsMultilineCapabilityBlock` FAILS (dev.hades has no such block yet). The
  `ScopeFromSpaceSplitListGatesAsConfigured` test should already PASS (it builds the scope itself) —
  it locks the parse→scope contract the wiring branch must match.

- [ ] **Step 3: Implement.**

**(a)** In `app/agent_wiring.cpp`, add the include near the other objective includes (lines 19-20):
```cpp
#include "hades/objective/capability_policy.h"
```
Add a file-local space-split helper inside the anonymous namespace (above `make_objective`):
```cpp
// Split a manifest value into a whitespace-separated list (the one place whitespace is an
// intended list separator; tool store paths forbid it, scopes use it).
std::vector<std::string> split_ws_list(const std::string& v) {
  std::vector<std::string> out;
  std::istringstream is(v);
  std::string w;
  while (is >> w) out.push_back(w);
  return out;
}
```
(Add `#include <sstream>` if not already present.) Extend `make_objective` with one branch (after the
`avoid_destructive` branch, before `return nullptr`):
```cpp
  if (b.name == "capability_policy") {
    CapabilityScope sc;
    if (b.kv.count("fs_read_allow"))  sc.fs_read_allow  = split_ws_list(b.kv.at("fs_read_allow"));
    if (b.kv.count("fs_read_deny"))   sc.fs_read_deny   = split_ws_list(b.kv.at("fs_read_deny"));
    if (b.kv.count("net_deny_hosts")) sc.net_deny_hosts = split_ws_list(b.kv.at("net_deny_hosts"));
    if (b.kv.count("block_private_net")) set_bool_on_string(b.kv.at("block_private_net"), sc.block_private_net);
    if (b.kv.count("confirm_unscoped"))  set_bool_on_string(b.kv.at("confirm_unscoped"),  sc.confirm_unscoped);
    return std::make_unique<CapabilityPolicy>(std::move(sc));
  }
```

**(b)** In `manifests/dev.hades`, add the block (multi-line!) **before** `Objective = avoid_destructive`
so it is registered first (hard-veto short-circuits before the pattern backstop). Keep
`avoid_destructive` as the defense-in-depth backstop:
```
Objective = capability_policy
{
  fs_read_allow     = ./ ./workspace ./docs ./manifests ./prompts ./web
  fs_read_deny      = /etc /root .env secrets/ .ssh/ .git/credentials id_rsa
  block_private_net = true
  confirm_unscoped  = true
}
```
(Place it between the existing `Objective = stay_on_budget …` and `Objective = avoid_destructive …`
lines. `stay_on_budget`/`avoid_destructive` are single-line and stay as-is; only the new block needs
multi-line because it has multiple keys.)

- [ ] **Step 4: Build + FULL suite** — `nix develop --command cmake -S . -B build -G Ninja && nix
  develop --command cmake --build build && nix develop --command ctest --test-dir build
  --output-on-failure`. Expected: **all green** — both new `CapabilityWiring` tests pass; the prior 119
  + Task-1 tests are unaffected (the test-overload agents never include `capability_policy`; `dev.hades`
  is only parsed by the new lock test).

- [ ] **Step 5: Commit**
```bash
git add app/agent_wiring.cpp manifests/dev.hades tests/test_capability_wiring.cpp CMakeLists.txt
git commit -m "feat: wire capability_policy objective into make_objective + dev.hades roster"
```

---

## Self-Review (against the spec)

- **Spec coverage:** capability taxonomy table (`capability_of`, T1) · built-in-table authority (no
  self-describe trust) · path-prefix `fs_read` allow/deny + confirm-unscoped · `Net` private-host
  hard-veto (SSRF/T3) + deny-host substrings · `FsWrite`/`Exec` confirm · `MemoryAppend` allow ·
  `Unknown`→confirm (T5) · enforcement at the Arbiter veto seam via `VetoResult` (no new path) ·
  registered before `avoid_destructive` (compose, backstop kept) · default = inert unless in the
  manifest (backward compat) · `dev.hades` closes T1/T3/T4/T5.
- **Threats closed in v1:** T1 (deny-path read), T3 (loopback/RFC1918 fetch), T4 (out-of-scope read →
  confirm), T5 (unknown tool → confirm). T2 partial (private half); full host-allowlist is v2.
- **Out of scope honored:** no path canonicalization (literal-prefix, documented), no DNS resolution, no
  positive net allowlist, no ToolRunner second layer, no AvoidDestructive fold-in — all v2.
- **Backward compat:** 119 tests use the test-overload `build_agent` (explicit objective lists without
  `capability_policy`) → unchanged; only `dev.hades` gains the block; new lock test guards the
  multi-line parse.
- **Type/name consistency:** `Capability`, `CapabilityScope`, `CapabilityPolicy`, `capability_of`,
  `parse_host`, `is_private_host`, `split_ws_list` consistent across T1/T2.

## Verification

1. `nix develop --command cmake -S . -B build -G Ninja && nix develop --command cmake --build build &&
   nix develop --command ctest --test-dir build --output-on-failure` → all green (121 = 119 + new).
2. Live smoke (manual): `nix develop --command ./build/hades manifests/dev.hades` then ask the agent to
   read `/etc/shadow` → `[blocked: read of a denied path: /etc/shadow]`; ask it to `http_fetch`
   `http://127.0.0.1:8080/` → `[blocked: net fetch to a private/loopback host: 127.0.0.1]`; ask it to
   read `./README.md` → succeeds (in scope).
