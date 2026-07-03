# Bridge Multi-Agent Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Agents in separate processes/machines cooperate: `ask_agent` tool delegates a question to a peer (which answers through its OWN Arbiter/objectives/gates), and pShare-style key forwarding shares selected Blackboard keys between agents.

**Architecture:** Approach B from the spec (`docs/superpowers/specs/2026-07-03-bridge-multi-agent-design.md`, committed `b286a25`): inbound = `BridgeModule` (Telegram-shaped front-end module with its own httplib listener; `/ask` drives a full turn through the shared TurnGate, `/share` stores `PEER.<name>.<KEY>`), outbound = `hades-ask-agent` native tool (rides the existing tool/capability/Eventlog infra) + share push on key change via the Executor. A built-in `PeerLoopGuard` objective (auto-registered whenever the bridge module is rostered) hard-vetoes `ask_agent` inside a peer-driven turn, killing the A↔B mutual-wait deadlock.

**Tech Stack:** C++20, CMake+Ninja in `nix develop`, nlohmann_json, cpp-httplib (server + test client), libcpr (outbound), GoogleTest, std::thread.

## Global Constraints

- **Every build/test command runs inside `nix develop`**: `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure`. Baseline: **306/306 tests green** before Task 1.
- Branch `feat/bridge` (already created; spec committed `b286a25`). Commit style `<type>: <desc>` — **NO attribution footer, NO Co-Authored-By**.
- Wire protocol (exact values): auth header **`X-Hades-Bridge`**; JSON fields `v` (int, `1`), `from`, `hops`, `message` / `key` / `value`; unknown fields ignored; **share body cap 64 KB**; endpoints `POST /ask`, `POST /share`, `GET /health`.
- Config defaults (exact): `host = 127.0.0.1`, `port = 9090`, `secret_env = HADES_BRIDGE_SECRET`, `max_hops = 1`, `ask_timeout_s = 180` (constant `kDefaultAskTimeoutS` in `include/hades/timeouts.h`).
- Inbound share bus key is **exactly** `PEER.<from>.<key>`; the peer USER_MESSAGE prefix is **exactly** `(from peer agent "<from>") ` (note the trailing space).
- Peer/agent names validate as `[A-Za-z0-9_-]{1,64}` (`valid_peer_name`, header-inline in `include/hades/bridge/protocol.h`).
- `TURN_ORIGIN` values: front-ends post the string `"human"`; the bridge posts `"peer:<name>"`. Absent/malformed key ⇒ treated as non-peer (guard allows).
- Auth failure (bad secret) and unknown `from` ⇒ **HTTP 403** with body `{"ok":false,"error":"forbidden"}`; never reveal which check failed.
- Confirm-band actions in a peer-driven turn are **auto-denied**; the `/ask` reply appends `\n[note: a confirm-gated action was auto-denied — peer requests cannot grant human confirmation]`.
- Secrets: bridge secret via env var ONLY (never manifest/argv), redacted in the Eventlog; the `ask_agent` tool receives the env-var NAME on argv and getenvs it itself.
- Pump-thread handlers must never throw (try/catch or typed `find`+`is_*` guards).
- Manifest blocks are one-kv-per-line except the legal single-kv inline (`Peer = front { url = http://x:9090 }` is single-kv inline — legal).
- File headers: `// path — one-line purpose` + short explanation block (house style).
- Do NOT stage/commit `memory/facts.md` or `skills/` working-tree churn (agent runtime artifacts).
- Teardown order in `struct Agent` after this feature: `…plain modules…, executor, bridge, telegram` (telegram destroyed first, bridge second, executor third). Do not reorder.

---

## File Structure

```
include/hades/bridge/protocol.h        T1  pure parse/build + valid_peer_name + peer_bus_key
src/bridge/protocol.cpp                T1
tests/test_bridge_protocol.cpp         T1
include/hades/module/bridge_module.h   T2 (extended T3, T4)
src/module/bridge_module.cpp           T2 (extended T3, T4)
tests/test_bridge_module.cpp           T2 (extended T3, T4)
include/hades/bridge/http.h            T3  BridgeHttp seam + CprBridgeHttp
src/bridge/cpr_bridge_http.cpp         T3
include/hades/objective/peer_loop_guard.h  T5
src/objective/peer_loop_guard.cpp      T5
src/module/{chat,http_server,telegram}_module.cpp  T5  TURN_ORIGIN posts (modify)
src/objective/capability_policy.{h,cpp}    T5  PeerAsk capability (modify)
tools/ask_agent_main.cpp               T6
tests/test_ask_agent_tool.cpp          T6
include/hades/tool/registry.h + src/tool/registry.cpp + src/module/tool_runner.{h,cpp}  T6  per-tool timeout (modify)
include/hades/timeouts.h               T6  kDefaultAskTimeoutS (modify)
app/agent_wiring.{h,cpp}, app/hades_main.cpp  T7  wiring + e2e (modify)
tests/test_bridge_wiring.cpp           T7
manifests/dev.hades, prompts/soul.md, CLAUDE.md  T8  ship (modify)
CMakeLists.txt                         T1,T2,T5,T6,T7 (add sources/targets)
```

---

## Task 1: Bridge wire-protocol library (pure)

**Files:**
- Create: `include/hades/bridge/protocol.h`, `src/bridge/protocol.cpp`
- Test: `tests/test_bridge_protocol.cpp`
- Modify: `CMakeLists.txt`

**Interfaces — Produces (all `namespace hades`):**
- `inline constexpr int kBridgeProtocolV = 1;`
- `inline constexpr std::size_t kMaxShareBytes = 64 * 1024;`
- `inline bool valid_peer_name(const std::string&)` — header-only, `[A-Za-z0-9_-]{1,64}`
- `struct BridgeMsg { bool ok; std::string error; std::string from; long long hops; std::string message; std::string key; nlohmann::json value; };`
- `nlohmann::json build_ask(const std::string& from, long long hops, const std::string& message)`
- `nlohmann::json build_share(const std::string& from, const std::string& key, const nlohmann::json& value)`
- `BridgeMsg parse_ask(const std::string& body)` — tolerant, never throws
- `BridgeMsg parse_share(const std::string& body)` — tolerant, never throws
- `std::string peer_bus_key(const std::string& from, const std::string& key)` — `"PEER."+from+"."+key`

- [ ] **Step 1: Write the failing tests** `tests/test_bridge_protocol.cpp`:

```cpp
// tests/test_bridge_protocol.cpp — pure bridge wire protocol: build/parse/validate
#include <gtest/gtest.h>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/bridge/protocol.h"
using namespace hades;

TEST(BridgeProtocol, ValidPeerNameGate) {
  EXPECT_TRUE(valid_peer_name("worker-1_A"));
  EXPECT_TRUE(valid_peer_name("A"));
  EXPECT_FALSE(valid_peer_name(""));
  EXPECT_FALSE(valid_peer_name("a b"));
  EXPECT_FALSE(valid_peer_name("a/b"));
  EXPECT_FALSE(valid_peer_name("peer:x"));
  EXPECT_FALSE(valid_peer_name(std::string(65, 'a')));
}

TEST(BridgeProtocol, BuildAskRoundTripsThroughParse) {
  auto j = build_ask("front", 0, "what is the disk usage?");
  auto m = parse_ask(j.dump());
  ASSERT_TRUE(m.ok) << m.error;
  EXPECT_EQ(m.from, "front");
  EXPECT_EQ(m.hops, 0);
  EXPECT_EQ(m.message, "what is the disk usage?");
}

TEST(BridgeProtocol, BuildShareRoundTripsThroughParse) {
  auto j = build_share("front", "STATUS", nlohmann::json{{"cpu", 0.5}});
  auto m = parse_share(j.dump());
  ASSERT_TRUE(m.ok) << m.error;
  EXPECT_EQ(m.from, "front");
  EXPECT_EQ(m.key, "STATUS");
  EXPECT_EQ(m.value["cpu"], 0.5);
}

TEST(BridgeProtocol, ParseAskRejectsBadInput) {
  EXPECT_FALSE(parse_ask("not json").ok);
  EXPECT_FALSE(parse_ask("42").ok);                                        // non-object
  EXPECT_FALSE(parse_ask(R"({"v":2,"from":"a","hops":0,"message":"m"})").ok);   // version
  EXPECT_FALSE(parse_ask(R"({"v":"1","from":"a","hops":0,"message":"m"})").ok); // v not int
  EXPECT_FALSE(parse_ask(R"({"v":1,"hops":0,"message":"m"})").ok);         // no from
  EXPECT_FALSE(parse_ask(R"({"v":1,"from":"../x","hops":0,"message":"m"})").ok); // bad name
  EXPECT_FALSE(parse_ask(R"({"v":1,"from":"a","hops":-1,"message":"m"})").ok);  // neg hops
  EXPECT_FALSE(parse_ask(R"({"v":1,"from":"a","hops":"0","message":"m"})").ok); // hops not int
  EXPECT_FALSE(parse_ask(R"({"v":1,"from":"a","hops":0,"message":""})").ok);    // empty msg
  EXPECT_FALSE(parse_ask(R"({"v":1,"from":"a","hops":0,"message":7})").ok);     // non-string
}

TEST(BridgeProtocol, ParseAskIgnoresUnknownFields) {
  auto m = parse_ask(R"({"v":1,"from":"a","hops":0,"message":"m","future":"stuff"})");
  EXPECT_TRUE(m.ok) << m.error;   // forward compatibility: unknown fields ignored
}

TEST(BridgeProtocol, ParseShareRejectsBadInput) {
  EXPECT_FALSE(parse_share("{}").ok);
  EXPECT_FALSE(parse_share(R"({"v":1,"from":"a","value":1})").ok);          // no key
  EXPECT_FALSE(parse_share(R"({"v":1,"from":"a","key":"","value":1})").ok); // empty key
  EXPECT_FALSE(parse_share(R"({"v":1,"from":"a","key":"K K","value":1})").ok); // ws in key
  EXPECT_FALSE(parse_share(R"({"v":1,"from":"a","key":"K"})").ok);          // no value
  EXPECT_FALSE(parse_share(R"({"v":1,"from":"a","key":7,"value":1})").ok);  // key not string
}

TEST(BridgeProtocol, PeerBusKeyFormat) {
  EXPECT_EQ(peer_bus_key("front", "STATUS"), "PEER.front.STATUS");
}
```

- [ ] **Step 2: Add to CMake and run — expect FAIL.** In `CMakeLists.txt`, after the `src/telegram/cpr_telegram_api.cpp` line (~line 38), add:

```cmake
target_sources(hades_core PRIVATE src/bridge/protocol.cpp)
```

and after the `tests/test_telegram_parse.cpp` line (~line 60), add:

```cmake
target_sources(hades_tests PRIVATE tests/test_bridge_protocol.cpp)
```

Run: `nix develop --command cmake --build build` → compile error (no such header).

- [ ] **Step 3: Implement.** `include/hades/bridge/protocol.h`:

```cpp
// include/hades/bridge/protocol.h — pure bridge wire-protocol helpers (build/parse/validate)
//
// The agent↔agent bridge protocol (spec 2026-07-03): versioned JSON over HTTP with an
// X-Hades-Bridge shared-secret header. build_* produce the exact request bodies; parse_*
// are tolerant, never throw, and IGNORE unknown fields (additive forward compatibility).
// valid_peer_name is header-only so the standalone hades-ask-agent tool binary shares the
// exact same gate. peer_bus_key renders the fixed pShare-style rename on arrival:
// an inbound share lands as PEER.<from>.<key>, so a peer can never inject a local bus key.
#pragma once
#include <cstddef>
#include <string>
#include <nlohmann/json.hpp>
namespace hades {

inline constexpr int kBridgeProtocolV = 1;
inline constexpr std::size_t kMaxShareBytes = 64 * 1024;   // /share request-body cap

// Strict peer/agent-name gate: 1..64 chars of [A-Za-z0-9_-] (same charset as skill names —
// explicit ASCII ranges, locale-independent). Anything else (path chars, ':', whitespace,
// empty) is rejected: the name is embedded in bus keys and the "peer:<name>" TURN_ORIGIN.
inline bool valid_peer_name(const std::string& n) {
  if (n.empty() || n.size() > 64) return false;
  for (char c : n) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

// One parsed inbound request. ok=false + error covers BOTH transport-level garbage and
// field-level validation failures; callers branch only on ok.
struct BridgeMsg {
  bool ok = false;
  std::string error;      // set when !ok
  std::string from;       // validated peer name
  long long hops = 0;     // /ask only; >= 0
  std::string message;    // /ask only; non-empty
  std::string key;        // /share only; non-empty, no whitespace/control chars
  nlohmann::json value;   // /share only
};

nlohmann::json build_ask(const std::string& from, long long hops, const std::string& message);
nlohmann::json build_share(const std::string& from, const std::string& key,
                           const nlohmann::json& value);
BridgeMsg parse_ask(const std::string& body);
BridgeMsg parse_share(const std::string& body);

// "PEER.<from>.<key>" — the fixed v1 rename-on-arrival for inbound shares.
std::string peer_bus_key(const std::string& from, const std::string& key);

}  // namespace hades
```

`src/bridge/protocol.cpp`:

```cpp
// src/bridge/protocol.cpp — bridge protocol build/parse (tolerant, never throws)
#include "hades/bridge/protocol.h"
namespace hades {
namespace {

// Common envelope checks: valid JSON object, v == kBridgeProtocolV, from is a valid peer
// name. Returns true and fills m.from on success; sets m.error otherwise.
bool parse_envelope(const nlohmann::json& j, BridgeMsg& m) {
  if (!j.is_object()) { m.error = "not a JSON object"; return false; }
  auto v = j.find("v");
  if (v == j.end() || !v->is_number_integer() ||
      v->get<long long>() != kBridgeProtocolV) {
    m.error = "unsupported protocol version";
    return false;
  }
  auto from = j.find("from");
  if (from == j.end() || !from->is_string() || !valid_peer_name(from->get<std::string>())) {
    m.error = "missing/invalid from";
    return false;
  }
  m.from = from->get<std::string>();
  return true;
}

bool valid_share_key(const std::string& k) {
  if (k.empty() || k.size() > 128) return false;
  for (char c : k)
    if (static_cast<unsigned char>(c) <= ' ' || static_cast<unsigned char>(c) >= 127)
      return false;   // no whitespace/control/non-ASCII in a bus key
  return true;
}

}  // namespace

nlohmann::json build_ask(const std::string& from, long long hops, const std::string& message) {
  return {{"v", kBridgeProtocolV}, {"from", from}, {"hops", hops}, {"message", message}};
}

nlohmann::json build_share(const std::string& from, const std::string& key,
                           const nlohmann::json& value) {
  return {{"v", kBridgeProtocolV}, {"from", from}, {"key", key}, {"value", value}};
}

BridgeMsg parse_ask(const std::string& body) {
  BridgeMsg m;
  auto j = nlohmann::json::parse(body, nullptr, false);
  if (j.is_discarded()) { m.error = "malformed JSON"; return m; }
  if (!parse_envelope(j, m)) return m;
  auto hops = j.find("hops");
  if (hops == j.end() || !hops->is_number_integer() || hops->get<long long>() < 0) {
    m.error = "missing/invalid hops";
    return m;
  }
  m.hops = hops->get<long long>();
  auto msg = j.find("message");
  if (msg == j.end() || !msg->is_string() || msg->get<std::string>().empty()) {
    m.error = "missing/invalid message";
    return m;
  }
  m.message = msg->get<std::string>();
  m.ok = true;
  return m;
}

BridgeMsg parse_share(const std::string& body) {
  BridgeMsg m;
  auto j = nlohmann::json::parse(body, nullptr, false);
  if (j.is_discarded()) { m.error = "malformed JSON"; return m; }
  if (!parse_envelope(j, m)) return m;
  auto key = j.find("key");
  if (key == j.end() || !key->is_string() || !valid_share_key(key->get<std::string>())) {
    m.error = "missing/invalid key";
    return m;
  }
  m.key = key->get<std::string>();
  auto val = j.find("value");
  if (val == j.end()) { m.error = "missing value"; return m; }
  m.value = *val;
  m.ok = true;
  return m;
}

std::string peer_bus_key(const std::string& from, const std::string& key) {
  return "PEER." + from + "." + key;
}
}  // namespace hades
```

- [ ] **Step 4: Build + test.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R BridgeProtocol` → all pass. Then the full suite.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/bridge/protocol.h src/bridge/protocol.cpp tests/test_bridge_protocol.cpp CMakeLists.txt
git commit -m "feat: bridge wire-protocol library (versioned build/parse, peer-name gate, PEER key rename)"
```

---

## Task 2: BridgeModule inbound core (socket-free)

**Files:**
- Create: `include/hades/module/bridge_module.h`, `src/module/bridge_module.cpp`
- Test: `tests/test_bridge_module.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: T1 protocol; `TurnGate` (`include/hades/turn_gate.h`); `Blackboard::post/run_until/pump`; `MalConfig` (`hades/launcher.h`); `kDefaultTurnIdleTimeoutS` (`hades/timeouts.h`); `set_pos_double_on_string` (`hades/config.h`).
- Produces: `class BridgeModule : public Module` — `type()=="bridge"`; `explicit BridgeModule(std::string secret_for_test = "")` (empty ⇒ on_start resolves the env var); `on_start(cfg, bb)` (reads `name`/`host`/`port`/`secret_env`/`max_hops`/`ask_timeout_s`/`share_out`; MalConfig on missing/invalid `name` or unset/empty secret env); `on_attach` (ASSISTANT_MESSAGE + CONFIRM_REQUEST captures, both `my_turn_`-guarded; confirm ⇒ **auto-deny**); `set_peers(std::map<std::string,std::string>)`; `set_turn_gate(TurnGate*)`; `set_turn_timeout_s(double)`; **socket-free handlers** `nlohmann::json handle_ask(const std::string& body, const std::string& presented_secret)` and `nlohmann::json handle_share(const std::string& body, const std::string& presented_secret)` and `nlohmann::json health_json() const`; accessors `const std::string& name() const`, `double ask_timeout_s() const`, `const std::vector<std::string>& share_out_keys() const`.

- [ ] **Step 1: Write the failing tests** `tests/test_bridge_module.cpp`:

```cpp
// tests/test_bridge_module.cpp — BridgeModule inbound: auth, allowlist, ask turns, shares
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <string>
#include "hades/blackboard.h"
#include "hades/bridge/protocol.h"
#include "hades/launcher.h"          // MalConfig
#include "hades/module/bridge_module.h"
using namespace hades;

namespace {
// Module named "worker1", secret "s3cret", peer allowlist {front}. `echo=true` installs a
// plain echo agent; tests that script their OWN bus behavior pass false.
struct Rig {
  Blackboard bb;
  std::unique_ptr<BridgeModule> mod;
  explicit Rig(bool echo = true) {
    mod = std::make_unique<BridgeModule>("s3cret");
    Block cfg;
    cfg.kv["name"] = "worker1";
    mod->on_start(cfg, bb);
    mod->set_peers({{"front", "http://127.0.0.1:1"}});
    mod->on_attach(bb);
    if (echo)
      bb.subscribe("USER_MESSAGE", [this](const Entry& e) {
        bb.post("ASSISTANT_MESSAGE", "echo:" + e.value.get<std::string>(), "t");
      });
  }
};
std::string ask_body(const std::string& from, long long hops, const std::string& msg) {
  return build_ask(from, hops, msg).dump();
}
}  // namespace

TEST(BridgeModule, MissingNameThrowsMalConfig) {
  Blackboard bb;
  BridgeModule m("s");
  EXPECT_THROW(m.on_start(Block{}, bb), MalConfig);
  Block bad; bad.kv["name"] = "not a name";      // whitespace fails valid_peer_name
  EXPECT_THROW(m.on_start(bad, bb), MalConfig);
}

TEST(BridgeModule, UnsetSecretEnvThrowsMalConfig) {
  Blackboard bb;
  BridgeModule m;                                 // no injected secret -> resolves env
  Block cfg;
  cfg.kv["name"] = "worker1";
  cfg.kv["secret_env"] = "HADES_TEST_BRIDGE_SECRET_UNSET_XYZ";
  EXPECT_THROW(m.on_start(cfg, bb), MalConfig);
}

TEST(BridgeModule, AskDrivesTurnAndReturnsReply) {
  Rig r;
  auto res = r.mod->handle_ask(ask_body("front", 0, "hello"), "s3cret");
  ASSERT_TRUE(res.value("ok", false)) << res.dump();
  EXPECT_EQ(res.value("reply", ""), "echo:(from peer agent \"front\") hello");
}

TEST(BridgeModule, AskPostsPeerTurnOrigin) {
  Rig r(false);
  std::string origin;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry& e) {
    auto o = r.bb.get("TURN_ORIGIN");
    origin = (o && o->value.is_string()) ? o->value.get<std::string>() : "<missing>";
    r.bb.post("ASSISTANT_MESSAGE", "ok", "t");
  });
  ASSERT_TRUE(r.mod->handle_ask(ask_body("front", 0, "hi"), "s3cret").value("ok", false));
  EXPECT_EQ(origin, "peer:front");   // posted BEFORE USER_MESSAGE (latest-value visible)
}

TEST(BridgeModule, BadSecretOrUnknownPeerIsForbidden) {
  Rig r;
  bool reached = false;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { reached = true; });
  auto bad_secret = r.mod->handle_ask(ask_body("front", 0, "hi"), "wrong");
  EXPECT_FALSE(bad_secret.value("ok", true));
  EXPECT_EQ(bad_secret.value("error", ""), "forbidden");
  auto unknown = r.mod->handle_ask(ask_body("stranger", 0, "hi"), "s3cret");
  EXPECT_FALSE(unknown.value("ok", true));
  EXPECT_EQ(unknown.value("error", ""), "forbidden");   // indistinguishable from bad secret
  EXPECT_FALSE(reached);
}

TEST(BridgeModule, HopLimitRejected) {
  Rig r;
  auto res = r.mod->handle_ask(ask_body("front", 1, "hi"), "s3cret");   // max_hops default 1
  EXPECT_FALSE(res.value("ok", true));
  EXPECT_NE(res.value("error", "").find("hop"), std::string::npos);
}

TEST(BridgeModule, MalformedAskRejectedWithoutTurn) {
  Rig r;
  bool reached = false;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { reached = true; });
  EXPECT_FALSE(r.mod->handle_ask("garbage", "s3cret").value("ok", true));
  EXPECT_FALSE(reached);
}

TEST(BridgeModule, ConfirmGatedActionIsAutoDenied) {
  Rig r(false);
  // Script an agent whose turn raises a confirm; the module must post the denial and the
  // "agent" then finishes the turn (mirrors the Arbiter's confirm continuation).
  nlohmann::json confirm_response;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) {
    r.bb.post("CONFIRM_REQUEST", {{"id", "c1"}, {"prompt", "run shell?"}}, "arbiter");
  });
  r.bb.subscribe("CONFIRM_RESPONSE", [&](const Entry& e) {
    confirm_response = e.value;
    r.bb.post("ASSISTANT_MESSAGE", "[declined]", "arbiter");
  });
  auto res = r.mod->handle_ask(ask_body("front", 0, "do risky thing"), "s3cret");
  ASSERT_TRUE(res.value("ok", false)) << res.dump();
  ASSERT_TRUE(confirm_response.is_object());
  EXPECT_EQ(confirm_response.value("id", ""), "c1");
  EXPECT_FALSE(confirm_response.value("approved", true));
  // The reply carries the standing auto-deny note for the asker's LLM.
  EXPECT_NE(res.value("reply", "").find("auto-denied"), std::string::npos);
}

TEST(BridgeModule, ForeignTurnConfirmIsNotAnswered) {
  Rig r(false);
  bool responded = false;
  r.bb.subscribe("CONFIRM_RESPONSE", [&](const Entry&) { responded = true; });
  // A confirm arriving OUTSIDE a bridge-driven turn (REPL/web turn) is not the bridge's.
  r.bb.post("CONFIRM_REQUEST", {{"id", "x"}, {"prompt", "p"}}, "arbiter");
  r.bb.pump();
  EXPECT_FALSE(responded);
}

TEST(BridgeModule, ShareStoresPrefixedKey) {
  Rig r;
  auto res = r.mod->handle_share(build_share("front", "STATUS", "sunny").dump(), "s3cret");
  EXPECT_TRUE(res.value("ok", false));
  auto e = r.bb.get("PEER.front.STATUS");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->value.get<std::string>(), "sunny");
}

TEST(BridgeModule, ShareRejectsForbiddenOversizedAndMalformed) {
  Rig r;
  EXPECT_EQ(r.mod->handle_share(build_share("front", "K", 1).dump(), "wrong")
                .value("error", ""), "forbidden");
  EXPECT_EQ(r.mod->handle_share(build_share("ghost", "K", 1).dump(), "s3cret")
                .value("error", ""), "forbidden");
  const std::string big = build_share("front", "K", std::string(kMaxShareBytes, 'x')).dump();
  EXPECT_FALSE(r.mod->handle_share(big, "s3cret").value("ok", true));      // > 64 KB body
  EXPECT_FALSE(r.mod->handle_share("garbage", "s3cret").value("ok", true));
  EXPECT_FALSE(r.bb.get("PEER.front.K").has_value());
}

TEST(BridgeModule, HealthReportsNameAndVersion) {
  Rig r;
  auto h = r.mod->health_json();
  EXPECT_EQ(h.value("name", ""), "worker1");
  EXPECT_EQ(h.value("v", 0), 1);
}

TEST(BridgeModule, AskTimeoutAbandonsTurn) {
  Rig r(false);                                   // NO responder -> run_until must time out
  r.mod->set_turn_timeout_s(0.05);
  bool abandoned = false;
  r.bb.subscribe("TURN_ABANDONED", [&](const Entry&) { abandoned = true; });
  auto res = r.mod->handle_ask(ask_body("front", 0, "hi"), "s3cret");
  EXPECT_FALSE(res.value("ok", true));
  EXPECT_NE(res.value("error", "").find("timed out"), std::string::npos);
  EXPECT_TRUE(abandoned);
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** In `CMakeLists.txt` after the `tests/test_turn_gate.cpp` line (~line 130), add:

```cmake
target_sources(hades_core PRIVATE src/module/bridge_module.cpp)
target_sources(hades_tests PRIVATE tests/test_bridge_module.cpp)
```

- [ ] **Step 3: Implement.** `include/hades/module/bridge_module.h`:

```cpp
// include/hades/module/bridge_module.h — agent↔agent bridge app (pShare analogue)
//
// The inbound half of the multi-agent bridge (spec 2026-07-03): peers reach this agent over
// HTTP with a shared-secret header. POST /ask drives ONE whole turn through the shared
// TurnGate exactly like the REPL/web/Telegram front-ends — the peer's request passes THIS
// agent's own Arbiter/objectives; a confirm-band action is AUTO-DENIED (a peer cannot grant
// human confirmation). POST /share stores the payload under PEER.<from>.<key> (fixed rename
// on arrival — a peer can never inject a local bus key). Security: the secret comes from an
// env var (secret_env, default HADES_BRIDGE_SECRET; NEVER the manifest) and `from` must be a
// rostered Peer — either failure returns an indistinguishable "forbidden". The socket layer
// (Task 4) is a thin shell over the socket-free handle_ask/handle_share seams below.
#pragma once
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "hades/module.h"
#include "hades/turn_gate.h"
namespace hades {
class Blackboard;

class BridgeModule : public Module {
 public:
  // Test injection: a non-empty secret skips the on_start env-var resolution.
  explicit BridgeModule(std::string secret_for_test = "") : secret_(std::move(secret_for_test)) {}
  std::string type() const override { return "bridge"; }
  void on_start(const Block& cfg, Blackboard& bb) override;
  void on_attach(Blackboard& bb) override;

  void set_peers(std::map<std::string, std::string> peers) { peers_ = std::move(peers); }
  void set_turn_gate(TurnGate* g) { gate_ = g; }
  void set_turn_timeout_s(double s) { turn_timeout_override_s_ = s; }

  // Socket-free request handlers (the Task-4 routes are thin shells over these).
  // presented_secret is the X-Hades-Bridge header value; a bad secret OR an unknown `from`
  // returns {"ok":false,"error":"forbidden"} (indistinguishable — no info leak).
  nlohmann::json handle_ask(const std::string& body, const std::string& presented_secret);
  nlohmann::json handle_share(const std::string& body, const std::string& presented_secret);
  nlohmann::json health_json() const;

  const std::string& name() const { return name_; }
  double ask_timeout_s() const { return ask_timeout_s_; }
  const std::vector<std::string>& share_out_keys() const { return share_out_; }

 private:
  std::string host_ = "127.0.0.1";
  int port_ = 9090;
  bool authorized_(const std::string& presented_secret, const std::string& from) const;
  std::mutex& turn_mu_() { return gate_ ? gate_->mu : local_gate_.mu; }
  double effective_timeout_() const;

  std::string name_;                       // REQUIRED (valid_peer_name)
  std::string secret_;                     // resolved once (env or test injection)
  std::map<std::string, std::string> peers_;   // name -> base url (allowlist + push targets)
  std::vector<std::string> share_out_;     // keys pushed to peers on change (Task 3)
  long long max_hops_ = 1;
  double ask_timeout_s_ = 180.0;           // kDefaultAskTimeoutS (set in on_start)
  double turn_timeout_override_s_ = 0.0;

  Blackboard* bb_ = nullptr;
  TurnGate* gate_ = nullptr;
  TurnGate local_gate_;

  // Turn-capture state (request thread only, under the gate while a turn runs).
  bool my_turn_ = false;
  bool got_reply_ = false;
  std::string last_reply_;
  bool denied_confirm_ = false;            // a confirm was auto-denied during MY turn
};
}  // namespace hades
```

`src/module/bridge_module.cpp`:

```cpp
// src/module/bridge_module.cpp — inbound bridge: auth, allowlist, peer turns, share ingest
#include "hades/module/bridge_module.h"
#include <cstdlib>
#include <sstream>
#include "hades/blackboard.h"
#include "hades/bridge/protocol.h"
#include "hades/config.h"
#include "hades/launcher.h"    // MalConfig
#include "hades/timeouts.h"    // kDefaultTurnIdleTimeoutS, kDefaultAskTimeoutS
namespace hades {

double BridgeModule::effective_timeout_() const {
  return turn_timeout_override_s_ > 0.0 ? turn_timeout_override_s_ : kDefaultTurnIdleTimeoutS;
}

void BridgeModule::on_start(const Block& cfg, Blackboard&) {
  // The agent's bridge identity. REQUIRED: it is the `from` peers verify us by, half of the
  // TURN_ORIGIN value, and the PEER.<name>. prefix on the receiving side. Fail fast + loud.
  if (!cfg.kv.count("name") || !valid_peer_name(cfg.kv.at("name")))
    throw MalConfig("bridge module requires a valid name ([A-Za-z0-9_-]{1,64})");
  name_ = cfg.kv.at("name");
  if (cfg.kv.count("host")) host_ = cfg.kv.at("host");
  if (cfg.kv.count("port")) {
    double p = 0.0;
    if (set_pos_double_on_string(cfg.kv.at("port"), p)) port_ = static_cast<int>(p);
  }
  if (cfg.kv.count("max_hops")) {
    double h = 0.0;
    if (set_pos_double_on_string(cfg.kv.at("max_hops"), h)) max_hops_ = static_cast<long long>(h);
  }
  ask_timeout_s_ = kDefaultAskTimeoutS;
  if (cfg.kv.count("ask_timeout_s")) {
    double t = 0.0;
    if (set_pos_double_on_string(cfg.kv.at("ask_timeout_s"), t)) ask_timeout_s_ = t;
  }
  if (cfg.kv.count("share_out")) {
    std::istringstream is(cfg.kv.at("share_out"));
    std::string k;
    while (is >> k) share_out_.push_back(k);
  }
  if (secret_.empty()) {                    // not injected (tests) -> resolve the env var
    const std::string env =
        cfg.kv.count("secret_env") ? cfg.kv.at("secret_env") : "HADES_BRIDGE_SECRET";
    const char* sec = std::getenv(env.c_str());
    if (!sec || !*sec)
      throw MalConfig("bridge secret env var not set or empty: " + env);
    secret_ = sec;
  }
}

void BridgeModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  // Turn-owner guard: capture ONLY for turns this module drives (my_turn_) — a REPL/web/
  // Telegram turn's reply or confirm is not ours (symmetric to the other front-ends).
  bb.subscribe("ASSISTANT_MESSAGE", [this](const Entry& e) {
    if (!my_turn_ || !e.value.is_string()) return;
    last_reply_ = e.value.get<std::string>();
    got_reply_ = true;
  });
  bb.subscribe("CONFIRM_REQUEST", [this](const Entry& e) {
    if (!my_turn_ || !e.value.is_object()) return;
    // AUTO-DENY (spec decision): a peer request cannot grant human confirmation. Post the
    // denial immediately — the Arbiter continues the turn to its final reply, and handle_ask
    // appends the standing note so the asker's LLM knows why.
    denied_confirm_ = true;
    auto id = e.value.find("id");
    bb_->post("CONFIRM_RESPONSE",
              {{"id", (id != e.value.end() && id->is_string()) ? id->get<std::string>() : ""},
               {"approved", false}},
              "bridge");
  });
}

bool BridgeModule::authorized_(const std::string& presented_secret,
                               const std::string& from) const {
  // One combined answer: a bad secret and an unknown peer are indistinguishable (no info leak).
  return !secret_.empty() && presented_secret == secret_ && peers_.count(from) > 0;
}

nlohmann::json BridgeModule::handle_ask(const std::string& body,
                                        const std::string& presented_secret) {
  BridgeMsg m = parse_ask(body);
  if (!m.ok) return {{"ok", false}, {"error", m.error}};
  if (!authorized_(presented_secret, m.from)) return {{"ok", false}, {"error", "forbidden"}};
  if (m.hops >= max_hops_) return {{"ok", false}, {"error", "hop limit exceeded"}};

  std::lock_guard<std::mutex> lk(turn_mu_());     // one turn at a time across ALL front-ends
  my_turn_ = true;
  // RAII reset declared AFTER the lock: runs BEFORE the mutex releases on EVERY exit path.
  struct Reset { bool& f; ~Reset() { f = false; } } reset{my_turn_};
  got_reply_ = false;
  last_reply_.clear();
  denied_confirm_ = false;
  // Origin BEFORE the message: the latest-value map updates on post, so the PeerLoopGuard
  // (and anything else) sees "peer:<name>" for the whole turn.
  bb_->post("TURN_ORIGIN", "peer:" + m.from, "bridge");
  bb_->post("USER_MESSAGE", "(from peer agent \"" + m.from + "\") " + m.message, "bridge");
  const bool done = bb_->run_until([this] { return got_reply_; }, effective_timeout_());
  if (!done) {
    bb_->post("TURN_ABANDONED", nlohmann::json::object(), "bridge");
    bb_->pump();
    return {{"ok", false}, {"error", "turn timed out"}};
  }
  std::string reply = last_reply_;
  if (denied_confirm_)
    reply += "\n[note: a confirm-gated action was auto-denied — peer requests cannot grant "
             "human confirmation]";
  return {{"ok", true}, {"reply", reply}};
}

nlohmann::json BridgeModule::handle_share(const std::string& body,
                                          const std::string& presented_secret) {
  if (body.size() > kMaxShareBytes) return {{"ok", false}, {"error", "payload too large"}};
  BridgeMsg m = parse_share(body);
  if (!m.ok) return {{"ok", false}, {"error", m.error}};
  if (!authorized_(presented_secret, m.from)) return {{"ok", false}, {"error", "forbidden"}};
  // No turn, no gate: thread-safe post; the PEER. prefix is collision-proof by construction.
  bb_->post(peer_bus_key(m.from, m.key), m.value, "bridge");
  return {{"ok", true}};
}

nlohmann::json BridgeModule::health_json() const {
  return {{"name", name_}, {"v", kBridgeProtocolV}};
}
}  // namespace hades
```

Also add to `include/hades/timeouts.h`, next to the existing defaults:

```cpp
inline constexpr double kDefaultAskTimeoutS = 180.0;   // ask_agent peer-call timeout (Bridge block)
```

(Include `<mutex>` and `<sstream>` where needed; `turn_gate.h` already brings `<mutex>`.)

- [ ] **Step 4: Build + test.** `-R BridgeModule` → pass; full suite green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/module/bridge_module.h src/module/bridge_module.cpp include/hades/timeouts.h tests/test_bridge_module.cpp CMakeLists.txt
git commit -m "feat: BridgeModule inbound — auth+allowlist, peer turns via TurnGate, confirm auto-deny, share ingest"
```

---

## Task 3: Outbound share push (BridgeHttp seam + Executor offload)

**Files:**
- Create: `include/hades/bridge/http.h`, `src/bridge/cpr_bridge_http.cpp`
- Modify: `include/hades/module/bridge_module.h`, `src/module/bridge_module.cpp`
- Test: append to `tests/test_bridge_module.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `struct BridgeHttp { virtual ~BridgeHttp() = default; virtual std::pair<int,std::string> post_json(const std::string& url, const std::string& body, const std::string& secret, double timeout_s) = 0; };` (returns {status, body}; status 0 = transport failure); `class CprBridgeHttp : public BridgeHttp` (cpr impl, X-Hades-Bridge header). `BridgeModule` gains: `explicit BridgeModule(std::unique_ptr<BridgeHttp> http, std::string secret_for_test = "")`; `void set_executor(Executor* ex)`; share_out subscriptions that push `POST <peer_url>/share` to ALL peers on key change (via Executor when set, inline otherwise); failures post `BRIDGE_ERROR` (string) and never throw.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_bridge_module.cpp`:

```cpp
namespace {
struct FakeHttp : BridgeHttp {
  std::vector<std::tuple<std::string, std::string, std::string>> posts;  // url, body, secret
  int status = 200;
  std::pair<int, std::string> post_json(const std::string& url, const std::string& body,
                                        const std::string& secret, double) override {
    posts.emplace_back(url, body, secret);
    return {status, R"({"ok":true})"};
  }
};
}  // namespace

TEST(BridgeModule, ShareOutPushesToAllPeersOnChange) {
  Blackboard bb;
  auto http = std::make_unique<FakeHttp>();
  FakeHttp* h = http.get();
  BridgeModule m(std::move(http), "s3cret");
  Block cfg;
  cfg.kv["name"] = "worker1";
  cfg.kv["share_out"] = "STATUS BUDGET_SPENT_USD";
  m.on_start(cfg, bb);
  m.set_peers({{"front", "http://10.0.0.1:9090"}, {"other", "http://10.0.0.2:9090"}});
  m.on_attach(bb);
  bb.post("STATUS", "sunny", "t");
  bb.pump();                                     // no executor -> push runs inline
  ASSERT_EQ(h->posts.size(), 2u);                // one per peer
  EXPECT_EQ(std::get<0>(h->posts[0]), "http://10.0.0.1:9090/share");
  EXPECT_EQ(std::get<2>(h->posts[0]), "s3cret");
  auto sent = parse_share(std::get<1>(h->posts[0]));
  ASSERT_TRUE(sent.ok);
  EXPECT_EQ(sent.from, "worker1");
  EXPECT_EQ(sent.key, "STATUS");
  EXPECT_EQ(sent.value.get<std::string>(), "sunny");
}

TEST(BridgeModule, UnlistedKeyIsNotPushed) {
  Blackboard bb;
  auto http = std::make_unique<FakeHttp>();
  FakeHttp* h = http.get();
  BridgeModule m(std::move(http), "s3cret");
  Block cfg; cfg.kv["name"] = "worker1"; cfg.kv["share_out"] = "STATUS";
  m.on_start(cfg, bb);
  m.set_peers({{"front", "http://10.0.0.1:9090"}});
  m.on_attach(bb);
  bb.post("OTHER_KEY", 1, "t");
  bb.pump();
  EXPECT_TRUE(h->posts.empty());
}

TEST(BridgeModule, FailedPushPostsBridgeErrorAndDoesNotThrow) {
  Blackboard bb;
  auto http = std::make_unique<FakeHttp>();
  http->status = 0;                              // transport failure
  BridgeModule m(std::move(http), "s3cret");
  Block cfg; cfg.kv["name"] = "worker1"; cfg.kv["share_out"] = "STATUS";
  m.on_start(cfg, bb);
  m.set_peers({{"front", "http://10.0.0.1:9090"}});
  m.on_attach(bb);
  std::string err;
  bb.subscribe("BRIDGE_ERROR", [&](const Entry& e) {
    if (e.value.is_string()) err = e.value.get<std::string>();
  });
  bb.post("STATUS", "x", "t");
  bb.pump();
  bb.pump();                                     // dispatch the BRIDGE_ERROR posted inline
  EXPECT_NE(err.find("front"), std::string::npos);
}
```

- [ ] **Step 2: Run — expect FAIL** (no `BridgeHttp`, no new ctor).
- [ ] **Step 3: Implement.** `include/hades/bridge/http.h`:

```cpp
// include/hades/bridge/http.h — outbound bridge HTTP seam (real impl: cpr; tests: fake)
//
// The BridgeModule's share push talks ONLY to this interface, so the push logic is testable
// without a network (TelegramApi precedent). post_json returns {status, body}; status 0 means
// a transport-level failure (connect refused / timeout). Never throws.
#pragma once
#include <string>
#include <utility>
namespace hades {
struct BridgeHttp {
  virtual ~BridgeHttp() = default;
  virtual std::pair<int, std::string> post_json(const std::string& url, const std::string& body,
                                                const std::string& secret, double timeout_s) = 0;
};

class CprBridgeHttp : public BridgeHttp {
 public:
  std::pair<int, std::string> post_json(const std::string& url, const std::string& body,
                                        const std::string& secret, double timeout_s) override;
};
}  // namespace hades
```

`src/bridge/cpr_bridge_http.cpp`:

```cpp
// src/bridge/cpr_bridge_http.cpp — thin cpr shell for outbound bridge requests
#include "hades/bridge/http.h"
#include <exception>
#include <cpr/cpr.h>
namespace hades {
std::pair<int, std::string> CprBridgeHttp::post_json(const std::string& url,
                                                     const std::string& body,
                                                     const std::string& secret,
                                                     double timeout_s) {
  try {
    cpr::Response r = cpr::Post(
        cpr::Url{url}, cpr::Body{body},
        cpr::Header{{"Content-Type", "application/json"}, {"X-Hades-Bridge", secret}},
        cpr::Timeout{static_cast<long>(timeout_s * 1000)}, cpr::Redirect{false});
    return {static_cast<int>(r.status_code), r.text};
  } catch (const std::exception&) {
    return {0, ""};   // transport failure — the caller degrades (never throws)
  }
}
}  // namespace hades
```

In `include/hades/module/bridge_module.h`: add includes `#include <memory>` and `#include "hades/bridge/http.h"`; forward-declare `class Executor;` inside `namespace hades`; add to the public section:

```cpp
  // Test injection of the outbound HTTP seam (real path: on_start creates CprBridgeHttp).
  // Delegating ctor: no initializer-order coupling to the member declaration order.
  explicit BridgeModule(std::unique_ptr<BridgeHttp> http, std::string secret_for_test = "")
      : BridgeModule(std::move(secret_for_test)) { http_ = std::move(http); }
  void set_executor(Executor* ex) { executor_ = ex; }
```

and to the private members:

```cpp
  std::unique_ptr<BridgeHttp> http_;
  Executor* executor_ = nullptr;   // push jobs run here when set (inline otherwise — tests)
  void push_share_(const std::string& key, const nlohmann::json& value);
```

In `src/module/bridge_module.cpp`: add `#include "hades/executor.h"`. At the end of `on_start`, after the secret resolution:

```cpp
  if (!http_) http_ = std::make_unique<CprBridgeHttp>();
```

At the end of `on_attach`, subscribe the share_out keys:

```cpp
  // Outbound pShare: on a change of any listed key, push it to ALL peers. The handler runs on
  // the pump thread, so the (possibly slow) HTTP posts are offloaded to the Executor when one
  // is set; without one (tests) they run inline and stay deterministic. Fire-and-forget.
  for (const auto& key : share_out_) {
    bb.subscribe(key, [this, key](const Entry& e) {
      // Capture by value: the worker must not touch `e` after the handler returns.
      const nlohmann::json value = e.value;
      auto job = [this, key, value] { push_share_(key, value); };
      if (executor_) executor_->submit(job);
      else job();
    });
  }
```

and implement the push:

```cpp
void BridgeModule::push_share_(const std::string& key, const nlohmann::json& value) {
  // Best-effort: a peer being down must never disturb the agent. status 0 = transport failure;
  // any non-2xx counts as failed too. BRIDGE_ERROR is observable in hades-scope and tests.
  const std::string body = build_share(name_, key, value).dump();
  for (const auto& [peer, url] : peers_) {
    try {
      auto [status, resp] = http_->post_json(url + "/share", body, secret_, 10.0);
      if (status < 200 || status >= 300)
        bb_->post("BRIDGE_ERROR", "share push to " + peer + " failed (status " +
                                      std::to_string(status) + ")", "bridge");
    } catch (...) {
      bb_->post("BRIDGE_ERROR", "share push to " + peer + " failed (exception)", "bridge");
    }
  }
}
```

(`Executor::submit(std::function<void()>)` is the enqueue method — `include/hades/executor.h`; tasks are dropped after the dtor has begun and exceptions are swallowed on the worker, so the fire-and-forget push is safe.)

- [ ] **Step 4: CMake.** After the `src/bridge/protocol.cpp` line add:

```cmake
target_sources(hades_core PRIVATE src/bridge/cpr_bridge_http.cpp)
```

- [ ] **Step 5: Build + test.** `-R BridgeModule` → pass; full suite green.
- [ ] **Step 6: Commit.**

```bash
git add include/hades/bridge/http.h src/bridge/cpr_bridge_http.cpp include/hades/module/bridge_module.h src/module/bridge_module.cpp tests/test_bridge_module.cpp CMakeLists.txt
git commit -m "feat: bridge outbound share push (BridgeHttp seam, executor offload, BRIDGE_ERROR)"
```

---

## Task 4: Bridge listener (httplib server thread)

**Files:**
- Modify: `include/hades/module/bridge_module.h`, `src/module/bridge_module.cpp`
- Test: append to `tests/test_bridge_module.cpp`

**Interfaces:**
- Produces: `int start_listening()` — binds (port 0 ⇒ any free port), spawns the server thread, returns the bound port (idempotent: returns the current port if already listening; returns -1 on bind failure); `int port() const`; `void wait()` (join — bridge-only roster blocks here); `~BridgeModule()` stops + joins. Routes: `POST /ask` → `handle_ask(body, header)`, `POST /share` → `handle_share(body, header)`, `GET /health` → `health_json()` (auth required: bad secret ⇒ 403). An `{"error":"forbidden"}` handler result maps to HTTP 403; everything else returns 200 with the JSON body.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_bridge_module.cpp` (add `#include <httplib.h>` at the top of the file):

```cpp
TEST(BridgeModule, RealSocketAskShareHealthAnd403) {
  Rig r;                                          // echo agent, secret s3cret, peer front
  const int port = r.mod->start_listening();      // port_ default 9090? Rig sets none -> use 0
  ASSERT_GT(port, 0);
  httplib::Client cli("127.0.0.1", port);

  // /health with auth
  auto h = cli.Get("/health", httplib::Headers{{"X-Hades-Bridge", "s3cret"}});
  ASSERT_TRUE(h);
  EXPECT_EQ(h->status, 200);
  EXPECT_NE(h->body.find("worker1"), std::string::npos);
  // /health without auth -> 403
  auto h403 = cli.Get("/health");
  ASSERT_TRUE(h403);
  EXPECT_EQ(h403->status, 403);

  // /ask end-to-end over the socket
  auto a = cli.Post("/ask", httplib::Headers{{"X-Hades-Bridge", "s3cret"}},
                    build_ask("front", 0, "ping").dump(), "application/json");
  ASSERT_TRUE(a);
  EXPECT_EQ(a->status, 200);
  auto aj = nlohmann::json::parse(a->body, nullptr, false);
  ASSERT_TRUE(aj.value("ok", false));
  EXPECT_NE(aj.value("reply", "").find("ping"), std::string::npos);

  // /ask with a bad secret -> 403
  auto f = cli.Post("/ask", httplib::Headers{{"X-Hades-Bridge", "wrong"}},
                    build_ask("front", 0, "ping").dump(), "application/json");
  ASSERT_TRUE(f);
  EXPECT_EQ(f->status, 403);

  // /share over the socket
  auto s = cli.Post("/share", httplib::Headers{{"X-Hades-Bridge", "s3cret"}},
                    build_share("front", "STATUS", "wet").dump(), "application/json");
  ASSERT_TRUE(s);
  EXPECT_EQ(s->status, 200);
  auto e = r.bb.get("PEER.front.STATUS");
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->value.get<std::string>(), "wet");
}
```

In the `Rig` constructor, set an ephemeral port so tests never collide: after `cfg.kv["name"] = "worker1";` add `cfg.kv["port"] = "0";` — and in `on_start`, `port = 0` must survive (adjust the `set_pos_double_on_string` guard: `0` is not positive, so read the port with an explicit branch — see Step 2).

- [ ] **Step 2: Implement.** In `include/hades/module/bridge_module.h`: add includes `<atomic>` and `<thread>`; forward-declare httplib before `namespace hades` (http_server_module.h precedent):

```cpp
namespace httplib {
class Server;
}
```

Public additions:

```cpp
  ~BridgeModule() override;   // stop + join the listener thread
  // Bind (port 0 -> any free port) and serve on a background thread. Called by hades_main
  // AFTER the graph is wired (never by on_attach — tests spawn no thread unless they ask).
  // Returns the bound port, or -1 on bind failure. Idempotent.
  int start_listening();
  int port() const { return port_; }
  void wait();                // join (bridge-only roster blocks here; Ctrl-C exits)
```

Private additions:

```cpp
  std::unique_ptr<httplib::Server> srv_;
  std::thread listen_thread_;
```

In `on_start`, replace the port read with an explicit branch (port 0 is a legal "ephemeral" request that `set_pos_double_on_string` would reject):

```cpp
  if (cfg.kv.count("port")) {
    try { port_ = std::stoi(cfg.kv.at("port")); } catch (const std::exception&) {}
    if (port_ < 0 || port_ > 65535) port_ = 9090;
  }
```

In `src/module/bridge_module.cpp`: add `#include <httplib.h>` and `#include <iostream>`; implement:

```cpp
BridgeModule::~BridgeModule() {
  if (srv_) srv_->stop();                          // wakes the listen() thread
  if (listen_thread_.joinable()) listen_thread_.join();
}

int BridgeModule::start_listening() {
  if (listen_thread_.joinable()) return port_;     // idempotent
  srv_ = std::make_unique<httplib::Server>();
  // Socket timeouts above the turn idle ceiling so a long-but-legit /ask is never cut off
  // mid-turn (HttpServerModule::configure_server_ rationale).
  const time_t secs = static_cast<time_t>(effective_timeout_()) + 60;
  srv_->set_read_timeout(secs, 0);
  srv_->set_write_timeout(secs, 0);

  // Routes are thin shells over the socket-free handlers; a "forbidden" result maps to 403.
  auto respond = [](httplib::Response& res, const nlohmann::json& out) {
    if (out.value("error", "") == "forbidden") res.status = 403;
    res.set_content(out.dump(), "application/json");
  };
  srv_->Post("/ask", [this, respond](const httplib::Request& req, httplib::Response& res) {
    respond(res, handle_ask(req.body, req.get_header_value("X-Hades-Bridge")));
  });
  srv_->Post("/share", [this, respond](const httplib::Request& req, httplib::Response& res) {
    respond(res, handle_share(req.body, req.get_header_value("X-Hades-Bridge")));
  });
  srv_->Get("/health", [this, respond](const httplib::Request& req, httplib::Response& res) {
    // Auth on /health too (spec): liveness is fleet-internal, not public.
    if (req.get_header_value("X-Hades-Bridge") != secret_) {
      respond(res, {{"ok", false}, {"error", "forbidden"}});
      return;
    }
    respond(res, health_json());
  });

  // Bind BEFORE spawning the thread so the caller learns the real port synchronously
  // (port 0 -> ephemeral; tests use this so parallel runs never collide).
  if (port_ == 0) {
    const int bound = srv_->bind_to_any_port(host_);
    if (bound <= 0) { srv_.reset(); return -1; }
    port_ = bound;
  } else if (!srv_->bind_to_port(host_, port_)) {
    srv_.reset();
    return -1;
  }
  listen_thread_ = std::thread([this] { srv_->listen_after_bind(); });
  return port_;
}

void BridgeModule::wait() {
  if (listen_thread_.joinable()) listen_thread_.join();
}
```

- [ ] **Step 3: Build + test.** `-R BridgeModule` → pass (including the real-socket test); full suite green.
- [ ] **Step 4: Commit.**

```bash
git add include/hades/module/bridge_module.h src/module/bridge_module.cpp tests/test_bridge_module.cpp
git commit -m "feat: bridge listener — httplib server thread, 403 mapping, ephemeral-port bind"
```

---

## Task 5: PeerLoopGuard + TURN_ORIGIN + PeerAsk capability

**Files:**
- Create: `include/hades/objective/peer_loop_guard.h`, `src/objective/peer_loop_guard.cpp`
- Modify: `src/module/chat_module.cpp` (2 REPL loops), `src/module/http_server_module.cpp` (`handle_message`), `src/module/telegram_module.cpp` (`drive_turn_`), `include/hades/objective/capability_policy.h` (enum), `src/objective/capability_policy.cpp` (`capability_of` + veto switch)
- Test: create `tests/test_peer_loop_guard.cpp`; append to `tests/test_capability_policy.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `class PeerLoopGuard : public Objective` — `type()=="peer_loop_guard"`; `veto(bb, action)` hard-vetoes (`vetoed=true, needs_confirm=false`) a ToolCall of `ask_agent` when `bb.get("TURN_ORIGIN")` is a string starting with `"peer:"`; allows everything else. `Capability::PeerAsk` with `capability_of("ask_agent") == PeerAsk` → `allow()`. Every human front-end posts `TURN_ORIGIN = "human"` immediately before its turn-triggering post.

- [ ] **Step 1: Write the failing tests** `tests/test_peer_loop_guard.cpp`:

```cpp
// tests/test_peer_loop_guard.cpp — PeerLoopGuard: no onward ask_agent in a peer-driven turn
#include <gtest/gtest.h>
#include "hades/blackboard.h"
#include "hades/objective/peer_loop_guard.h"
using namespace hades;

namespace {
Action ask_action() {
  Action a;
  a.kind = Action::Kind::ToolCall;
  a.tool = "ask_agent";
  a.args = {{"peer", "front"}, {"message", "hi"}};
  return a;
}
}  // namespace

TEST(PeerLoopGuard, VetoesAskAgentInPeerTurn) {
  Blackboard bb;
  bb.post("TURN_ORIGIN", "peer:front", "bridge");
  PeerLoopGuard g;
  auto v = g.veto(bb, ask_action());
  EXPECT_TRUE(v.vetoed);
  EXPECT_FALSE(v.needs_confirm);                 // hard veto, not confirm
}

TEST(PeerLoopGuard, AllowsAskAgentInHumanTurn) {
  Blackboard bb;
  bb.post("TURN_ORIGIN", "human", "chat");
  PeerLoopGuard g;
  EXPECT_FALSE(g.veto(bb, ask_action()).vetoed);
}

TEST(PeerLoopGuard, AllowsWhenOriginAbsentOrMalformed) {
  Blackboard bb;
  PeerLoopGuard g;
  EXPECT_FALSE(g.veto(bb, ask_action()).vetoed);     // no TURN_ORIGIN at all
  bb.post("TURN_ORIGIN", 42, "x");                   // non-string
  EXPECT_FALSE(g.veto(bb, ask_action()).vetoed);
}

TEST(PeerLoopGuard, IgnoresOtherToolsAndAnswers) {
  Blackboard bb;
  bb.post("TURN_ORIGIN", "peer:front", "bridge");
  PeerLoopGuard g;
  Action fs; fs.kind = Action::Kind::ToolCall; fs.tool = "fs_read";
  EXPECT_FALSE(g.veto(bb, fs).vetoed);           // only ask_agent is loop-relevant
  Action ans; ans.kind = Action::Kind::Answer;
  EXPECT_FALSE(g.veto(bb, ans).vetoed);
}
```

Append to `tests/test_capability_policy.cpp`:

```cpp
TEST(CapabilityPolicy, AskAgentHasPeerAskCapabilityAndIsAllowed) {
  EXPECT_EQ(CapabilityPolicy::capability_of("ask_agent"), Capability::PeerAsk);
  CapabilityScope sc;                            // defaults: confirm_unscoped = true — proves
  CapabilityPolicy p(sc);                        // this is NOT the Unknown->confirm path
  Blackboard bb;
  Action a;
  a.kind = Action::Kind::ToolCall;
  a.tool = "ask_agent";
  a.args = {{"peer", "front"}, {"message", "hi"}};
  EXPECT_FALSE(p.veto(bb, a).vetoed);
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** After the `tests/test_capability_wiring.cpp` line add:

```cmake
target_sources(hades_core PRIVATE src/objective/peer_loop_guard.cpp)
target_sources(hades_tests PRIVATE tests/test_peer_loop_guard.cpp)
```

- [ ] **Step 3: Implement.** `include/hades/objective/peer_loop_guard.h`:

```cpp
// include/hades/objective/peer_loop_guard.h — no onward delegation from a peer-driven turn
//
// Kills the A↔B ask deadlock: A's ask_agent blocks A's pump holding A's TurnGate; if B's
// peer-driven turn could ask A back, both would wait forever. This standing safety behavior
// hard-vetoes ask_agent whenever the current turn's TURN_ORIGIN is "peer:<name>" (posted by
// the BridgeModule before the USER_MESSAGE). AUTO-REGISTERED by wiring whenever the bridge
// module is rostered — the bridge brings its own guard; it is NOT a manifest objective.
// Absent/malformed TURN_ORIGIN (front-ends post "human") -> allow. The wire `hops` field is
// the belt-and-braces second layer at the HTTP boundary.
#pragma once
#include "hades/objective.h"
namespace hades {
class PeerLoopGuard : public Objective {
 public:
  std::string type() const override { return "peer_loop_guard"; }
  VetoResult veto(const Blackboard& bb, const Action& a) const override;
};
}  // namespace hades
```

`src/objective/peer_loop_guard.cpp`:

```cpp
// src/objective/peer_loop_guard.cpp — hard-veto ask_agent when TURN_ORIGIN is peer:*
#include "hades/objective/peer_loop_guard.h"
#include "hades/blackboard.h"
namespace hades {
VetoResult PeerLoopGuard::veto(const Blackboard& bb, const Action& a) const {
  if (a.kind != Action::Kind::ToolCall || a.tool != "ask_agent") return {};
  auto e = bb.get("TURN_ORIGIN");
  if (e && e->value.is_string() && e->value.get<std::string>().rfind("peer:", 0) == 0)
    return {true, "peer-driven turn cannot ask another agent (loop guard)", false};
  return {};
}
}  // namespace hades
```

Capability table: in `include/hades/objective/capability_policy.h` extend the enum:

```cpp
enum class Capability { FsRead, FsWrite, Net, Exec, MemoryAppend, SkillRead, SkillWrite, PeerAsk, Unknown };
```

In `src/objective/capability_policy.cpp`, in `capability_of` before the final `return Capability::Unknown;`:

```cpp
  if (tool == "ask_agent")                               return Capability::PeerAsk;
```

and in the `veto` switch, before `case Capability::Unknown:`:

```cpp
    case Capability::PeerAsk:
      // Delegation to a rostered peer agent: the roster/urls are fixed by wiring argv (never
      // chosen by the LLM) and the RECEIVING agent's own objectives/confirm gates evaluate
      // whatever the request causes — those are the real protection. Distinct capability so a
      // future policy can confirm-gate outbound asks with zero code (SkillWrite precedent).
      return allow();
```

TURN_ORIGIN posts (one line before each turn-triggering USER_MESSAGE post):

- `src/module/chat_module.cpp` — in BOTH REPL loops, directly above `bb_->post("USER_MESSAGE", line, "chat");`:

```cpp
      bb_->post("TURN_ORIGIN", "human", "chat");
```

- `src/module/http_server_module.cpp` — in `handle_message`, directly above `bb_->post("USER_MESSAGE", text, "http");`:

```cpp
  bb_->post("TURN_ORIGIN", "human", "http");
```

- `src/module/telegram_module.cpp` — in `drive_turn_`, directly above `bb_->post(key, post_value, "telegram");`:

```cpp
  bb_->post("TURN_ORIGIN", "human", "telegram");
```

(Unconditional in drive_turn_: both its entry points — message and confirm callback — are human-driven. `handle_confirm` on the serve path continues an existing human turn, whose latest-value origin is already `"human"`; no post needed there.)

- [ ] **Step 4: Build + test.** `-R "PeerLoopGuard|CapabilityPolicy"` → pass; full suite green (chat/serve/telegram tests unaffected — TURN_ORIGIN is a new key nothing else reads).
- [ ] **Step 5: Commit.**

```bash
git add include/hades/objective/peer_loop_guard.h src/objective/peer_loop_guard.cpp include/hades/objective/capability_policy.h src/objective/capability_policy.cpp src/module/chat_module.cpp src/module/http_server_module.cpp src/module/telegram_module.cpp tests/test_peer_loop_guard.cpp tests/test_capability_policy.cpp CMakeLists.txt
git commit -m "feat: PeerLoopGuard objective, TURN_ORIGIN convention, PeerAsk capability"
```

---

## Task 6: `ask_agent` native tool + per-tool timeout

**Files:**
- Create: `tools/ask_agent_main.cpp`
- Test: `tests/test_ask_agent_tool.cpp`
- Modify: `include/hades/tool/registry.h` (ToolEntry.timeout_s), `src/tool/registry.cpp` (`add_from_block` reads it), `src/module/tool_runner.cpp` (per-tool override)
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `build_ask`, `valid_peer_name` (T1); `run_subprocess` in tests.
- Produces: binary `hades-ask-agent` (links `hades_core` — the hades-shell precedent — for protocol reuse + cpr). Argv: `hades-ask-agent <own_name> <secret_env_name> <timeout_s> <name=url>...` (all appended by wiring; bare argv still answers `describe`). LLM args `{peer, message}` (required strings). Success result `{"peer":..., "reply":...}`; all failures `ok:false` with a reason. `ToolEntry` gains `double timeout_s = 0.0;` (0 ⇒ ToolRunner default); `add_from_block` reads an optional `timeout_s` kv; `ToolRunner` dispatch uses `te->timeout_s > 0 ? te->timeout_s : timeout_s_` for BOTH native and mcp.

- [ ] **Step 1: Write the failing tests** `tests/test_ask_agent_tool.cpp`:

```cpp
// tests/test_ask_agent_tool.cpp — drive the hades-ask-agent binary against a stub peer
#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include <thread>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "hades/bridge/protocol.h"
#include "hades/tool/subprocess.h"
using namespace hades;

namespace {
// Stub peer bridge: answers /ask like a real BridgeModule would.
struct StubPeer {
  httplib::Server srv;
  int port = 0;
  std::thread th;
  std::string seen_secret, seen_body;
  StubPeer() {
    srv.Post("/ask", [this](const httplib::Request& req, httplib::Response& res) {
      seen_secret = req.get_header_value("X-Hades-Bridge");
      seen_body = req.body;
      if (seen_secret != "s3cret") {
        res.status = 403;
        res.set_content(R"({"ok":false,"error":"forbidden"})", "application/json");
        return;
      }
      res.set_content(R"({"ok":true,"reply":"42 GB free"})", "application/json");
    });
    port = srv.bind_to_any_port("127.0.0.1");
    th = std::thread([this] { srv.listen_after_bind(); });
  }
  ~StubPeer() { srv.stop(); th.join(); }
};

nlohmann::json run_tool(const std::vector<std::string>& argv, const std::string& stdin_line) {
  ProcResult r = run_subprocess(argv, stdin_line, 30.0);
  return nlohmann::json::parse(r.out, nullptr, false);
}
std::string call(const std::string& peer, const std::string& msg) {
  return nlohmann::json{{"call", "ask_agent"}, {"args", {{"peer", peer}, {"message", msg}}}}
      .dump();
}
}  // namespace

TEST(AskAgentTool, DescribeYieldsSpecWithPeerRoster) {
  ::setenv("HADES_TEST_ASK_SECRET", "s3cret", 1);
  auto j = run_tool({ASK_AGENT_BIN, "front", "HADES_TEST_ASK_SECRET", "5",
                     "worker1=http://127.0.0.1:1"},
                    R"({"call":"describe"})");
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_EQ(j["result"].value("name", ""), "ask_agent");
  // The description names the known peers so the LLM can pick one.
  EXPECT_NE(j["result"].value("description", "").find("worker1"), std::string::npos);
}

TEST(AskAgentTool, AsksPeerAndReturnsReply) {
  StubPeer peer;
  ::setenv("HADES_TEST_ASK_SECRET", "s3cret", 1);
  auto j = run_tool({ASK_AGENT_BIN, "front", "HADES_TEST_ASK_SECRET", "10",
                     "worker1=http://127.0.0.1:" + std::to_string(peer.port)},
                    call("worker1", "disk space?"));
  ASSERT_TRUE(j.value("ok", false)) << j.dump();
  EXPECT_EQ(j["result"].value("reply", ""), "42 GB free");
  EXPECT_EQ(peer.seen_secret, "s3cret");
  auto sent = parse_ask(peer.seen_body);
  ASSERT_TRUE(sent.ok);
  EXPECT_EQ(sent.from, "front");
  EXPECT_EQ(sent.hops, 0);
  EXPECT_EQ(sent.message, "disk space?");
}

TEST(AskAgentTool, UnknownPeerFailsClosed) {
  ::setenv("HADES_TEST_ASK_SECRET", "s3cret", 1);
  auto j = run_tool({ASK_AGENT_BIN, "front", "HADES_TEST_ASK_SECRET", "5",
                     "worker1=http://127.0.0.1:1"},
                    call("ghost", "hi"));
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));
  EXPECT_NE(j["result"].value("error", "").find("unknown peer"), std::string::npos);
}

TEST(AskAgentTool, PeerDownReturnsError) {
  ::setenv("HADES_TEST_ASK_SECRET", "s3cret", 1);
  auto j = run_tool({ASK_AGENT_BIN, "front", "HADES_TEST_ASK_SECRET", "2",
                     "worker1=http://127.0.0.1:1"},          // nothing listens on port 1
                    call("worker1", "hi"));
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));
}

TEST(AskAgentTool, MissingSecretEnvFailsClosed) {
  ::unsetenv("HADES_TEST_ASK_SECRET_MISSING");
  auto j = run_tool({ASK_AGENT_BIN, "front", "HADES_TEST_ASK_SECRET_MISSING", "5",
                     "worker1=http://127.0.0.1:1"},
                    call("worker1", "hi"));
  EXPECT_FALSE(j.value("ok", true));
}

TEST(AskAgentTool, MalformedArgsFailClosed) {
  ::setenv("HADES_TEST_ASK_SECRET", "s3cret", 1);
  const std::vector<std::string> argv = {ASK_AGENT_BIN, "front", "HADES_TEST_ASK_SECRET", "5",
                                         "worker1=http://127.0.0.1:1"};
  for (const char* raw :
       {R"({"call":"ask_agent","args":{"peer":"worker1"}})",              // no message
        R"({"call":"ask_agent","args":{"message":"m"}})",                 // no peer
        R"({"call":"ask_agent","args":{"peer":7,"message":"m"}})",        // non-string
        R"({"call":"nonsense"})"}) {
    auto j = run_tool(argv, raw);
    ASSERT_FALSE(j.is_discarded()) << raw;
    EXPECT_FALSE(j.value("ok", true)) << raw;
  }
}
```

Append a per-tool-timeout test to `tests/test_toolrunner.cpp` (match its existing rig style — it drives the real `hades-fs-read` via TOOL_REQUEST):

```cpp
TEST(ToolRegistry, PerToolTimeoutParsedFromBlock) {
  ToolRegistry reg;
  Block b;
  b.name = "slow_tool";
  b.kv["native"] = "/bin/true";
  b.kv["timeout_s"] = "190";
  reg.add_from_block(b);
  ASSERT_EQ(reg.entries().size(), 1u);
  EXPECT_DOUBLE_EQ(reg.entries()[0].timeout_s, 190.0);
  Block d;
  d.name = "default_tool";
  d.kv["native"] = "/bin/true";
  reg.add_from_block(d);
  EXPECT_DOUBLE_EQ(reg.entries()[1].timeout_s, 0.0);   // 0 -> runner default
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** After the `hades-save-skill` block (~line 115) add:

```cmake
add_executable(hades-ask-agent tools/ask_agent_main.cpp)
target_link_libraries(hades-ask-agent PRIVATE hades_core)            # protocol + cpr (shell precedent)
target_sources(hades_tests PRIVATE tests/test_ask_agent_tool.cpp)
target_compile_definitions(hades_tests PRIVATE ASK_AGENT_BIN="$<TARGET_FILE:hades-ask-agent>")
add_dependencies(hades_tests hades-ask-agent)
```

- [ ] **Step 3: Implement the registry/runner timeout.** In `include/hades/tool/registry.h`, extend `ToolEntry`:

```cpp
struct ToolEntry {
  std::string name;     // the Tool block name (config-side handle)
  std::string kind;     // "native" | "mcp"
  std::string command;  // argv string (split on whitespace at spawn time)
  double timeout_s = 0.0;  // per-tool subprocess cap; 0 -> the ToolRunner default (30s)
};
```

In `src/tool/registry.cpp`, replace the whole `add_from_block` (it currently pushes a 3-field aggregate):

```cpp
void ToolRegistry::add_from_block(const Block& b) {
  ToolEntry e;
  e.name = b.name;
  if (b.kv.count("native")) { e.kind = "native"; e.command = b.kv.at("native"); }
  else if (b.kv.count("mcp")) { e.kind = "mcp"; e.command = b.kv.at("mcp"); }
  else return;                         // unchanged behavior: a block with neither is ignored
  if (b.kv.count("timeout_s"))
    set_pos_double_on_string(b.kv.at("timeout_s"), e.timeout_s);
  tools_.push_back(std::move(e));
}
```

(`set_pos_double_on_string` comes from `hades/config.h`, already included via `registry.h`.)

In `src/module/tool_runner.cpp`, inside the TOOL_REQUEST handler, compute the effective timeout once and use it for both kinds:

```cpp
    // Per-tool override (Tool block timeout_s, e.g. ask_agent's long peer-call window);
    // 0 -> the runner-wide default.
    const double timeout = (te && te->timeout_s > 0.0) ? te->timeout_s : timeout_s_;
```

and replace the two `timeout_s_` uses in the dispatch (`run_subprocess(..., timeout_s_)` and `mcp_call(..., timeout_s_)`) with `timeout`.

- [ ] **Step 4: Implement the tool.** `tools/ask_agent_main.cpp`:

```cpp
// tools/ask_agent_main.cpp — bundled ask_agent native tool binary (outbound delegation)
//
// Reads one JSON line ({"call":"describe"|"ask_agent","args":{peer,message}}) and writes one
// JSON line. POSTs a v1 bridge /ask to the named peer and returns the peer's reply as the
// tool result — the receiving agent runs the request through its OWN Arbiter/objectives.
// Argv (appended by wiring; single source of truth): <own_name> <secret_env> <timeout_s>
// <name=url>... The shared secret is resolved from the NAMED env var here (never argv/
// manifest). hops is always 0 in v1 (multi-hop is a v2 wire seam); the PeerLoopGuard on the
// asking side is what actually prevents relay loops. Fail-closed: malformed input, unknown
// peer, missing secret, transport failure -> ok:false, never throws.
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <nlohmann/json.hpp>
#include <cpr/cpr.h>
#include "hades/bridge/protocol.h"

int main(int argc, char** argv) {
  const std::string own_name = argc > 1 ? argv[1] : "hades";
  const std::string secret_env = argc > 2 ? argv[2] : "HADES_BRIDGE_SECRET";
  double timeout_s = 180.0;
  if (argc > 3) {
    try { timeout_s = std::stod(argv[3]); } catch (const std::exception&) {}
    if (timeout_s <= 0) timeout_s = 180.0;
  }
  std::map<std::string, std::string> peers;   // name -> base url
  for (int i = 4; i < argc; ++i) {
    const std::string pair = argv[i];
    const auto eq = pair.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 >= pair.size()) continue;
    peers[pair.substr(0, eq)] = pair.substr(eq + 1);
  }

  std::string line;
  std::getline(std::cin, line);
  auto in = nlohmann::json::parse(line, nullptr, false);

  nlohmann::json out;
  std::string call;
  if (in.is_object() && in.contains("call") && in["call"].is_string())
    call = in["call"].get<std::string>();

  if (call == "describe") {
    std::string roster;
    for (const auto& [name, url] : peers) roster += (roster.empty() ? "" : ", ") + name;
    out = {{"ok", true},
           {"result",
            {{"name", "ask_agent"},
             {"description",
              "Ask a peer hades agent a question and get its answer. The peer runs your "
              "request through its own tools and safety gates. Known peers: " +
                  (roster.empty() ? std::string("(none configured)") : roster) +
                  ". peer: the peer's name; message: what you want it to do or answer."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"peer", {{"type", "string"}}}, {"message", {{"type", "string"}}}}},
               {"required", {"peer", "message"}}}}}}};
  } else if (call == "ask_agent") {
    nlohmann::json args = (in.is_object() && in.contains("args") && in["args"].is_object())
                              ? in["args"]
                              : nlohmann::json::object();
    auto str = [&](const char* k) {
      return args.contains(k) && args[k].is_string() ? args[k].get<std::string>()
                                                     : std::string{};
    };
    const std::string peer = str("peer");
    const std::string message = str("message");
    const char* secret = std::getenv(secret_env.c_str());
    if (peer.empty() || message.empty()) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: peer and message required"}}}};
    } else if (!peers.count(peer)) {
      out = {{"ok", false}, {"result", {{"error", "unknown peer: " + peer}}}};
    } else if (!secret || !*secret) {
      out = {{"ok", false},
             {"result", {{"error", "bridge secret env var not set: " + secret_env}}}};
    } else {
      const std::string body = hades::build_ask(own_name, 0, message).dump();
      cpr::Response r = cpr::Post(
          cpr::Url{peers.at(peer) + "/ask"}, cpr::Body{body},
          cpr::Header{{"Content-Type", "application/json"}, {"X-Hades-Bridge", secret}},
          cpr::Timeout{static_cast<long>(timeout_s * 1000)}, cpr::Redirect{false});
      auto resp = nlohmann::json::parse(r.text, nullptr, false);
      if (r.status_code == 200 && resp.is_object() && resp.value("ok", false)) {
        out = {{"ok", true},
               {"result", {{"peer", peer}, {"reply", resp.value("reply", "")}}}};
      } else if (r.status_code == 0) {
        out = {{"ok", false},
               {"result", {{"error", "peer unreachable: " + peer + " (connect/timeout)"}}}};
      } else {
        const std::string why =
            resp.is_object() ? resp.value("error", "") : std::string("bad response");
        out = {{"ok", false},
               {"result",
                {{"error", "peer " + peer + " refused (" + std::to_string(r.status_code) +
                               "): " + why}}}};
      }
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }
  std::cout << out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << std::endl;
  return 0;
}
```

- [ ] **Step 5: Build + test.** `-R "AskAgentTool|ToolRegistry"` → pass; full suite green.
- [ ] **Step 6: Commit.**

```bash
git add tools/ask_agent_main.cpp tests/test_ask_agent_tool.cpp tests/test_toolrunner.cpp include/hades/tool/registry.h src/tool/registry.cpp src/module/tool_runner.cpp CMakeLists.txt
git commit -m "feat: ask_agent native tool + per-tool timeout override (Tool block timeout_s)"
```

---

## Task 7: Wiring + hades_main + two-agent e2e

**Files:**
- Modify: `app/agent_wiring.h`, `app/agent_wiring.cpp`, `app/hades_main.cpp`
- Test: `tests/test_bridge_wiring.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: everything above.
- Produces: `Agent::bridge` (`std::unique_ptr<BridgeModule>`, declared AFTER `executor` and BEFORE `telegram`); factory `"bridge"`; `Bridge` + `Peer` manifest blocks; `wire_agent` gains trailing params `const Block& bridge_cfg = Block{}` and `const std::vector<Block>& peer_blocks = {}`; ask_agent argv append + `timeout_s` injection; PeerLoopGuard auto-registration; MalConfig rules; hades_main: bridge secret redaction, `start_listening()` after wiring, bridge-only `wait()` tail.

**Wiring rules (all in `wire_agent`, fail-fast before side effects):**
1. Resolve `peers`: for each Peer block — `valid_peer_name(b.name)` else MalConfig; `url` key present + non-empty + `reject_ws` else MalConfig; duplicate name → MalConfig.
2. `ask_agent` Tool present ⇒ requires ≥1 Peer block AND a `Bridge { name }` (valid) — else MalConfig. (The Bridge BLOCK is the agent's identity; the bridge MODULE is only the listener — an ask-only agent has the block but not the module.)
3. `a.bridge` present ⇒ Bridge block `name` required (module's own on_start throws, but wire_agent must pass the block through).
4. ask_agent argv append: `t.kv["native"] += " " + bridge_name + " " + secret_env + " " + fmt(ask_timeout_s) + " " + "n1=u1" + ...` (peers sorted by name for determinism) and `t.kv["timeout_s"] = fmt(ask_timeout_s + 10)` (ToolRunner cap > the tool's inner HTTP timeout, so the tool reports its own timeout instead of being killed).
5. `if (a.bridge) a.arbiter->add_objective(std::make_unique<PeerLoopGuard>())` BEFORE the manifest-objectives loop (first hard-veto wins).
6. Bridge module wiring (a new step between serve and telegram): `set_turn_gate(a.gate.get())`, `on_start(bridge_cfg, bb)`, `set_peers(peers)`, `if (a.executor) set_executor(...)`, `on_attach(bb)`.
7. Manifest overload: `launcher.register_factory("bridge", ...)`, `a.bridge = take_as<BridgeModule>(launcher, "bridge")` — taken but **assigned to the member declared after executor**; extract `m.of("Bridge")` / `m.of("Peer")`; pass both to wire_agent; `if (a.bridge) a.bridge->set_turn_timeout_s(turn_idle_timeout_s)` next to the other front-ends.

- [ ] **Step 1: Write the failing tests** `tests/test_bridge_wiring.cpp`:

```cpp
// tests/test_bridge_wiring.cpp — manifest wiring + the two-agent e2e over real sockets.
// The e2e builds TWO full manifest agents in one process (no llm module: the "brain" is an
// echo subscriber, LLM_REQUEST is never consumed) and drives A's real ask_agent binary
// through A's ToolRunner at B's real bridge listener.
#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
using namespace hades;

namespace {
std::string bridge_manifest(const std::string& name, const std::string& peer_name,
                            const std::string& peer_url) {
  return std::string("Session\n{\n  model = m\n}\n") +
         "Module = tool_runner\n" +
         "Module = arbiter\n" +
         "Module = bridge\n" +
         "Bridge\n{\n  name = " + name + "\n  port = 0\n}\n" +
         "Peer = " + peer_name + " { url = " + peer_url + " }\n" +
         "Tool = ask_agent { native = " + ASK_AGENT_BIN + " }\n";
}
}  // namespace

TEST(BridgeWiring, BridgeModuleWithoutNameThrows) {
  ::setenv("HADES_BRIDGE_SECRET", "s3cret", 1);
  Blackboard bb;
  Manifest m = parse_manifest(
      "Session\n{\n  model = m\n}\nModule = arbiter\nModule = bridge\n"
      "Peer = x { url = http://127.0.0.1:1 }\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(BridgeWiring, AskAgentToolWithoutPeersThrows) {
  Blackboard bb;
  Manifest m = parse_manifest(
      std::string("Session\n{\n  model = m\n}\nModule = tool_runner\nModule = arbiter\n") +
      "Bridge\n{\n  name = solo\n}\n" +
      "Tool = ask_agent { native = " + ASK_AGENT_BIN + " }\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(BridgeWiring, AskAgentToolWithoutBridgeNameThrows) {
  Blackboard bb;
  Manifest m = parse_manifest(
      std::string("Session\n{\n  model = m\n}\nModule = tool_runner\nModule = arbiter\n") +
      "Peer = x { url = http://127.0.0.1:1 }\n" +
      "Tool = ask_agent { native = " + ASK_AGENT_BIN + " }\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(BridgeWiring, BadPeerBlocksThrow) {
  Blackboard bb;
  // duplicate peer name
  EXPECT_THROW(build_agent(bb, parse_manifest(
      "Session\n{\n  model = m\n}\nModule = arbiter\n"
      "Peer = x { url = http://a:1 }\nPeer = x { url = http://b:1 }\n")), MalConfig);
  // missing url
  EXPECT_THROW(build_agent(bb, parse_manifest(
      "Session\n{\n  model = m\n}\nModule = arbiter\nPeer = x { nope = 1 }\n")), MalConfig);
}

TEST(BridgeWiring, NoBridgeRosterLeavesAgentBridgeNull) {
  Blackboard bb;
  Manifest m = parse_manifest("Session\n{\n  model = m\n}\nModule = arbiter\n");
  Agent agent = build_agent(bb, m);
  EXPECT_EQ(agent.bridge, nullptr);
}

TEST(BridgeWiring, TwoAgentAskEndToEnd) {
  ::setenv("HADES_BRIDGE_SECRET", "s3cret", 1);

  // Agent B ("worker1"): real bridge listener; its "brain" echoes USER_MESSAGE.
  Blackboard bb_b;
  Agent b = build_agent(bb_b, parse_manifest(
      bridge_manifest("worker1", "front", "http://127.0.0.1:1")));
  ASSERT_NE(b.bridge, nullptr);
  bb_b.subscribe("USER_MESSAGE", [&](const Entry& e) {
    bb_b.post("ASSISTANT_MESSAGE", "B says: " + e.value.get<std::string>(), "t");
  });
  const int b_port = b.bridge->start_listening();
  ASSERT_GT(b_port, 0);

  // Agent A ("front"): knows B by its real port; drive A's REAL ask_agent tool binary
  // through A's ToolRunner (the argv was assembled by wiring — this is what the e2e pins).
  Blackboard bb_a;
  Agent a = build_agent(bb_a, parse_manifest(
      bridge_manifest("front", "worker1", "http://127.0.0.1:" + std::to_string(b_port))));
  nlohmann::json result;
  bb_a.subscribe("TOOL_RESULT", [&](const Entry& e) { result = e.value; });
  bb_a.post("TOOL_REQUEST",
            {{"id", "e2e1"},
             {"tool", "ask_agent"},
             {"args", {{"peer", "worker1"}, {"message", "status?"}}}},
            "arbiter");
  bb_a.pump();
  ASSERT_TRUE(result.is_object());
  EXPECT_TRUE(result.value("ok", false)) << result.dump();
  EXPECT_EQ(result["content"].value("reply", ""),
            "B says: (from peer agent \"front\") status?");
}

TEST(BridgeWiring, PeerTurnCannotAskOnward) {
  ::setenv("HADES_BRIDGE_SECRET", "s3cret", 1);
  Blackboard bb;
  Agent agent = build_agent(bb, parse_manifest(
      bridge_manifest("worker1", "front", "http://127.0.0.1:1")));
  // Simulate a peer-driven turn, then ask the ARBITER to dispatch ask_agent: the
  // auto-registered PeerLoopGuard must hard-veto it (no confirm, no subprocess spawn).
  bb.post("TURN_ORIGIN", "peer:front", "bridge");
  nlohmann::json result;
  bool confirm = false;
  bb.subscribe("CONFIRM_REQUEST", [&](const Entry&) { confirm = true; });
  bb.subscribe("ASSISTANT_MESSAGE", [&](const Entry& e) { result = e.value; });
  // LLM_RESPONSE with a tool call is the Arbiter's dispatch entry (no llm module rostered:
  // post it directly, the shape the LLMModule would produce).
  bb.post("USER_MESSAGE", "(from peer agent \"front\") do it", "bridge");
  bb.pump();
  auto req = bb.get("LLM_REQUEST");
  ASSERT_TRUE(req.has_value());   // the turn started
  bb.post("LLM_RESPONSE",
          {{"epoch", req->value.value("epoch", 0)},
           {"tool_calls",
            {{{"id", "t1"},
              {"name", "ask_agent"},
              {"args", {{"peer", "front"}, {"message", "loop!"}}}}}}},
          "test");
  bb.pump();
  EXPECT_FALSE(confirm);                        // hard veto, not confirm-gated
  ASSERT_TRUE(result.is_string());
  EXPECT_NE(result.get<std::string>().find("loop guard"), std::string::npos);
}
```

**Note for the implementer on the last test:** the exact `LLM_RESPONSE` shape (field names for tool calls, the epoch echo) must match what `src/arbiter/arbiter.cpp` consumes — check `tests/test_arbiter.cpp` for the established scripted shape and mirror it exactly (including the blocked-action `ASSISTANT_MESSAGE` text the Arbiter emits on a hard veto — adjust the final `EXPECT_NE` substring to the Arbiter's real wording if it differs).

- [ ] **Step 2: CMake + run — expect FAIL.** After the `tests/test_telegram_wiring.cpp` line add:

```cmake
target_sources(hades_tests PRIVATE tests/test_bridge_wiring.cpp)
```

(`ASK_AGENT_BIN` is already defined from Task 6.) Expect compile FAIL: `Agent` has no member `bridge`.

- [ ] **Step 3: Implement — `app/agent_wiring.h`.** Add `#include "hades/module/bridge_module.h"` with the other module includes. In `struct Agent`, between `executor` and `telegram`:

```cpp
  // Agent↔agent bridge (listener thread + share push). Declared AFTER executor and BEFORE
  // telegram: destroyed second (after telegram, before executor), so its dtor stop+joins the
  // listener while the Executor and every module an in-flight /ask turn touches are still
  // alive. Do NOT reorder (see the executor/telegram comments).
  std::unique_ptr<BridgeModule> bridge;
```

- [ ] **Step 4: Implement — `app/agent_wiring.cpp`.**

Add includes: `#include "hades/objective/peer_loop_guard.h"`, `#include "hades/bridge/protocol.h"` (valid_peer_name), `#include "hades/timeouts.h"` is already there.

Extend `wire_agent`'s signature with two trailing params (after `telegram_cfg`):

```cpp
                const Block& bridge_cfg = Block{},
                const std::vector<Block>& peer_blocks = {}) {
```

Inside `wire_agent`, after the skills-dir resolution block, add the bridge resolution:

```cpp
  // Bridge identity + peer roster. The Bridge BLOCK is the agent's bridge identity (name/
  // secret/timeout) — needed by the ask_agent tool even without the listener module; the
  // bridge MODULE is only the inbound listener. Fail fast on a mis-wiring, BEFORE any
  // on_start side effects.
  std::map<std::string, std::string> peers;
  for (const auto& p : peer_blocks) {
    if (!valid_peer_name(p.name))
      throw MalConfig("Peer block has an invalid name: " + p.name);
    if (!p.kv.count("url") || p.kv.at("url").empty())
      throw MalConfig("Peer " + p.name + " requires a url");
    reject_ws(p.kv.at("url"), "peer url");
    if (!peers.emplace(p.name, p.kv.at("url")).second)
      throw MalConfig("duplicate Peer block: " + p.name);
  }
  const std::string bridge_name =
      bridge_cfg.kv.count("name") ? bridge_cfg.kv.at("name") : "";
  const std::string bridge_secret_env =
      bridge_cfg.kv.count("secret_env") ? bridge_cfg.kv.at("secret_env") : "HADES_BRIDGE_SECRET";
  double ask_timeout_s = kDefaultAskTimeoutS;
  if (bridge_cfg.kv.count("ask_timeout_s"))
    set_pos_double_on_string(bridge_cfg.kv.at("ask_timeout_s"), ask_timeout_s);
  bool has_ask_agent = false;
  for (const auto& t : tools) if (t.name == "ask_agent") has_ask_agent = true;
  if (has_ask_agent) {
    if (peers.empty())
      throw MalConfig("ask_agent tool requires at least one Peer block (nobody to call)");
    if (!valid_peer_name(bridge_name))
      throw MalConfig("ask_agent tool requires Bridge { name } (the agent's own peer name)");
  }
```

In the `tools_resolved` loop, add a branch after the skills one:

```cpp
    else if (t.name == "ask_agent" && t.kv.count("native")) {
      // argv: <own_name> <secret_env> <timeout_s> <name=url>... (peers sorted — std::map).
      auto fmt = [](double d) { std::ostringstream o; o << d; return o.str(); };
      std::string argv_tail = " " + bridge_name + " " + bridge_secret_env + " " +
                              fmt(ask_timeout_s);
      for (const auto& [pname, purl] : peers) argv_tail += " " + pname + "=" + purl;
      t.kv["native"] = t.kv["native"] + argv_tail;
      // ToolRunner cap ABOVE the tool's inner HTTP timeout: the tool reports its own timeout
      // error instead of being killed mid-write (single source: Bridge.ask_timeout_s).
      t.kv["timeout_s"] = fmt(ask_timeout_s + 10);
    }
```

In the Arbiter step (3), immediately BEFORE the `for (const auto& ob : objectives)` loop:

```cpp
    // The bridge brings its OWN safety behavior (not manifest-optional): a peer-driven turn
    // must never ask_agent onward — the A<->B mutual-wait deadlock. Registered FIRST so its
    // hard veto short-circuits before any manifest objective.
    if (a.bridge) a.arbiter->add_objective(std::make_unique<PeerLoopGuard>());
```

After the serve step (5) and before the telegram step (6), add:

```cpp
  // 5b) Bridge: inbound peer front-end + outbound share push. Gate BEFORE on_attach (it
  //     drives whole turns like the other front-ends); peers BEFORE on_attach (the allowlist
  //     must exist before any request can arrive — the listener itself starts later, in
  //     hades_main); executor for the share-push offload. on_start throws MalConfig on a
  //     missing name / unset secret env.
  if (a.bridge) {
    a.bridge->set_turn_gate(a.gate.get());
    a.bridge->on_start(bridge_cfg, bb);
    a.bridge->set_peers(peers);
    if (a.executor) a.bridge->set_executor(a.executor.get());
    a.bridge->on_attach(bb);
  }
```

In the Manifest overload: register the factory with the others:

```cpp
  launcher.register_factory("bridge",      []{ return std::make_unique<BridgeModule>(); });
```

take it with the others:

```cpp
  a.bridge  = take_as<BridgeModule>(launcher, "bridge");
```

extract the blocks next to the Telegram one:

```cpp
  const auto bridge_blocks = m.of("Bridge");
  const Block bridge_cfg = bridge_blocks.empty() ? Block{} : bridge_blocks.front();
  const auto peer_blocks = m.of("Peer");
```

pass them as the new trailing args of the `wire_agent` call:

```cpp
  wire_agent(a, bb, s, m.of("Tool"), m.of("Objective"), memory, model, embedding, session_path,
             skills_cfg, telegram_cfg, bridge_cfg, peer_blocks);
```

and apply the idle ceiling next to the other front-ends:

```cpp
  if (a.bridge) a.bridge->set_turn_timeout_s(turn_idle_timeout_s);
```

**Note:** the `a.executor` creation currently happens AFTER the `take_as` block and BEFORE `wire_agent` — the bridge needs it inside `wire_agent` exactly like the embedding module, and that ordering already holds. Do not move it.

The TEST overload's `wire_agent(...)` call is unchanged (defaults: empty bridge_cfg/peers; `a.bridge` stays null like `a.embedding`).

- [ ] **Step 5: Implement — `app/hades_main.cpp`.**

After the Telegram-token redaction block, add:

```cpp
    // Redact the bridge shared secret too (it travels in every bridge request header).
    // Best-effort: resolve the same env var the module/tool will use.
    {
      const auto br = manifest.of("Bridge");
      std::string br_env = "HADES_BRIDGE_SECRET";
      if (!br.empty() && br.front().kv.count("secret_env")) br_env = br.front().kv.at("secret_env");
      if (const char* br_secret = std::getenv(br_env.c_str()); br_secret && *br_secret)
        eventlog.add_redaction(br_secret);
    }
```

After the `agent.telegram->start_polling()` line:

```cpp
    // Bridge listener: started AFTER the full graph is wired (same rule as the telegram poll
    // thread — no surprise threads in tests). Peer turns serialize through the shared TurnGate.
    if (agent.bridge) {
      const int p = agent.bridge->start_listening();
      if (p < 0) { std::cerr << "hades: bridge failed to bind its port\n"; return 1; }
      std::cerr << "hades: bridge \"" << agent.bridge->name() << "\" listening on port "
                << p << "\n";
    }
```

Extend the front-end tail: after the `else if (agent.telegram)` branch, add:

```cpp
    } else if (agent.bridge) {
      std::cerr << "hades: bridge-only roster — serving peers (Ctrl-C to exit)\n";
      agent.bridge->wait();                                  // blocks on the listener thread
```

(keeping the final `else` error branch last).

- [ ] **Step 6: Build + test.** `-R BridgeWiring` → pass; **full suite** green (existing wiring/pantler tests untouched).
- [ ] **Step 7: Commit.**

```bash
git add app/agent_wiring.h app/agent_wiring.cpp app/hades_main.cpp tests/test_bridge_wiring.cpp CMakeLists.txt
git commit -m "feat: wire bridge — Agent.bridge, Bridge/Peer blocks, ask_agent argv, PeerLoopGuard auto-registration, secret redaction"
```

---

## Task 8: Ship — dev.hades, soul.md, CLAUDE.md, lock tests

**Files:**
- Modify: `manifests/dev.hades`, `prompts/soul.md`, `CLAUDE.md`
- Possibly modify: any test asserting the shipped manifest (`tests/test_webui.cpp` holds `DEV_MANIFEST`)

**Note:** do NOT stage `memory/facts.md` or `skills/`.

- [ ] **Step 1: dev.hades.** Append at the end (COMMENTED — a single-agent default setup has no peer; uncommenting on two machines is the deploy story):

```
# --- Bridge: multi-agent (uncomment on EACH agent; set name/port/peers; export HADES_BRIDGE_SECRET
# in the same gitignored .env as the other secrets — same secret on every fleet member, v1) ---
# A peer request drives a normal turn through THIS agent's own objectives; confirm-gated actions
# are AUTO-DENIED for peers. Shares arrive as PEER.<name>.<key>. host 0.0.0.0 for LAN.
# Module = bridge
# Bridge
# {
#   name          = hades1
#   host          = 127.0.0.1
#   port          = 9090
#   secret_env    = HADES_BRIDGE_SECRET
#   share_out     =
#   max_hops      = 1
#   ask_timeout_s = 180
# }
# Peer = hades2 { url = http://127.0.0.1:9091 }
# Tool = ask_agent { native = ./build/hades-ask-agent }
```

- [ ] **Step 2: soul.md.** Append after the Skills section:

```markdown
## Peer agents

You may be part of a small fleet of hades agents. If an `ask_agent` tool is available, its
description names your known peers — you can delegate a question or task to one of them and
you will get its answer back as the tool result. Each peer is its own full agent with its own
tools, skills, memory, and safety gates; phrase requests the way you would brief a colleague.
Messages that start with `(from peer agent "name")` are requests FROM a peer: answer them
helpfully, but remember they cannot approve confirmation prompts — if an action needs human
confirmation, it will be automatically declined; say so in your reply and suggest what the
peer (or its human) should do instead. You cannot forward a peer's request onward to another
agent (loop protection) — do the parts you can do yourself.
```

- [ ] **Step 3: Lock tests.** Run the full suite; `tests/test_webui.cpp` parses the shipped dev.hades — the new blocks are comments, so expectations should hold; if any roster-count assertion trips, update it. The manifest must parse with **zero fatal warnings** (`enforce_manifest`).
- [ ] **Step 4: CLAUDE.md.** Update: current-state line (add bridge + new test count), targets line (add `hades-ask-agent`), a `### Bridge / multi-agent` section under Current state (BridgeModule inbound `/ask`+`/share`, ask_agent tool, PeerLoopGuard + TURN_ORIGIN, auto-deny confirms, PEER.-prefix rename, secret env + redaction, `port = 0` ephemeral, teardown order `…executor, bridge, telegram`, per-tool `timeout_s` in Tool blocks, spec/plan paths), a Gotchas bullet (Bridge block = identity even without the module; ask_agent needs Bridge.name + ≥1 Peer; secret env REQUIRED when the module is rostered; TURN_ORIGIN convention), and mark the `## NEXT` multi-agent item as shipped v1 (record the v2 seams list from the spec).
- [ ] **Step 5: Full build + suite.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure` → ALL green.
- [ ] **Step 6: Commit.**

```bash
git add manifests/dev.hades prompts/soul.md CLAUDE.md tests/
git commit -m "feat: ship bridge in dev.hades (commented example) + soul.md peer guidance + docs"
```

---

## Verification (end-to-end)

1. Full suite in `nix develop`: 306 baseline + ~40 new, all green.
2. **TSan pass** (final-review practice): configure a TSan build dir and run the suite — the new listener thread × TurnGate × Executor surfaces are exactly what it exists for.
3. Manual live smoke (Vaios, two terminals, same machine):
   ```bash
   # terminal 1 — worker (edit a copy of dev.hades: name=hades2, port=9091, Peer = hades1 { url = http://127.0.0.1:9090 })
   export HADES_BRIDGE_SECRET=$(openssl rand -hex 16)   # same value in both shells
   nix develop --command ./build/hades manifests/worker.hades
   # terminal 2 — front (name=hades1, port=9090, Peer = hades2 { url = http://127.0.0.1:9091 })
   nix develop --command ./build/hades manifests/dev.hades
   # user> ask hades2 what time it is
   #   -> agent calls ask_agent; terminal 1 shows the peer turn; reply arrives in terminal 2
   ```
4. Security spot-checks: `curl -s -X POST http://127.0.0.1:9090/ask -d '{"v":1,"from":"hades2","hops":0,"message":"hi"}'` (no header) → 403; with the header but `from: "stranger"` → 403; confirm-band request via a peer → reply carries the auto-deny note.
5. `hades-scope session.log` — bridge secret never appears (redacted); `PEER.` keys visible on share.

## Execution

Subagent-driven development (per project process): fresh implementer per task (opus per `feedback_sdd_implementer_opus`), per-task review, final whole-branch review, then finishing-a-development-branch (merge ff to main — no remote, never push).
