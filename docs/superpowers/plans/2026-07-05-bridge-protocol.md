# Bridge-as-protocol Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Agents advertise a standardized A2A-shaped capability **card** peers discover (pull) and receive (push), share typed content with per-peer trust tiers, and the Arbiter folds known-peer capabilities + peer reports into each turn's system prompt — so delegation routes by advertised capability.

**Architecture:** Two channels over the existing Bridge. **Card (pull):** secret-gated `GET /card` serving an on-demand A2A-shaped card; a discovery timer re-pulls each manifest peer's card → `PEER.<peer>.card`. **Typed `/share` (push):** a `type` field (`card`/`fact`/`raw`, tolerant default `raw`); a fresh/changed agent self-announces its card to peers; inbound facts are trust-labeled. The Arbiter `subscribe("PEER.*")`s into a local map (Blackboard already supports wildcard patterns — no core change) and folds two blocks at turn start. Spec: `docs/superpowers/specs/2026-07-05-bridge-protocol-blackboard-vars-design.md`.

**Tech Stack:** C++20, CMake+Ninja in `nix develop`, nlohmann_json, cpr, httplib, GoogleTest, std::thread.

## Global Constraints

- **Every build/test runs inside `nix develop`:** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure`. **Baseline: 426/426 green** before Task 1.
- Branch `feat/bridge-protocol` (already created; spec committed). Commit style `<type>: <desc>` — **NO attribution footer, NO Co-Authored-By**.
- **Tolerant parse everywhere:** unknown JSON fields ignored; a missing/typed-wrong field degrades, never throws (the existing bridge-protocol style). Pump-thread handlers and the discovery-thread body must **never throw** (try/catch; degrade).
- **Share `type` default is `"raw"`** — absent type reproduces today's exact `/share` behavior (backward-compat; existing shares and tests unchanged).
- **`caps{}` in the card is a SUMMARY** — categories/booleans only (`"scoped"`/`"public"`/`"none"`), **NEVER** the literal fs paths or exec prefixes.
- **`/card` is secret-gated** (same `X-Hades-Bridge` check as `/health`).
- **Teardown:** the discovery timer thread is stop-flagged + notified + **joined in `~BridgeModule`** (alongside the listener join), before any member it touches dies. `Agent::bridge` stays destroyed before `executor` (unchanged). Do NOT reorder Agent members.
- **Threads started by `hades_main`, never `on_attach`** — tests spawn no discovery thread unless they call the starter explicitly (the listener/telegram precedent).
- Secret stays **env-only, redacted** in `session.log` (unchanged). `kMaxShareBytes` still caps typed shares.
- **Do NOT stage** the user's uncommitted `manifests/dev.hades` live edits, `memory/facts.md`, or untracked `skills/` / `manifests/dev2.hades` / `build-tsan/`. Task 9's dev.hades edit touches only the **commented** Bridge/Peer example; commit it split-clean (from the clean base, not his live edits — the prior stash-dance).
- File headers: `// path — one-line purpose` + a short explanation block (house style).
- **TSan lane at feature end** (listener + discovery timer threads): the reviewer/controller runs the TSan build after Task 8.

---

## File Structure

```
include/hades/bridge/registry.h        T1  canonical card schema + share-type constants + pure builder decls
src/apps/bridge/bridge.cpp             T1,T2,T3,T4,T5,T6  builders + typed envelope + card seam + discovery + push + routing (existing TU)
include/hades/bridge/protocol.h        T2  `type` on build_share/parse_share/BridgeMsg
include/hades/bridge/http.h            T4  BridgeHttp::get_json
include/hades/module/bridge_module.h   T3,T4,T5,T6  setters, card_json, discovery thread, trust map
tests/test_bridge_registry.cpp         T1  (new)
tests/test_bridge_protocol.cpp         T2  (append)
tests/test_bridge_module.cpp           T3,T4,T5,T6  (append)
include/hades/arbiter.h                T7  peer_vars_ member
src/apps/arbiter/arbiter.cpp           T7  subscribe PEER.* + fold at start_turn
tests/test_arbiter.cpp                 T7  (append)
app/agent_wiring.cpp                   T8  set_description/tools/caps/peer_trust + caps_summary + discover_interval
app/hades_main.cpp                     T8  start_discovery after wiring
tests/test_bridge_wiring.cpp           T8  (append)
manifests/dev.hades, docs/manifest-reference.md, prompts/soul.md, CLAUDE.md  T9  ship
CMakeLists.txt                         T1  (add test_bridge_registry.cpp)
```

---

## Task 1: Registry — card schema constants + pure builders

**Files:** Create `include/hades/bridge/registry.h`; add builders to `src/apps/bridge/bridge.cpp`; Test `tests/test_bridge_registry.cpp`; Modify `CMakeLists.txt`.

**Interfaces — Produces (all `namespace hades`):**
- `inline constexpr const char* kShareTypeCard = "card";` / `kShareTypeFact = "fact";` / `kShareTypeRaw = "raw";`
- `inline constexpr const char* kCardKey = "card";` (the share key + `PEER.<from>.card` suffix)
- `nlohmann::json build_skills_from_announce(const std::string& announce)` — reverse-parse `"- <id>: <desc>"` lines → `[{"id","description"}]`; non-matching lines skipped; `""`/no lines → `[]`.
- `nlohmann::json caps_summary(const Block& capability_policy_cfg)` — `{"fs_read","fs_write","net","exec"}`, values `"scoped"`/`"public"`/`"none"`; NO literal paths.
- `nlohmann::json build_card(const std::string& name, const std::string& url, int version, const std::string& description, const std::string& skills_announce, const nlohmann::json& tools, const nlohmann::json& caps)` — the A2A-shaped card.

- [ ] **Step 1: Write the failing tests** `tests/test_bridge_registry.cpp`:

```cpp
// tests/test_bridge_registry.cpp — pure bridge card builders (reverse-parse, caps summary, card)
#include <gtest/gtest.h>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/bridge/registry.h"
#include "hades/config.h"   // Block
using namespace hades;

TEST(BridgeRegistry, SkillsReverseParseFromAnnounce) {
  const std::string ann =
      "Available skills (call use_skill with a name to load its full instructions):\n"
      "- deploy: ship the app\n"
      "- triage: sort incoming issues";
  auto s = build_skills_from_announce(ann);
  ASSERT_EQ(s.size(), 2u);
  EXPECT_EQ(s[0].value("id", ""), "deploy");
  EXPECT_EQ(s[0].value("description", ""), "ship the app");
  EXPECT_EQ(s[1].value("id", ""), "triage");
}

TEST(BridgeRegistry, SkillsReverseParseEmptyOrJunk) {
  EXPECT_TRUE(build_skills_from_announce("").empty());
  EXPECT_TRUE(build_skills_from_announce("no skill lines here").empty());
  // a header-only announce (no "- " lines) -> []
  EXPECT_TRUE(build_skills_from_announce("Available skills (…):").empty());
}

TEST(BridgeRegistry, CapsSummaryIsCategoriesNotPaths) {
  Block b;
  b.name = "capability_policy";
  b.kv["fs_read_allow"]  = "./workspace ./prompts";
  b.kv["fs_write_allow"] = "./workspace";
  b.kv["block_private_net"] = "true";
  b.kv["exec_allow"] = "cmake --build build, ctest --test-dir build";
  auto c = caps_summary(b);
  EXPECT_EQ(c.value("fs_read", ""), "scoped");
  EXPECT_EQ(c.value("fs_write", ""), "scoped");
  EXPECT_EQ(c.value("exec", ""), "scoped");
  EXPECT_EQ(c.value("net", ""), "private-blocked");
  // CRITICAL: no literal path/command leaks anywhere in the summary
  const std::string dump = c.dump();
  EXPECT_EQ(dump.find("workspace"), std::string::npos);
  EXPECT_EQ(dump.find("cmake"), std::string::npos);
}

TEST(BridgeRegistry, CapsSummaryDefaultsWhenUnset) {
  auto c = caps_summary(Block{});
  EXPECT_EQ(c.value("fs_read", ""), "none");
  EXPECT_EQ(c.value("exec", ""), "none");
  EXPECT_EQ(c.value("net", ""), "public");   // no block_private_net -> public egress
}

TEST(BridgeRegistry, BuildCardIsA2AShaped) {
  nlohmann::json tools = nlohmann::json::array({{{"name", "shell"}}, {{"name", "http_fetch"}}});
  nlohmann::json caps = {{"fs_read", "scoped"}, {"net", "public"}};
  auto card = build_card("hades2", "http://h:9090", 1, "a helper",
                         "Available skills (…):\n- deploy: ship it", tools, caps);
  EXPECT_EQ(card.value("name", ""), "hades2");
  EXPECT_EQ(card.value("description", ""), "a helper");
  EXPECT_EQ(card.value("url", ""), "http://h:9090");
  EXPECT_EQ(card.value("version", 0), 1);
  ASSERT_TRUE(card["skills"].is_array());
  EXPECT_EQ(card["skills"][0].value("id", ""), "deploy");
  EXPECT_TRUE(card["capabilities"].value("streaming", true) == false);
  EXPECT_EQ(card["tools"][0].value("name", ""), "shell");
  EXPECT_EQ(card["caps"].value("fs_read", ""), "scoped");
}
```

- [ ] **Step 2: Add the test to CMake and run — expect FAIL (missing header).** In `CMakeLists.txt`, after the `tests/test_bridge_protocol.cpp` line (~52):

```cmake
target_sources(hades_tests PRIVATE tests/test_bridge_registry.cpp)
```

Run `nix develop --command cmake --build build` → compile error (no `hades/bridge/registry.h`).

- [ ] **Step 3: Implement.** `include/hades/bridge/registry.h`:

```cpp
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
```

Append to `src/apps/bridge/bridge.cpp` (a new `// ── bridge registry: card builders ──` section, `namespace hades`):

```cpp
// ── bridge registry: canonical card builders (registry.h) ──────────────
#include "hades/bridge/registry.h"
#include <sstream>
namespace hades {

nlohmann::json build_skills_from_announce(const std::string& announce) {
  nlohmann::json out = nlohmann::json::array();
  std::istringstream is(announce);
  std::string line;
  while (std::getline(is, line)) {
    if (line.rfind("- ", 0) != 0) continue;              // only "- id: desc" list lines
    const std::size_t colon = line.find(": ", 2);
    if (colon == std::string::npos) continue;
    std::string id = line.substr(2, colon - 2);
    std::string desc = line.substr(colon + 2);
    if (id.empty()) continue;
    out.push_back({{"id", id}, {"description", desc}});
  }
  return out;
}

nlohmann::json caps_summary(const Block& cfg) {
  auto has = [&](const char* k) { return cfg.kv.count(k) && !cfg.kv.at(k).empty(); };
  bool block_priv = false;
  if (cfg.kv.count("block_private_net")) {
    const std::string& v = cfg.kv.at("block_private_net");
    block_priv = (v == "true" || v == "1" || v == "yes");
  }
  return {{"fs_read",  has("fs_read_allow")  ? "scoped" : "none"},
          {"fs_write", has("fs_write_allow") ? "scoped" : "none"},
          {"exec",     has("exec_allow")     ? "scoped" : "none"},
          {"net",      block_priv ? "private-blocked" : "public"}};
}

nlohmann::json build_card(const std::string& name, const std::string& url, int version,
                          const std::string& description, const std::string& skills_announce,
                          const nlohmann::json& tools, const nlohmann::json& caps) {
  return {{"name", name},
          {"description", description.empty() ? name : description},
          {"url", url},
          {"version", version},
          {"capabilities", {{"streaming", false}}},
          {"skills", build_skills_from_announce(skills_announce)},
          {"tools", tools.is_array() ? tools : nlohmann::json::array()},
          {"caps", caps.is_object() ? caps : nlohmann::json::object()}};
}

}  // namespace hades
```

- [ ] **Step 4: Build + test.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R BridgeRegistry` → pass. Then full suite.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/bridge/registry.h src/apps/bridge/bridge.cpp tests/test_bridge_registry.cpp CMakeLists.txt
git commit -m "feat: bridge registry — A2A card builder, skills reverse-parse, caps summary"
```

---

## Task 2: Typed `/share` envelope

**Files:** Modify `include/hades/bridge/protocol.h`, `src/apps/bridge/bridge.cpp`; Test append `tests/test_bridge_protocol.cpp`.

**Interfaces:**
- `BridgeMsg` gains `std::string type;` (parsed value; `"raw"` when absent).
- `build_share(from, key, value, type = "raw")` — 4th param; emits `"type"` only helps forward peers, but ALWAYS include it for clarity.
- `parse_share` fills `m.type` (absent/non-string → `"raw"`).

- [ ] **Step 1: Write the failing tests** — append to `tests/test_bridge_protocol.cpp` (match its style):

```cpp
TEST(BridgeProtocol, ShareTypeDefaultsToRawWhenAbsent) {
  // A body with no "type" field (the pre-feature wire form) parses as raw.
  auto j = nlohmann::json{{"v", kBridgeProtocolV}, {"from", "a"}, {"key", "K"}, {"value", 1}};
  BridgeMsg m = parse_share(j.dump());
  ASSERT_TRUE(m.ok);
  EXPECT_EQ(m.type, "raw");
}

TEST(BridgeProtocol, ShareTypeRoundTrips) {
  BridgeMsg m = parse_share(build_share("a", "card", {{"name", "a"}}, kShareTypeCard).dump());
  ASSERT_TRUE(m.ok);
  EXPECT_EQ(m.type, "card");
  EXPECT_EQ(m.key, "card");
  EXPECT_EQ(m.value.value("name", ""), "a");
}

TEST(BridgeProtocol, ShareTypeNonStringIsRaw) {
  auto j = nlohmann::json{{"v", kBridgeProtocolV}, {"from", "a"}, {"key", "K"},
                          {"value", 1}, {"type", 42}};
  BridgeMsg m = parse_share(j.dump());
  ASSERT_TRUE(m.ok);
  EXPECT_EQ(m.type, "raw");   // tolerant: bad type degrades, does not reject
}
```

(Add `#include "hades/bridge/registry.h"` to the test if `kShareTypeCard` is not already visible.)

- [ ] **Step 2: Run — expect FAIL** (`BridgeMsg` has no `type`).
- [ ] **Step 3: Implement.** In `include/hades/bridge/protocol.h`, add to `struct BridgeMsg` (after `value`):

```cpp
  std::string type = "raw";   // /share only: "card" | "fact" | "raw" (default; absent -> raw)
```

Change the `build_share` decl to:

```cpp
nlohmann::json build_share(const std::string& from, const std::string& key,
                           const nlohmann::json& value, const std::string& type = "raw");
```

In `src/apps/bridge/bridge.cpp`, update `build_share`:

```cpp
nlohmann::json build_share(const std::string& from, const std::string& key,
                           const nlohmann::json& value, const std::string& type) {
  return {{"v", kBridgeProtocolV}, {"from", from}, {"key", key}, {"value", value}, {"type", type}};
}
```

and in `parse_share`, right before `m.ok = true;`:

```cpp
  auto ty = j.find("type");                       // tolerant: absent / non-string -> "raw"
  m.type = (ty != j.end() && ty->is_string()) ? ty->get<std::string>() : "raw";
```

- [ ] **Step 4: Build + test.** `-R BridgeProtocol` → pass; full suite green (existing `build_share` 3-arg calls still compile via the default).
- [ ] **Step 5: Commit.**

```bash
git add include/hades/bridge/protocol.h src/apps/bridge/bridge.cpp tests/test_bridge_protocol.cpp
git commit -m "feat: typed /share envelope (type field, tolerant default raw)"
```

---

## Task 3: Bridge card seam — `card_json()` + setters + `GET /card`

**Files:** Modify `include/hades/module/bridge_module.h`, `src/apps/bridge/bridge.cpp`; Test append `tests/test_bridge_module.cpp`.

**Interfaces:**
- `void set_description(std::string)`, `void set_tools(nlohmann::json)`, `void set_caps(nlohmann::json)`.
- `nlohmann::json card_json() const` — assembles `build_card(name_, url, kBridgeProtocolV, description_, SKILLS_ANNOUNCE, tools_, caps_)`; `url` = `"http://" + host_ + ":" + std::to_string(port_)`.
- `GET /card` route, secret-gated (like `/health`).

- [ ] **Step 1: Write the failing tests** — append to `tests/test_bridge_module.cpp`:

```cpp
TEST(BridgeModule, CardJsonAssemblesInjectedAndBusInputs) {
  Rig r(false);
  r.mod->set_description("worker one");
  r.mod->set_tools(nlohmann::json::array({{{"name", "shell"}}}));
  r.mod->set_caps({{"fs_read", "scoped"}, {"net", "public"}});
  r.bb.post("SKILLS_ANNOUNCE",
            "Available skills (…):\n- deploy: ship it", "skills");   // latest-value, get() sees it
  auto c = r.mod->card_json();
  EXPECT_EQ(c.value("name", ""), "worker1");
  EXPECT_EQ(c.value("description", ""), "worker one");
  EXPECT_EQ(c["skills"][0].value("id", ""), "deploy");
  EXPECT_EQ(c["tools"][0].value("name", ""), "shell");
  EXPECT_EQ(c["caps"].value("fs_read", ""), "scoped");
  // no literal path leak from a real capability summary shape
  EXPECT_EQ(c.dump().find("workspace"), std::string::npos);
}

TEST(BridgeModule, CardJsonEmptyWhenNoSkillsOrInjection) {
  Rig r(false);
  auto c = r.mod->card_json();
  EXPECT_EQ(c.value("name", ""), "worker1");
  EXPECT_TRUE(c["skills"].is_array());
  EXPECT_TRUE(c["skills"].empty());              // no SKILLS_ANNOUNCE -> []
}
```

Add a `GET /card` leg to the existing `RealSocketAskShareHealthAnd403` test (after the `/health` legs):

```cpp
  // /card with auth -> 200 A2A-shaped; without auth -> 403
  auto cok = cpr::Get(cpr::Url{base + "/card"}, cpr::Header{{"X-Hades-Bridge", "s3cret"}});
  EXPECT_EQ(cok.status_code, 200);
  EXPECT_NE(cok.text.find("worker1"), std::string::npos);
  EXPECT_EQ(cpr::Get(cpr::Url{base + "/card"}).status_code, 403);
```

- [ ] **Step 2: Run — expect FAIL** (`card_json`/setters missing; no `/card` route).
- [ ] **Step 3: Implement.** In `include/hades/module/bridge_module.h` public section:

```cpp
  void set_description(std::string d) { description_ = std::move(d); }
  void set_tools(nlohmann::json t) { tools_ = std::move(t); }
  void set_caps(nlohmann::json c) { caps_ = std::move(c); }
  nlohmann::json card_json() const;   // A2A-shaped; built on demand from injected + bus inputs
```

private members (near `name_`):

```cpp
  std::string description_;
  nlohmann::json tools_ = nlohmann::json::array();
  nlohmann::json caps_ = nlohmann::json::object();
```

In `src/apps/bridge/bridge.cpp`, add `#include "hades/bridge/registry.h"` at the top include block if not present (Task 1 already added it further down — hoist it to the top includes), and implement `card_json` near `health_json`:

```cpp
nlohmann::json BridgeModule::card_json() const {
  std::string skills;
  if (bb_) {                                      // SKILLS_ANNOUNCE is latest-value; absent -> ""
    if (auto e = bb_->get("SKILLS_ANNOUNCE"); e && e->value.is_string())
      skills = e->value.get<std::string>();
  }
  const std::string url = "http://" + host_ + ":" + std::to_string(port_);
  return build_card(name_, url, kBridgeProtocolV, description_, skills, tools_, caps_);
}
```

In `start_listening()`, after the `/health` route, add (same secret gate):

```cpp
  srv_->Get("/card", [this, respond](const httplib::Request& req, httplib::Response& res) {
    if (req.get_header_value("X-Hades-Bridge") != secret_) {
      respond(res, {{"ok", false}, {"error", "forbidden"}});
      return;
    }
    respond(res, card_json());
  });
```

- [ ] **Step 4: Build + test.** `-R BridgeModule` → pass; full suite green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/module/bridge_module.h src/apps/bridge/bridge.cpp tests/test_bridge_module.cpp
git commit -m "feat: bridge card seam — card_json + set_description/tools/caps + GET /card"
```

---

## Task 4: Discovery (pull) — `get_json` + timer thread → `PEER.<peer>.card`

**Files:** Modify `include/hades/bridge/http.h`, `include/hades/module/bridge_module.h`, `src/apps/bridge/bridge.cpp`; Test append `tests/test_bridge_module.cpp`.

**Interfaces:**
- `BridgeHttp::get_json(url, secret, timeout_s)` → `{status, body}` (pure virtual; `CprBridgeHttp` impl; `FakeHttp` in tests overrides).
- `void set_discover_interval_s(double s)`; `void discover_once()` (public for tests — best-effort pull of every peer's `/card` → `PEER.<peer>.card`); `void start_discovery()` (spawns the timer thread; idempotent; no-op if interval `0` after the first pull).

- [ ] **Step 1: Write the failing tests** — append to `tests/test_bridge_module.cpp`. First, extend the existing `FakeHttp` (add a `get_json` override + a canned card); since `FakeHttp` is in an anonymous namespace already, add the method there:

```cpp
// (extend the existing FakeHttp struct)
//   std::string card_body = R"({"v":1})";
//   std::pair<int,std::string> get_json(const std::string& url, const std::string&, double) override {
//     gets.push_back(url); return {get_status, card_body};
//   }
//   std::vector<std::string> gets; int get_status = 200;
```

New test:

```cpp
TEST(BridgeModule, DiscoverOncePullsPeerCardsToBus) {
  Blackboard bb;
  auto http = std::make_unique<FakeHttp>();
  http->card_body = R"({"name":"front","skills":[{"id":"deploy","description":"ship"}]})";
  FakeHttp* h = http.get();
  BridgeModule m(std::move(http), "s3cret");
  Block cfg; cfg.kv["name"] = "worker1";
  m.on_start(cfg, bb);
  m.set_peers({{"front", "http://10.0.0.1:9090"}});
  m.on_attach(bb);
  m.discover_once();
  ASSERT_EQ(h->gets.size(), 1u);
  EXPECT_EQ(h->gets[0], "http://10.0.0.1:9090/card");
  auto e = bb.get("PEER.front.card");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->value.value("name", ""), "front");
}

TEST(BridgeModule, DiscoverFailureLogsBridgeErrorNoThrow) {
  Blackboard bb;
  auto http = std::make_unique<FakeHttp>();
  http->get_status = 0;                                  // transport failure
  BridgeModule m(std::move(http), "s3cret");
  Block cfg; cfg.kv["name"] = "worker1";
  m.on_start(cfg, bb);
  m.set_peers({{"front", "http://10.0.0.1:9090"}});
  m.on_attach(bb);
  std::string err;
  bb.subscribe("BRIDGE_ERROR", [&](const Entry& e) {
    if (e.value.is_string()) err = e.value.get<std::string>();
  });
  m.discover_once();
  bb.pump();
  EXPECT_FALSE(bb.get("PEER.front.card").has_value());
  EXPECT_NE(err.find("front"), std::string::npos);
}
```

- [ ] **Step 2: Run — expect FAIL** (no `get_json`/`discover_once`).
- [ ] **Step 3: Implement.** `include/hades/bridge/http.h` — add to the `BridgeHttp` struct and `CprBridgeHttp`:

```cpp
  virtual std::pair<int, std::string> get_json(const std::string& url, const std::string& secret,
                                               double timeout_s) = 0;
```

```cpp
  std::pair<int, std::string> get_json(const std::string& url, const std::string& secret,
                                       double timeout_s) override;
```

`src/apps/bridge/bridge.cpp` — `CprBridgeHttp::get_json` (next to `post_json`):

```cpp
std::pair<int, std::string> CprBridgeHttp::get_json(const std::string& url,
                                                    const std::string& secret, double timeout_s) {
  try {
    cpr::Response r = cpr::Get(cpr::Url{url}, cpr::Header{{"X-Hades-Bridge", secret}},
                               cpr::Timeout{static_cast<int>(timeout_s * 1000)},
                               cpr::Redirect{false});
    return {static_cast<int>(r.status_code), r.text};
  } catch (const std::exception&) {
    return {0, ""};
  }
}
```

`include/hades/module/bridge_module.h` — public:

```cpp
  void set_discover_interval_s(double s) { discover_interval_s_ = s; }
  void discover_once();     // best-effort pull of every peer's /card -> PEER.<peer>.card
  void start_discovery();   // spawn the periodic pull thread (hades_main; idempotent)
```

private members:

```cpp
  double discover_interval_s_ = 300.0;
  std::thread discovery_thread_;
  std::mutex discovery_mu_;
  std::condition_variable discovery_cv_;
  bool discovery_stop_ = false;
```

(Add `#include <condition_variable>` if not already pulled in via `turn_gate.h`.)

`src/apps/bridge/bridge.cpp` — implement, and extend the dtor to join the discovery thread:

```cpp
void BridgeModule::discover_once() {
  for (const auto& [peer, url] : peers_) {
    try {
      auto [status, body] = http_->get_json(url + "/card", secret_, 10.0);
      if (status < 200 || status >= 300) {
        bb_->post("BRIDGE_ERROR", "card pull from " + peer + " failed (status " +
                                      std::to_string(status) + ")", "bridge");
        continue;
      }
      auto card = nlohmann::json::parse(body, nullptr, false);
      if (card.is_discarded() || !card.is_object()) {
        bb_->post("BRIDGE_ERROR", "card pull from " + peer + " returned non-JSON", "bridge");
        continue;
      }
      bb_->post(peer_bus_key(peer, kCardKey), card, "bridge");   // PEER.<peer>.card
    } catch (...) {
      bb_->post("BRIDGE_ERROR", "card pull from " + peer + " threw", "bridge");
    }
  }
}

void BridgeModule::start_discovery() {
  if (discovery_thread_.joinable()) return;                     // idempotent
  discover_once();                                              // boot pull (first tick)
  if (discover_interval_s_ <= 0.0) return;                      // 0 -> boot-only, no thread
  discovery_thread_ = std::thread([this] {
    std::unique_lock<std::mutex> lk(discovery_mu_);
    while (!discovery_stop_) {
      if (discovery_cv_.wait_for(lk, std::chrono::duration<double>(discover_interval_s_),
                                 [this] { return discovery_stop_; }))
        break;                                                  // stopped
      lk.unlock();
      try { discover_once(); } catch (...) {}                   // never escape the thread
      lk.lock();
    }
  });
}
```

Extend `~BridgeModule` (join discovery before listener join is fine; both before members die):

```cpp
BridgeModule::~BridgeModule() {
  {
    std::lock_guard<std::mutex> lk(discovery_mu_);
    discovery_stop_ = true;
  }
  discovery_cv_.notify_all();
  if (discovery_thread_.joinable()) discovery_thread_.join();
  if (srv_) srv_->stop();
  if (listen_thread_.joinable()) listen_thread_.join();
}
```

(Add `#include <chrono>` if absent.)

- [ ] **Step 4: Build + test.** `-R BridgeModule` → pass; full suite green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/bridge/http.h include/hades/module/bridge_module.h src/apps/bridge/bridge.cpp tests/test_bridge_module.cpp
git commit -m "feat: bridge discovery — get_json + periodic /card pull to PEER.<peer>.card"
```

---

## Task 5: Push self-announce — card `type=card` to peers on boot + skills change

**Files:** Modify `include/hades/module/bridge_module.h`, `src/apps/bridge/bridge.cpp`; Test append `tests/test_bridge_module.cpp`.

**Interfaces:**
- `run_share_push` gains a `type` param (default `"raw"`) so a card push carries `type=card`.
- `void announce_card_()` — build own card, submit a push job (executor or inline) to all peers with `key="card"`, `type="card"`.
- `on_attach` calls `announce_card_()` once (initial) and subscribes `SKILLS_ANNOUNCE` → `announce_card_()` (re-announce on change).

- [ ] **Step 1: Write the failing tests** — append to `tests/test_bridge_module.cpp`:

```cpp
TEST(BridgeModule, SelfAnnouncesCardToPeersOnAttach) {
  Blackboard bb;
  auto http = std::make_unique<FakeHttp>();
  FakeHttp* h = http.get();
  BridgeModule m(std::move(http), "s3cret");
  Block cfg; cfg.kv["name"] = "worker1";
  m.on_start(cfg, bb);
  m.set_description("worker one");
  m.set_peers({{"front", "http://10.0.0.1:9090"}, {"other", "http://10.0.0.2:9090"}});
  m.on_attach(bb);                                     // initial announce (inline: no executor)
  bb.pump();
  ASSERT_EQ(h->posts.size(), 2u);                      // one /share per peer
  EXPECT_EQ(std::get<0>(h->posts[0]), "http://10.0.0.1:9090/share");
  auto sent = parse_share(std::get<1>(h->posts[0]));
  ASSERT_TRUE(sent.ok);
  EXPECT_EQ(sent.type, "card");
  EXPECT_EQ(sent.key, "card");
  EXPECT_EQ(sent.value.value("name", ""), "worker1");
}

TEST(BridgeModule, ReAnnouncesCardWhenSkillsChange) {
  Blackboard bb;
  auto http = std::make_unique<FakeHttp>();
  FakeHttp* h = http.get();
  BridgeModule m(std::move(http), "s3cret");
  Block cfg; cfg.kv["name"] = "worker1";
  m.on_start(cfg, bb);
  m.set_peers({{"front", "http://10.0.0.1:9090"}});
  m.on_attach(bb);
  bb.pump();
  const std::size_t after_attach = h->posts.size();    // >=1 initial
  bb.post("SKILLS_ANNOUNCE", "Available skills (…):\n- deploy: ship it", "skills");
  bb.pump();
  ASSERT_GT(h->posts.size(), after_attach);            // re-announced
  auto sent = parse_share(std::get<1>(h->posts.back()));
  ASSERT_TRUE(sent.ok);
  EXPECT_EQ(sent.value["skills"][0].value("id", ""), "deploy");
}
```

- [ ] **Step 2: Run — expect FAIL** (no self-announce).
- [ ] **Step 3: Implement.** In `src/apps/bridge/bridge.cpp`, change `run_share_push` to carry a type:

```cpp
void run_share_push(Blackboard* bb, std::shared_ptr<BridgeHttp> http, const std::string& name,
                    const std::string& secret,
                    const std::map<std::string, std::string>& peers, const std::string& key,
                    const nlohmann::json& value, const std::string& type) {
  try {
    const std::string body = build_share(name, key, value, type).dump();
    // … unchanged loop …
```

Update its existing caller (the `share_out` handler in `on_attach`) to pass `kShareTypeRaw`:

```cpp
      auto job = [bb, http, name, secret, peers, key, value] {
        run_share_push(bb, http, name, secret, peers, key, value, kShareTypeRaw);
      };
```

Add `announce_card_` (submits the same job shape with the card + `kShareTypeCard`):

```cpp
void BridgeModule::announce_card_() {
  if (peers_.empty()) return;                          // nobody to tell
  Blackboard* bb = bb_;
  std::shared_ptr<BridgeHttp> http = http_;
  std::string name = name_, secret = secret_;
  std::map<std::string, std::string> peers = peers_;
  const nlohmann::json card = card_json();
  auto job = [bb, http, name, secret, peers, card] {
    run_share_push(bb, http, name, secret, peers, kCardKey, card, kShareTypeCard);
  };
  if (executor_) executor_->submit(job);
  else job();
}
```

Declare it in the header (`private: void announce_card_();`). In `on_attach`, after installing the `share_out` subscriptions, add:

```cpp
  // Self-announce: push our card to all peers now, and again whenever our skills change.
  bb.subscribe("SKILLS_ANNOUNCE", [this](const Entry&) { announce_card_(); });
  announce_card_();
```

- [ ] **Step 4: Build + test.** `-R BridgeModule` → pass (existing `ShareOutPushesToAllPeersOnChange` still green — it uses `share_out`, unaffected by the extra card posts because that test sets no peers-with-card? Verify: that test sets peers and posts STATUS; `announce_card_` also fires on attach → its FakeHttp will now see extra card posts BEFORE the STATUS ones. **Adjust that test** if it asserts `posts.size()==2` exactly: it does. Fix: assert the STATUS push distinctly, or clear `h->posts` after `on_attach`+`pump`). Apply this fix in Step 3: in `ShareOutPushesToAllPeersOnChange` and `UnlistedKeyIsNotPushed` and `SharePushJobSurvivesModuleTeardown` and `FailedPushPostsBridgeErrorAndDoesNotThrow`, add `h->posts.clear();` (or `calls=0`) immediately after the `on_attach`+initial `pump()` so the assertions measure only the intended push. Full suite green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/module/bridge_module.h src/apps/bridge/bridge.cpp tests/test_bridge_module.cpp
git commit -m "feat: bridge self-announce — push card (type=card) to peers on boot + skills change"
```

---

## Task 6: Inbound typed routing + trust tiers

**Files:** Modify `include/hades/module/bridge_module.h`, `src/apps/bridge/bridge.cpp`; Test append `tests/test_bridge_module.cpp`.

**Interfaces:**
- `void set_peer_trust(std::map<std::string, bool> t)` — peer name → trusted? (absent → trusted).
- `handle_share` routes by `m.type`:
  - `card` → `PEER.<from>.card = value`.
  - `fact` → `PEER.<from>.fact.<key> = {"from","trust","text"}` (trust from `peer_trusted_`, default true).
  - `raw`/other → `PEER.<from>.<key> = value` (unchanged legacy).

- [ ] **Step 1: Write the failing tests** — append to `tests/test_bridge_module.cpp`:

```cpp
TEST(BridgeModule, InboundCardShareStoredUnderCardKey) {
  Rig r;
  auto res = r.mod->handle_share(
      build_share("front", "card", {{"name", "front"}}, kShareTypeCard).dump(), "s3cret");
  ASSERT_TRUE(res.value("ok", false));
  auto e = r.bb.get("PEER.front.card");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->value.value("name", ""), "front");
}

TEST(BridgeModule, InboundFactFromTrustedPeerLabeledReports) {
  Rig r;                                                // "front" not in trust map -> trusted
  auto res = r.mod->handle_share(
      build_share("front", "weather", "sunny", kShareTypeFact).dump(), "s3cret");
  ASSERT_TRUE(res.value("ok", false));
  auto e = r.bb.get("PEER.front.fact.weather");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->value.value("trust", ""), "trusted");
  EXPECT_EQ(e->value.value("text", ""), "sunny");
  EXPECT_EQ(e->value.value("from", ""), "front");
}

TEST(BridgeModule, InboundFactFromUntrustedPeerLabeledUnverified) {
  Rig r;
  r.mod->set_peer_trust({{"front", false}});
  auto res = r.mod->handle_share(
      build_share("front", "weather", "sunny", kShareTypeFact).dump(), "s3cret");
  ASSERT_TRUE(res.value("ok", false));
  auto e = r.bb.get("PEER.front.fact.weather");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->value.value("trust", ""), "untrusted");
}

TEST(BridgeModule, InboundRawShareUnchangedLegacy) {
  Rig r;                                                // no type -> raw (Task 2 default)
  auto j = nlohmann::json{{"v", kBridgeProtocolV}, {"from", "front"}, {"key", "STATUS"},
                          {"value", "wet"}};
  auto res = r.mod->handle_share(j.dump(), "s3cret");
  ASSERT_TRUE(res.value("ok", false));
  auto e = r.bb.get("PEER.front.STATUS");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->value.get<std::string>(), "wet");        // legacy PEER.<from>.<key>
}
```

- [ ] **Step 2: Run — expect FAIL** (routing not typed; no `set_peer_trust`).
- [ ] **Step 3: Implement.** Header: `void set_peer_trust(std::map<std::string, bool> t) { peer_trusted_ = std::move(t); }` and member `std::map<std::string, bool> peer_trusted_;`. Add `#include "hades/bridge/registry.h"` to the header? No — keep it in the .cpp (the constants are used in the .cpp). Rewrite `handle_share`'s post step:

```cpp
nlohmann::json BridgeModule::handle_share(const std::string& body,
                                          const std::string& presented_secret) {
  if (body.size() > kMaxShareBytes) return {{"ok", false}, {"error", "payload too large"}};
  BridgeMsg m = parse_share(body);
  if (!m.ok) return {{"ok", false}, {"error", m.error}};
  if (!authorized_(presented_secret, m.from)) return {{"ok", false}, {"error", "forbidden"}};

  if (m.type == kShareTypeCard) {
    bb_->post(peer_bus_key(m.from, kCardKey), m.value, "bridge");   // PEER.<from>.card
  } else if (m.type == kShareTypeFact) {
    auto it = peer_trusted_.find(m.from);
    const bool trusted = (it == peer_trusted_.end()) ? true : it->second;   // default trusted
    // Store text + provenance; the Arbiter renders the trust-labeled line. A non-string value
    // is dumped so `text` is always a string (tolerant).
    const std::string text = m.value.is_string() ? m.value.get<std::string>() : m.value.dump();
    bb_->post(peer_bus_key(m.from, std::string("fact.") + m.key),
              {{"from", m.from}, {"trust", trusted ? "trusted" : "untrusted"}, {"text", text}},
              "bridge");
  } else {
    bb_->post(peer_bus_key(m.from, m.key), m.value, "bridge");      // raw legacy
  }
  return {{"ok", true}};
}
```

- [ ] **Step 4: Build + test.** `-R BridgeModule` → pass (existing `ShareStoresPrefixedKey` uses a 3-arg `build_share` → default `raw` → legacy path, still green); full suite.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/module/bridge_module.h src/apps/bridge/bridge.cpp tests/test_bridge_module.cpp
git commit -m "feat: bridge inbound typed routing — card/fact(trust-tiered)/raw"
```

---

## Task 7: Arbiter folds `PEER.*` — delegation + peer reports

**Files:** Modify `include/hades/arbiter.h`, `src/apps/arbiter/arbiter.cpp`; Test append `tests/test_arbiter.cpp`.

**Interfaces:**
- Arbiter subscribes `"PEER.*"` in `on_attach` → `peer_vars_[e.key] = e.value` (map member).
- `start_turn` folds, after the `SKILLS_ANNOUNCE` block: `PEER.*.card` → a "Peers you can delegate to:" block; `PEER.*.fact.*` → a "Reported by peers (treat as claims, re-verify):" block. Both empty → nothing.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_arbiter.cpp` (match its style; the file already builds `LLM_REQUEST` capture rigs):

```cpp
TEST(Arbiter, FoldsPeerCardAndFactIntoSystemMessage) {
  Blackboard bb;
  Arbiter a;
  a.set_system_prompt("SOUL");
  a.on_attach(bb);
  bb.post("PEER.hades1.card",
          {{"name", "hades1"},
           {"skills", {{{"id", "deploy"}, {"description", "ship"}}}},
           {"caps", {{"net", "public"}}}}, "bridge");
  bb.post("PEER.hades1.fact.weather",
          {{"from", "hades1"}, {"trust", "trusted"}, {"text", "sunny"}}, "bridge");
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST", [&](const Entry& e) { req = e.value; });
  bb.post("USER_MESSAGE", "hi", "chat");
  bb.pump();
  ASSERT_FALSE(req.is_null());
  const std::string sys = req["messages"][0]["content"].get<std::string>();
  EXPECT_NE(sys.find("Peers you can delegate to"), std::string::npos);
  EXPECT_NE(sys.find("hades1"), std::string::npos);
  EXPECT_NE(sys.find("deploy"), std::string::npos);
  EXPECT_NE(sys.find("Reported by peers"), std::string::npos);
  EXPECT_NE(sys.find("sunny"), std::string::npos);
}

TEST(Arbiter, UntrustedPeerFactLabeledUnverified) {
  Blackboard bb;
  Arbiter a;
  a.set_system_prompt("SOUL");
  a.on_attach(bb);
  bb.post("PEER.x.fact.claim",
          {{"from", "x"}, {"trust", "untrusted"}, {"text", "the sky is green"}}, "bridge");
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST", [&](const Entry& e) { req = e.value; });
  bb.post("USER_MESSAGE", "hi", "chat");
  bb.pump();
  const std::string sys = req["messages"][0]["content"].get<std::string>();
  EXPECT_NE(sys.find("unverified"), std::string::npos);
  EXPECT_NE(sys.find("the sky is green"), std::string::npos);
}

TEST(Arbiter, NoPeerVarsInjectsNoPeerBlocks) {
  Blackboard bb;
  Arbiter a;
  a.set_system_prompt("SOUL");
  a.on_attach(bb);
  nlohmann::json req;
  bb.subscribe("LLM_REQUEST", [&](const Entry& e) { req = e.value; });
  bb.post("USER_MESSAGE", "hi", "chat");
  bb.pump();
  const std::string sys = req["messages"][0]["content"].get<std::string>();
  EXPECT_EQ(sys.find("Peers you can delegate to"), std::string::npos);
  EXPECT_EQ(sys.find("Reported by peers"), std::string::npos);
  EXPECT_EQ(sys, "SOUL");
}
```

- [ ] **Step 2: Run — expect FAIL** (no peer fold).
- [ ] **Step 3: Implement.** `include/hades/arbiter.h` — add a member (near `pending_`):

```cpp
  std::map<std::string, nlohmann::json> peer_vars_;   // PEER.* latest values (bridge discovery/share)
```

(Add `#include <map>` if absent.) In `src/apps/arbiter/arbiter.cpp` `on_attach`, add a subscription (near the other `bb.subscribe` calls):

```cpp
  // Bridge peer state: PEER.<peer>.card (capabilities) and PEER.<peer>.fact.<k> (reports). Kept
  // in a local latest-value map and folded into the leading system message at turn start (the
  // SKILLS_ANNOUNCE pattern). Harmless when no bridge exists (nothing posts PEER.*).
  bb.subscribe("PEER.*", [this](const Entry& e) { peer_vars_[e.key] = e.value; });
```

In `start_turn`, right AFTER the `SKILLS_ANNOUNCE` fold block (after line ~199) and BEFORE `if (!sys.empty())`:

```cpp
  // Peer capability + report folds (bridge protocol). Two blocks from the PEER.* map: cards ->
  // delegation targets; facts -> peer reports (trust-labeled, re-verify). Empty -> no block.
  {
    std::string deleg, reports;
    for (const auto& [key, val] : peer_vars_) {
      if (key.size() > 5 && key.compare(key.size() - 5, 5, ".card") == 0 && val.is_object()) {
        std::string line = "\n- " + val.value("name", "?");
        if (val.contains("skills") && val["skills"].is_array() && !val["skills"].empty()) {
          line += " skills:[";
          bool first = true;
          for (const auto& sk : val["skills"]) {
            line += (first ? "" : ",") + sk.value("id", "");
            first = false;
          }
          line += "]";
        }
        if (val.contains("caps") && val["caps"].is_object())
          line += " caps:" + val["caps"].dump();
        deleg += line;
      } else if (key.find(".fact.") != std::string::npos && val.is_object()) {
        const std::string who = val.value("from", "?");
        const std::string lbl = val.value("trust", "trusted") == "untrusted"
                                    ? "unverified claim from " + who
                                    : who + " reports";
        reports += "\n- " + lbl + ": " + val.value("text", "");
      }
    }
    if (!deleg.empty()) {
      if (!sys.empty()) sys += "\n\n";
      sys += "Peers you can delegate to (use ask_agent by advertised capability):" + deleg;
    }
    if (!reports.empty()) {
      if (!sys.empty()) sys += "\n\n";
      sys += "Reported by peers (treat as claims, re-verify before acting):" + reports;
    }
  }
```

- [ ] **Step 4: Build + test.** `-R Arbiter` → pass (new + existing); full suite green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/arbiter.h src/apps/arbiter/arbiter.cpp tests/test_arbiter.cpp
git commit -m "feat: Arbiter folds PEER.* cards (delegation) + facts (reports) into system prompt"
```

---

## Task 8: Wiring — feed description/tools/caps/trust, discover interval, start discovery

**Files:** Modify `app/agent_wiring.cpp`, `app/hades_main.cpp`; Test append `tests/test_bridge_wiring.cpp`.

**Interfaces:**
- `wire_agent` bridge branch: `set_description` (from `bridge_cfg["description"]`, default = `bridge_name`), `set_tools` (json array of `{name}` from the resolved tool list), `set_caps` (`caps_summary` of the `capability_policy` objective block, or `{}`), `set_peer_trust` (from each `Peer` block's `trust` key), `set_discover_interval_s` (from `bridge_cfg["discover_interval_s"]`).
- `hades_main`: after `start_listening`, call `agent.bridge->start_discovery()`.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_bridge_wiring.cpp` (follow its manifest-build pattern; a bridge-rostered manifest with a `Peer` + `capability_policy`):

```cpp
TEST(BridgeWiring, CardServesInjectedDescriptionToolsCapsAndPeerTrustParsed) {
  // Manifest: bridge module + a peer with trust=untrusted + a capability_policy scope + a tool.
  const std::string mtext =
      "Session\n{\n  model = m\n}\n"
      "Module = tool_runner\nModule = arbiter\nModule = bridge\n"
      "Tool = shell { native = /bin/true }\n"
      "Bridge\n{\n  name = worker1\n  port = 0\n  description = a helper\n"
      "  discover_interval_s = 0\n}\n"
      "Peer = mate { url = http://127.0.0.1:1  trust = untrusted }\n"
      "Objective = capability_policy\n{\n  fs_read_allow = ./workspace\n"
      "  block_private_net = true\n}\n";
  setenv("HADES_BRIDGE_SECRET", "s3cret", 1);
  Blackboard bb;
  Manifest m = parse_manifest(mtext);
  Agent agent = build_agent(bb, m);
  ASSERT_NE(agent.bridge, nullptr);
  auto card = agent.bridge->card_json();
  EXPECT_EQ(card.value("description", ""), "a helper");
  EXPECT_EQ(card["caps"].value("fs_read", ""), "scoped");
  EXPECT_EQ(card["caps"].value("net", ""), "private-blocked");
  ASSERT_TRUE(card["tools"].is_array());
  EXPECT_EQ(card["tools"][0].value("name", ""), "shell");
  // trust parsed: an untrusted peer's fact is labeled untrusted
  auto res = agent.bridge->handle_share(
      build_share("mate", "w", "x", kShareTypeFact).dump(), "s3cret");
  ASSERT_TRUE(res.value("ok", false));
  auto e = bb.get("PEER.mate.fact.w");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->value.value("trust", ""), "untrusted");
}
```

(Include `hades/bridge/registry.h` and `hades/bridge/protocol.h` in the test if not already.)

- [ ] **Step 2: Run — expect FAIL** (setters not fed).
- [ ] **Step 3: Implement.** In `app/agent_wiring.cpp`, inside the `if (a.bridge) { … }` block (after `set_peers`, before `on_attach`):

```cpp
    a.bridge->set_description(bridge_cfg.kv.count("description") ? bridge_cfg.kv.at("description")
                                                                : bridge_name);
    // Card tools = the resolved tool NAMES (same roster ask_agent sees). Names only — no argv.
    nlohmann::json card_tools = nlohmann::json::array();
    for (const auto& t : tools_resolved) card_tools.push_back({{"name", t.name}});
    a.bridge->set_tools(card_tools);
    // Card caps = a SUMMARY of the capability_policy block (categories, never literal paths).
    for (const auto& ob : objectives)
      if (ob.name == "capability_policy") { a.bridge->set_caps(caps_summary(ob)); break; }
    // Per-peer trust (default trusted); the seam for future untrusted dynamic joiners.
    std::map<std::string, bool> peer_trust;
    for (const auto& p : peer_blocks)
      peer_trust[p.name] = !(p.kv.count("trust") && p.kv.at("trust") == "untrusted");
    a.bridge->set_peer_trust(peer_trust);
    if (bridge_cfg.kv.count("discover_interval_s")) {
      double di = 0.0;
      if (set_pos_double_on_string(bridge_cfg.kv.at("discover_interval_s"), di))
        a.bridge->set_discover_interval_s(di);
      else if (bridge_cfg.kv.at("discover_interval_s") == "0")
        a.bridge->set_discover_interval_s(0.0);   // explicit off (positive-only helper rejects 0)
    }
```

Add `#include "hades/bridge/registry.h"` to `agent_wiring.cpp` includes (for `caps_summary`). `tools_resolved` here is the `std::vector<Block>` already built earlier in `wire_agent`; `p.name`/`p.kv` are the `Peer` block fields (`peer_blocks` is already a param). In `app/hades_main.cpp`, after the `start_listening()` success branch (~line 171):

```cpp
      agent.bridge->start_discovery();   // boot pull + periodic /card refresh (after wiring)
```

- [ ] **Step 4: Build + test.** `-R BridgeWiring` → pass; **full suite** green.
- [ ] **Step 5: Commit.**

```bash
git add app/agent_wiring.cpp app/hades_main.cpp tests/test_bridge_wiring.cpp
git commit -m "feat: wire bridge card — description/tools/caps/peer-trust + start discovery"
```

- [ ] **Step 6: TSan lane.** Build+run the TSan config (the project's TSan build dir) → the bridge's listener + discovery threads must be clean. Report results.

---

## Task 9: Ship — dev.hades, manifest-reference, soul.md, CLAUDE.md

**Files:** Modify `manifests/dev.hades` (commented example only), `docs/manifest-reference.md`, `prompts/soul.md`, `CLAUDE.md`.

**Note:** `manifests/dev.hades` carries the user's uncommitted live edits. Touch ONLY the **commented** Bridge/Peer example; commit split-clean (from the clean committed base, not his working-tree edits). Do NOT stage `memory/facts.md` / `skills/`.

- [ ] **Step 1: dev.hades commented Bridge/Peer example** — extend the existing commented Bridge block with the new keys and a trust example:

```
# Bridge
# {
#   name          = hades1
#   host          = 127.0.0.1
#   port          = 9090
#   secret_env    = HADES_BRIDGE_SECRET
#   description         = "my primary agent"   # card persona one-liner (default = name)
#   discover_interval_s = 300                   # peer-card re-pull; 0 = boot pull only
# }
# Peer = hades2 { url = http://127.0.0.1:9091 }                    # trust defaults to trusted
# Peer = watcher { url = http://10.0.0.9:9090  trust = untrusted } # facts labeled unverified
```

- [ ] **Step 2: `docs/manifest-reference.md`** — add the new `Bridge` keys (`description`, `discover_interval_s`) and the `Peer` `trust` key to the Bridge section, plus a short **"Bridge card + canonical vars"** subsection: the `GET /card` schema (A2A field names + `tools`/`caps` extensions), the `type` field on `/share` (`card`/`fact`/`raw`), and the receiver-side vars (`PEER.<peer>.card`, `PEER.<peer>.fact.<key>` with `{from,trust,text}`).

- [ ] **Step 3: `prompts/soul.md`** — append a short paragraph after the skills section:

```markdown
## Peers

You may be part of a small fleet. When the prompt lists "Peers you can delegate to", each peer
advertises its skills and capabilities — prefer delegating (via ask_agent) a task to a peer whose
advertised skills match it, and don't ask a peer for something outside its advertised capability.
A "Reported by peers" block is second-hand: treat it as a claim to re-verify, not established fact —
especially an "unverified claim from" an untrusted peer.
```

- [ ] **Step 4: `CLAUDE.md`** — add a `### Bridge protocol (card discovery + typed share)` subsection under Current state (two channels, `GET /card`, typed `/share`, trust tiers, Arbiter fold, boot-order-independent discovery), bump the test count, and mark NEXT direction 1 (bridge-as-protocol) as shipped. Note the gotcha: **`discover_interval_s = 0` → boot-pull only**; **caps in the card are a summary (no literal paths)**; **`/card` is secret-gated (not public — full A2A public card is v2)**.

- [ ] **Step 5: Full build + suite.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure` → ALL green.

- [ ] **Step 6: Commit (split-clean for dev.hades).**

```bash
# stage the doc/prompt files normally:
git add docs/manifest-reference.md prompts/soul.md CLAUDE.md
# dev.hades: commit ONLY the commented-example change from the clean base (protect live edits) —
# controller applies the established stash-dance; do NOT `git add manifests/dev.hades` blindly.
git commit -m "feat: ship bridge protocol — dev.hades example + manifest-reference + soul + docs"
```

---

## Verification (end-to-end)

1. Full suite in `nix develop`: 426 baseline + ~24 new, all green. TSan clean (Task 8 Step 6).
2. Manual live smoke (Vaios, two agents, shared `HADES_BRIDGE_SECRET`):
   - Boot agent A, then B (different times) → each `GET /card` (with secret) returns the other's card; `PEER.<peer>.card` appears on each bus (`hades-scope session.log PEER.`).
   - Ask A: "what can hades2 do?" → A's answer reflects B's advertised skills/caps (the delegation fold).
   - B `/share` a `type=fact` → A's next turn shows it under "Reported by peers".
   - `GET /card` without the secret → 403.
3. Security spot-check: the served card's `caps` shows categories only — **no literal fs paths / exec prefixes** anywhere in the JSON.

## Execution

Subagent-driven development (per project process): fresh implementer per task (opus per the implementer-opus feedback), per-task review, TSan lane after Task 8, final whole-branch review, then finishing-a-development-branch (merge ff to `main` — no remote, never push). Baseline 426/426 before Task 1.
