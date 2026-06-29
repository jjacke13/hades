# Launcher / pAntler Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Make the manifest's `Module =` lines actually drive which modules run, via the `Launcher` (pAntler) — instead of the binary hard-coding its module set.

**Architecture:** The `Launcher` instantiates the `Module =` roster via registered factories (MalConfig on unknown type), then `agent_wiring` moves the present instances into the `Agent` and runs the existing config + cross-wire + attach logic guarded on presence (dependency order preserved). Test path is unchanged (direct build, injected provider).

**Tech Stack:** C++20 · CMake + Ninja · Nix dev shell · GoogleTest.

## Global Constraints

- **C++20**, g++ 15.2. **Every build/test command runs inside `nix develop`**.
- **TDD:** RED → GREEN → COMMIT per task. Commit `<type>: <desc>`, **no attribution footer**.
- **Preserve existing wiring exactly** where modules are present: ToolRunner warmed before the Arbiter pulls specs; MemoryModule attached BEFORE the Arbiter; the `save_memory`/`pin_fact` tool-path appends + whitespace guards + the `pin_fact`-requires-`memory_file` rule; the CSRF/serve wiring. Ordering is enforced by the call sequence, NOT by roster order.
- Modules absent from the roster → their `Agent` member is `nullptr`; all wiring/use is null-guarded.

---

## File Structure

```
include/hades/launcher.h        T1 (modify)  + instantiate / has / take
src/core/launcher.cpp           T1 (modify)
app/agent_wiring.{h,cpp}        T2 (modify)  Manifest path via Launcher; wire_agent shared, null-guarded
app/hades_main.cpp              T2 (modify)  null-guard agent.serve / agent.chat
manifests/dev.hades             T2 (modify)  add Module = memory, Module = serve to the roster
tests/test_launcher.cpp         T1 (extend)
tests/test_pantler_wiring.cpp   T2 (new)
```

---

## Task 1: Launcher roster instantiation + ownership transfer

**Files:** Modify `include/hades/launcher.h`, `src/core/launcher.cpp`, `tests/test_launcher.cpp`.

**Interfaces — Produces:** `Launcher::instantiate(const Manifest&)`, `Launcher::has(type)`, `Launcher::take(type)`. Keeps `register_factory`/`build`/`modules`/`shutdown` unchanged.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_launcher.cpp` (FakeMod already defined there):

```cpp
TEST(Launcher, InstantiateBuildsRosterAndHasByType) {
  Blackboard bb; Launcher L(bb);
  L.register_factory("a", []{ return std::make_unique<FakeMod>("a"); });
  L.register_factory("b", []{ return std::make_unique<FakeMod>("b"); });
  L.instantiate(parse_manifest("Module = a\nModule = b\n"));
  EXPECT_TRUE(L.has("a"));
  EXPECT_TRUE(L.has("b"));
  EXPECT_FALSE(L.has("c"));
}
TEST(Launcher, TakeTransfersOwnershipOnceByType) {
  Blackboard bb; Launcher L(bb);
  L.register_factory("a", []{ return std::make_unique<FakeMod>("a"); });
  L.instantiate(parse_manifest("Module = a\n"));
  auto m1 = L.take("a");
  ASSERT_NE(m1, nullptr);
  EXPECT_EQ(m1->type(), "a");
  EXPECT_EQ(L.take("a"), nullptr);   // already taken
  EXPECT_FALSE(L.has("a"));
}
TEST(Launcher, InstantiateUnknownTypeThrowsMalConfig) {
  Blackboard bb; Launcher L(bb);
  L.register_factory("a", []{ return std::make_unique<FakeMod>("a"); });
  EXPECT_THROW(L.instantiate(parse_manifest("Module = ghost\n")), MalConfig);
}
```

- [ ] **Step 2: Run, expect FAIL** — `nix develop --command cmake --build build` → `instantiate`/`has`/`take` undeclared.

- [ ] **Step 3: Implement.**

`include/hades/launcher.h` — add to the public section (after `build`):
```cpp
  // pAntler roster: instantiate the manifest's `Module =` blocks (in order) via the
  // registered factories; throws MalConfig on an unknown type. Does NOT call on_start/
  // on_attach — the caller drives lifecycle + cross-wiring. Roster validation only.
  void instantiate(const Manifest& m);
  bool has(const std::string& type) const;                  // a module of this type was instantiated (and not yet taken)
  std::unique_ptr<Module> take(const std::string& type);     // transfer the first module of `type` out; nullptr if absent
```

`src/core/launcher.cpp` — add the definitions (after `build`):
```cpp
void Launcher::instantiate(const Manifest& m){
  try {
    for(const auto& blk : m.of("Module")){
      auto it=p_->factories.find(blk.name);
      if(it==p_->factories.end()) throw MalConfig("unknown module type: "+blk.name);
      p_->mods.push_back(it->second());
    }
  } catch(...) { p_->mods.clear(); throw; }
}
bool Launcher::has(const std::string& type) const {
  for(const auto& u : p_->mods) if(u && u->type()==type) return true;
  return false;
}
std::unique_ptr<Module> Launcher::take(const std::string& type){
  for(auto& u : p_->mods)
    if(u && u->type()==type) return std::move(u);   // leaves a null hole; has()/take() skip it
  return nullptr;
}
```

- [ ] **Step 4: Build + test** — `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R Launcher` → PASS (prior + 3 new).

- [ ] **Step 5: Commit**
```bash
git add include/hades/launcher.h src/core/launcher.cpp tests/test_launcher.cpp
git commit -m "feat: Launcher roster instantiate/has/take (pAntler roster validation)"
```

---

## Task 2: agent_wiring uses the Launcher on the live path

**Files:** Modify `app/agent_wiring.h`, `app/agent_wiring.cpp`, `app/hades_main.cpp`, `manifests/dev.hades`. Create `tests/test_pantler_wiring.cpp`. Modify `CMakeLists.txt`.

**Interfaces:**
- Consumes: `Launcher::instantiate/has/take` (T1), the existing module set + wiring.
- Produces: `build_agent(bb, m)` resolves modules from the `Module =` roster; a shared `wire_agent` that null-guards absent modules.

**Read the CURRENT `app/agent_wiring.cpp` fully before editing.** You are refactoring its `build_agent_impl` into a presence-guarded `wire_agent`, and routing the Manifest overload through the Launcher. Preserve every existing wiring detail (tool-path append for `save_memory`+`pin_fact`, `reject_ws` whitespace guards, the `pin_fact`-requires-`memory_file` MalConfig, ToolRunner-warm-before-Arbiter-specs, MemoryModule-attach-before-Arbiter, `set_memory_path`, objectives, system prompt).

- [ ] **Step 1: Write the failing test** — `tests/test_pantler_wiring.cpp`:

```cpp
// tests/test_pantler_wiring.cpp — the Module= roster drives which modules the agent builds
#include <gtest/gtest.h>
#include <memory>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/launcher.h"   // MalConfig
#include "hades/llm/provider.h"
using namespace hades;

namespace {
// Minimal manifest with the LLM env var satisfied is awkward (build_agent(Manifest) resolves a real
// provider). These tests use the Manifest overload, so set the api key env in the manifest to one we set.
const char* kFullRoster = R"(
Session
{
  provider       = openai_compat
  endpoint       = https://example.invalid/v1
  model          = test-model
  api_key_env    = HADES_TEST_KEY
  price_per_mtok = 1.0
}
Module = llm
Module = tool_runner
Module = memory
Module = chat
Module = arbiter
Module = serve
Memory { store = .hades/test_mem.jsonl  top_n = 5 }
Arbiter { policy = v1 }
)";
}  // namespace

TEST(PantlerWiring, FullRosterBuildsAllModules) {
  setenv("HADES_TEST_KEY", "x", 1);
  Blackboard bb;
  Agent a = build_agent(bb, parse_manifest(kFullRoster));
  EXPECT_NE(a.llm, nullptr);
  EXPECT_NE(a.tools, nullptr);
  EXPECT_NE(a.memory, nullptr);
  EXPECT_NE(a.arbiter, nullptr);
  EXPECT_NE(a.chat, nullptr);
  EXPECT_NE(a.serve, nullptr);
}
TEST(PantlerWiring, RosterOmittingServeYieldsNullServe) {
  setenv("HADES_TEST_KEY", "x", 1);
  std::string m = kFullRoster;
  // drop the serve line
  auto pos = m.find("Module = serve\n");
  ASSERT_NE(pos, std::string::npos);
  m.erase(pos, std::string("Module = serve\n").size());
  Blackboard bb;
  Agent a = build_agent(bb, parse_manifest(m));
  EXPECT_EQ(a.serve, nullptr);    // roster drives presence
  EXPECT_NE(a.arbiter, nullptr);  // the rest still built
}
TEST(PantlerWiring, UnknownModuleTypeThrows) {
  setenv("HADES_TEST_KEY", "x", 1);
  std::string m = kFullRoster;
  m += "\nModule = bogus\n";
  Blackboard bb;
  EXPECT_THROW(build_agent(bb, parse_manifest(m)), MalConfig);
}
```

`CMakeLists.txt` — register the test:
```cmake
target_sources(hades_tests PRIVATE tests/test_pantler_wiring.cpp)
```

- [ ] **Step 2: Run, expect FAIL** — build error / `build_agent(Manifest)` still hard-codes modules so the omit-serve + unknown-type tests fail (serve always built; unknown type ignored, not thrown).

- [ ] **Step 3: Implement the refactor.**

In `app/agent_wiring.cpp`:

**(a) Rename/refactor `build_agent_impl` → `wire_agent`** that operates on an already-populated `Agent& a` (members may be null) instead of constructing them. It takes `(Agent& a, Blackboard& bb, const Block& session, const std::vector<Block>& tools, const std::vector<Block>& objectives, const Block& memory, std::string model)`. Move the existing body in, replacing every `a.X = std::make_unique<...>()` with a **null guard** `if (a.X)` around that module's config/attach. Keep the exact order: tools (warm) → llm → memory (attach) → arbiter (specs/model/prompt/memory_path/objectives/attach) → chat (attach) → serve (attach). The `reject_ws` + `core_path`/`store_path` tool-append + the `pin_fact`-requires-`memory_file` MalConfig stay (they run when the relevant tools/blocks are present). When the Arbiter is present but the ToolRunner is absent, `set_tools({})`.

**(b) Manifest overload** `build_agent(bb, m)` — route through the Launcher:
```cpp
Agent build_agent(Blackboard& bb, const Manifest& m) {
  auto session = m.session();
  if (!session) throw MalConfig("manifest has no Session block");
  const Block& s = *session;
  const std::string model = s.kv.count("model") ? s.kv.at("model") : "";

  // pAntler: the Module= roster decides which modules exist. Factories just construct;
  // the LLM self-builds its provider from the Session block in on_start (existing path).
  Launcher launcher(bb);
  launcher.register_factory("llm",         []{ return std::make_unique<LLMModule>(); });
  launcher.register_factory("tool_runner", []{ return std::make_unique<ToolRunner>(); });
  launcher.register_factory("memory",      []{ return std::make_unique<MemoryModule>(); });
  launcher.register_factory("arbiter",     []{ return std::make_unique<Arbiter>(); });
  launcher.register_factory("chat",        []{ return std::make_unique<ChatModule>(); });
  launcher.register_factory("serve",       []{ return std::make_unique<HttpServerModule>(); });
  launcher.instantiate(m);   // MalConfig on unknown Module type

  Agent a;
  a.llm     = take_as<LLMModule>(launcher, "llm");
  a.tools   = take_as<ToolRunner>(launcher, "tool_runner");
  a.memory  = take_as<MemoryModule>(launcher, "memory");
  a.arbiter = take_as<Arbiter>(launcher, "arbiter");
  a.chat    = take_as<ChatModule>(launcher, "chat");
  a.serve   = take_as<HttpServerModule>(launcher, "serve");

  const auto mem_blocks = m.of("Memory");
  const Block memory = mem_blocks.empty() ? Block{} : mem_blocks.front();
  wire_agent(a, bb, s, m.of("Tool"), m.of("Objective"), memory, model);
  return a;
}
```
Add a small file-local helper (anonymous namespace):
```cpp
template <class T>
std::unique_ptr<T> take_as(Launcher& L, const std::string& type) {
  std::unique_ptr<Module> m = L.take(type);
  return std::unique_ptr<T>(static_cast<T*>(m.release()));  // factory key == module type(); safe
}
```
(`#include "hades/launcher.h"` is already present for MalConfig.)

**(c) Test overload** `build_agent(bb, provider, tools, objectives, model, memory, session)` — keep constructing all six modules directly into the `Agent` (injecting the provider into `LLMModule`), then call the shared `wire_agent(a, bb, session, tools, objectives, memory, model)`. (All members present, so the null guards are no-ops.)

In `app/hades_main.cpp` — null-guard the dispatch:
```cpp
    if (serve) {
      if (!agent.serve) { std::cerr << "hades: no `serve` module in the manifest Module roster\n"; return 1; }
      const ServeConfig cfg = resolve_serve_config(manifest, cli_port);
      agent.serve->listen(cfg.host, cfg.port, cfg.webroot);
    } else {
      if (!agent.chat) { std::cerr << "hades: no `chat` module in the manifest Module roster\n"; return 1; }
      agent.chat->run_repl(std::cin, std::cout);
    }
```

In `manifests/dev.hades` — complete the roster (memory before arbiter, serve last):
```
Module = llm
Module = tool_runner
Module = memory
Module = chat
Module = arbiter
Module = serve
```

- [ ] **Step 4: Build + full suite** — `nix develop --command cmake -S . -B build -G Ninja && nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure`. Expected: all green — the new pantler tests pass, and `test_e2e`/`test_memory_wiring`/`test_pin_fact_wiring`/`test_serve` (test overload, all modules present) are unaffected.

- [ ] **Step 5: Commit**
```bash
git add app/agent_wiring.h app/agent_wiring.cpp app/hades_main.cpp manifests/dev.hades tests/test_pantler_wiring.cpp CMakeLists.txt
git commit -m "feat: manifest Module= roster drives the module set via the Launcher (pAntler)"
```

---

## Self-Review (against the spec)

- **Spec coverage:** Launcher instantiate/has/take + unknown-type MalConfig (T1); manifest-driven presence via take(), null-guarded wire_agent, binary guards, completed dev.hades roster (T2). Existing wiring preserved (tool-path append, whitespace guard, pin_fact-requires-memory_file, warm-before-specs, memory-before-arbiter). Test path unchanged.
- **Honesty win:** `Module =` now drives presence + validates types; the Launcher is on the live path.
- **Out of scope honored:** no bus-decoupled config, no required-module enforcement, no roster-driven attach order.
- **Type consistency:** `instantiate`/`has`/`take`, `wire_agent`, `take_as<T>` consistent across tasks.

## Verification

1. `nix develop --command cmake -S . -B build -G Ninja && nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure` → all green.
2. `nix develop --command ./build/hades manifests/dev.hades` → REPL works (full roster); removing `Module = serve` then `--serve` → clear "no serve module" message.
