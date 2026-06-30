# Configurable timeouts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. **Implementers run on OPUS**. Read current files fully first. **DISCIPLINE:** build + FULL suite + `git commit` + verify `git log` + write report before replying; do NOT report DONE unless `git log` shows your commit.

**Goal:** Make the LLM per-call timeout and the `run_until` idle timeout manifest-configurable (Session block), defaults cpr **600s** / idle **900s**, with a fail-loud invariant `turn_idle_timeout_s > llm_timeout_s`. Spec: `docs/superpowers/specs/2026-06-30-configurable-timeouts-design.md` (read first).

**Architecture:** LLMModule reads `llm_timeout_s` from its Session block → `cpr_http(llm_timeout_s)`. Wiring reads `turn_idle_timeout_s` → `set_turn_timeout_s`/`set_collect_timeout_s` on the front-ends (default constants bumped to 900). Wiring validates the invariant → `MalConfig`. The `--serve` httplib read/write timeouts are raised so a long turn's connection isn't dropped.

**Tech Stack:** C++20 · CMake + Ninja · Nix dev shell · GoogleTest. Build/test ONLY inside `nix develop`.

## Global Constraints

- **C++20**, g++ 15.2. Every command inside `nix develop`.
- **TDD:** RED → GREEN → COMMIT per task. Commit `<type>: <desc>`, **no attribution footer**.
- **Backward-compat:** absent keys → defaults (600 / 900). The 169 existing tests stay green (they use the small test override for the idle timeout; none depends on the old 180 default). The test `build_agent` overload (no Manifest) is unaffected by the new validation.
- **Defaults in ONE place:** `kDefaultLlmTimeoutS = 600.0`, `kDefaultTurnIdleTimeoutS = 900.0` as named constants shared by LLMModule + wiring (avoid drift). Put them where both can include (e.g. a small `include/hades/timeouts.h`, or reuse an existing config header — implementer's call, but single-source).

## File Structure

```
include/hades/timeouts.h            T1 (new, optional)  kDefaultLlmTimeoutS / kDefaultTurnIdleTimeoutS
src/module/llm_module.cpp           T1 (modify)  read llm_timeout_s -> cpr_http(llm_timeout_s); store for test
include/hades/module/llm_module.h   T1 (modify)  member + getter for the resolved timeout (test observability)
include/hades/llm/http.h            T1 (modify)  default 120 -> kDefaultLlmTimeoutS (or doc only)
src/module/chat_module.cpp          T1 (modify)  kTurnTimeoutS 180 -> 900 (use the default const)
src/module/http_server_module.cpp   T1 (modify)  kCollectTimeoutS 180 -> 900
app/agent_wiring.cpp                T1 (modify)  read turn_idle_timeout_s + set on front-ends; validate invariant -> MalConfig
manifests/dev.hades                 T1 (modify)  add llm_timeout_s=600, turn_idle_timeout_s=900 to Session
src/module/http_server_module.cpp   T2 (modify)  srv.set_read_timeout/set_write_timeout(idle+60)
tests/test_llm_module.cpp           T1 (extend)
tests/test_pantler_wiring.cpp       T1 (extend)
tests/test_serve.cpp                T2 (extend, optional)
```

---

## Task 1: Manifest-configurable timeouts + fail-loud invariant

**Files:** as above (T1). **Read first:** `src/module/llm_module.cpp` (`on_start` reads endpoint/model/api_key_env/price_per_mtok; builds `cpr_http()`), `include/hades/module/llm_module.h`, `include/hades/llm/http.h` (`cpr_http(double=120.0)`), `src/module/chat_module.cpp` + `src/module/http_server_module.cpp` (`kTurnTimeoutS`/`kCollectTimeoutS`=180, `effective_timeout_()`, the `set_*_timeout_s` seam), `app/agent_wiring.cpp` (`build_agent(bb, Manifest)`, how the Session block is read, where `MalConfig` is thrown, how front-end modules are configured/attached), `include/hades/config.h` (`set_pos_double_on_string`), `manifests/dev.hades`.

**Interfaces — Produces:** `llm_timeout_s` flows to cpr; `turn_idle_timeout_s` flows to both front-ends; `MalConfig` if idle ≤ llm.

- [ ] **Step 1: Failing tests.**
  - `tests/test_llm_module.cpp` — `ResolvesLlmTimeoutFromSession`: `Block cfg; cfg.kv["llm_timeout_s"]="5";` (+ the injected-provider ctor is NOT used here — we need the on_start path; but on_start builds a real provider needing an api key env. Approach: add a getter `double resolved_llm_timeout_s() const` to LLMModule, set in on_start BEFORE the provider build, and assert it. To avoid the api-key requirement in the test, EITHER set the env var in the test OR resolve+store the timeout before the key check and assert via the getter on a module whose on_start you call with the env set. Pick the cleanest; the assertion is `resolved_llm_timeout_s()==5.0`, and absent key → `==600.0`.)
  - `tests/test_pantler_wiring.cpp` — `IdleTimeoutFromManifest`: a manifest with `turn_idle_timeout_s = 30` (and a valid `llm_timeout_s` < 30, e.g. 10) → `build_agent(bb, m)` → assert the ChatModule's effective idle is 30 (expose via a getter `double idle_timeout_s() const` on ChatModule returning `effective_timeout_()`, or assert through the existing seam). `InvalidTimeoutInvariant`: a manifest with `turn_idle_timeout_s = 60`, `llm_timeout_s = 120` → `EXPECT_THROW(build_agent(bb, m), MalConfig)`. `DefaultsWhenAbsent`: no keys → idle 900, llm 600.
- [ ] **Step 2: Run, expect FAIL** (keys unread; no getters; no invariant check).
- [ ] **Step 3: Implement.**
  - Add `include/hades/timeouts.h` with `inline constexpr double kDefaultLlmTimeoutS = 600.0; inline constexpr double kDefaultTurnIdleTimeoutS = 900.0;`.
  - `llm_module.cpp` on_start: `double t = kDefaultLlmTimeoutS; if (cfg.kv.count("llm_timeout_s")) set_pos_double_on_string(cfg.kv.at("llm_timeout_s"), t);` store `llm_timeout_s_ = t;` and build `cpr_http(t)`. Add member `double llm_timeout_s_ = kDefaultLlmTimeoutS;` + getter `double resolved_llm_timeout_s() const { return llm_timeout_s_; }`.
  - `chat_module.cpp`/`http_server_module.cpp`: change the file-local `kTurnTimeoutS`/`kCollectTimeoutS` to `kDefaultTurnIdleTimeoutS` (= 900). (effective_timeout_ unchanged: override>0 ? override : kDefault.) Add a public getter on ChatModule `double idle_timeout_s() const { return effective_timeout_(); }` for the test.
  - `agent_wiring.cpp` `build_agent(bb, Manifest)`: read the Session block → `llm_timeout_s` (default 600) + `turn_idle_timeout_s` (default 900) via `set_pos_double_on_string`. **Validate:** if `turn_idle_timeout_s <= llm_timeout_s` → `throw MalConfig("turn_idle_timeout_s (" + … + ") must be greater than llm_timeout_s (" + … + ")")`. Then if a `turn_idle_timeout_s` was configured (or always — passing 900 is harmless), call `agent.chat->set_turn_timeout_s(turn_idle_timeout_s)` and `agent.serve->set_collect_timeout_s(turn_idle_timeout_s)` (null-guard each — a roster may omit chat/serve). Do NOT set the override in the test overload.
  - Reword the "test-only" comments on `set_turn_timeout_s`/`set_collect_timeout_s` to "configured idle ceiling (0 ⇒ default); tests pass a small value."
  - `dev.hades`: add `llm_timeout_s = 600` and `turn_idle_timeout_s = 900` to the multi-line `Session` block.
  - (Optional) `http.h`: bump the `cpr_http` default param + comment to reference kDefaultLlmTimeoutS / 600.
- [ ] **Step 4: Build + test** `-R "LLMModule|Pantler|Wiring|Manifest|Serve|Chat"`, then FULL suite green (169 + new). Confirm shipped dev.hades builds (no false MalConfig — 900 > 600 ✓).
- [ ] **Step 5: Commit** `feat: manifest-configurable llm_timeout_s + turn_idle_timeout_s (defaults 600/900) with idle>llm MalConfig guard`

---

## Task 2: `--serve` survives a long (up to idle) turn

**Files:** Modify `src/module/http_server_module.cpp` (`listen`). Extend `tests/test_serve.cpp` if a seam allows (optional — this is mostly a defensive socket-timeout config). **Read `listen()` first** (the `httplib::Server srv;` setup + `srv.listen(host, port)`).

**Interfaces — Produces:** the httplib server's read/write socket timeouts exceed the idle ceiling so a long `collect_` handler's connection isn't dropped.

- [ ] **Step 1: Failing/again test (light).** A pure unit test of httplib socket timeouts is awkward; if there's no clean seam, make this a code change verified by inspection + the existing serve tests staying green, and state that in the report. If feasible, assert that `listen` sets the timeouts (e.g. factor the server-config into a small testable helper `configure_server_(httplib::Server&, double idle)` and assert it set read/write timeouts > idle). Prefer the helper so there IS a test.
- [ ] **Step 2–3: Implement.** In `listen` (or the new `configure_server_` helper), before `srv.listen`: `srv.set_read_timeout(static_cast<time_t>(idle_s) + 60, 0); srv.set_write_timeout(static_cast<time_t>(idle_s) + 60, 0);` where `idle_s` is the module's effective collect timeout (the configured/`kDefaultTurnIdleTimeoutS` value — read the member, not the bare constant, so it tracks the manifest). Comment: brackets socket I/O; raised so a long collect_ handler isn't cut off. Leave keep-alive default.
- [ ] **Step 4: Build + FULL suite green.**
- [ ] **Step 5: Commit** `feat: raise --serve httplib read/write timeouts above the idle ceiling (long turns survive)`

---

## Self-Review (against the spec)

- **Coverage:** llm_timeout_s→cpr (T1); turn_idle_timeout_s→front-ends (T1); idle>llm MalConfig (T1); defaults single-sourced (T1); dev.hades keys (T1); serve survives long turn (T2).
- **Invariant enforced** fail-loud at the live boundary; test overload unaffected.
- **Backward-compat:** defaults 600/900; existing tests green (small override still used by them).
- **Type consistency:** timeouts are `double` seconds; `set_pos_double_on_string` validates; constants in `timeouts.h`.

## Verification

1. Full suite green (new config + invariant tests). 2. `dev.hades` builds (900>600). 3. Manual (needs key): set `llm_timeout_s=600`/`turn_idle_timeout_s=900`, run a slow task → it is NOT abandoned at 180s; set `turn_idle_timeout_s=60 llm_timeout_s=120` → binary refuses to start (MalConfig). 4. `--serve` a long turn → browser receives the answer (connection not dropped).
