# Configurable think-time / idle timeouts — design spec

**Date:** 2026-06-30
**Status:** approved (Vaios: manifest-configurable; defaults cpr 600s / idle 900s = 10 min think / 15 min idle).
Ready for plan.

## Goal

Let an agent think longer on hard single calls without recompiling. Today two hardcoded limits bound it:
- **cpr per-call HTTP timeout = 120s** (`cpr_http(timeout_s=120.0)`, `include/hades/llm/http.h`) — the real
  cap on ONE LLM "think" (a single request that doesn't return until done).
- **`run_until` idle timeout = 180s** (`kTurnTimeoutS`/`kCollectTimeoutS`) — abandons a turn after that
  many seconds of NO bus activity. It is an IDLE timeout (resets on every event), so it does NOT cap total
  turn time; it only bounds a single silent stretch.

Make both **manifest-configurable** (Session block), with generous defaults (**cpr 600s, idle 900s**).

## The load-bearing invariant (must be enforced)

`turn_idle_timeout_s` MUST stay **>** the maximum single in-flight poster duration, i.e. **> `llm_timeout_s`**
(and > the tool subprocess timeout, 30s, which is fixed/small). This guarantees a slow-but-ALIVE call always
posts back (resetting the idle deadline) before `run_until` abandons the turn. If idle ≤ llm_timeout, a
legitimate long call could be abandoned mid-flight; the TURN_ABANDONED machinery would then (now harmlessly)
drop its eventual response, so the user would get `[timed out]` on a call that was actually working.

**Enforcement:** validate at the build/launch boundary → throw `MalConfig` with a clear message naming both
values if `turn_idle_timeout_s <= llm_timeout_s`. (Same fail-loud philosophy as the parser-fail-loud feature.)

## Components

### 1. Session-block keys (`manifests/dev.hades`, parsed by LLMModule + wiring)
Two new optional keys in the `Session` block (already multi-line — no packed-line risk):
```
Session
{
  …existing (endpoint/model/api_key_env/price_per_mtok)…
  llm_timeout_s       = 600      # per-call LLM HTTP timeout (cpr). Default 600.
  turn_idle_timeout_s = 900      # run_until idle ceiling (front-ends). Default 900. MUST be > llm_timeout_s.
}
```
Defaults defined as named constants in ONE place so LLMModule and wiring agree (e.g.
`kDefaultLlmTimeoutS = 600.0`, `kDefaultTurnIdleTimeoutS = 900.0`).

### 2. cpr per-call timeout (`src/module/llm_module.cpp` on_start)
Read `llm_timeout_s` from the Session block (default `kDefaultLlmTimeoutS`) and pass it to
`cpr_http(llm_timeout_s)` when building the provider. (Validate it's a positive double via
`set_pos_double_on_string`; on bad value, default or MalConfig per the existing config-validation style.)

### 3. Front-end idle timeout (`app/agent_wiring.cpp` + the two front-end modules)
- Bump the default constants `kTurnTimeoutS` / `kCollectTimeoutS` from 180.0 → **900.0** (the new default).
- The existing seam (`set_turn_timeout_s` / `set_collect_timeout_s`; member defaults 0.0 → use the constant)
  is GENERALIZED: it's no longer "test-only" — wiring reads `turn_idle_timeout_s` from the Session block and,
  if present, calls the setters with that value (so a manifest value overrides the 900 default). Tests still
  set small values through the same seam. Reword the "test-only" comments to "configured idle (0 ⇒ default)".

### 4. Validation (`app/agent_wiring.cpp`, Manifest path)
After reading the Session block, if `turn_idle_timeout_s <= llm_timeout_s` → throw `MalConfig`
("turn_idle_timeout_s (X) must be greater than llm_timeout_s (Y) — a slow LLM call would be abandoned
mid-flight"). Runs on the live `build_agent(bb, Manifest)` path only (the test overload is unaffected).

### 5. `--serve` survives a long turn (`src/module/http_server_module.cpp` listen)
A 10-min `collect_` handler holds the HTTP connection while `run_until` runs. cpp-httplib's read/write
timeouts bracket socket I/O (not handler duration), so a long handler is not killed by default — but set
them generously anyway (defensive + explicit): `srv.set_read_timeout(idle + 60)`, `srv.set_write_timeout(idle + 60)`
(and leave keep-alive default). The browser `fetch()` has no default timeout, so the page already waits.

## Out of scope

- Per-tool / configurable tool subprocess timeout (fixed 30s, far below any sane idle — note only).
- A hard absolute per-turn wall-clock cap (we intentionally only have an idle ceiling; a multi-step turn may
  legitimately run long).
- Streaming (SSE) — would make idle timeouts mostly moot (continuous activity); separate feature.

## Testing (TDD)

- LLMModule: an `on_start` with `llm_timeout_s = 5` builds a provider whose injected/observed cpr timeout is
  5s (inject the HttpClient factory or assert via a seam); absent key → 600 default.
- Wiring: `build_agent(bb, manifest)` with `turn_idle_timeout_s = 30` sets the ChatModule/HttpServerModule
  idle to 30; absent → 900. A manifest with `turn_idle_timeout_s <= llm_timeout_s` → `EXPECT_THROW(..., MalConfig)`.
- Shipped `dev.hades` (600/900) builds cleanly (lock test); 169 existing tests stay green (defaults preserve
  behavior except the longer ceiling, which no test depends on — they use the small override).

## MOOS-IvP framing

The idle ceiling is the helm's **watchdog**: it fires only when the community's bus goes silent (nothing
posting) for the configured window — not on a slow-but-progressing maneuver. Making it (and the per-call I/O
timeout) mission-file parameters lets an operator tune the watchdog to the vehicle's expected think/act
latency, with a guard that the watchdog can never be set tighter than the slowest single actuator call.
