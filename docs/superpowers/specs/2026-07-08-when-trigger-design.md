# hades reactive `when=` trigger — design (condition-fired self-turns)

**Date:** 2026-07-08
**Status:** approved (Vaios)
**Branch:** `feat/when-trigger` off `main` @ `65d853f`
**Depends on:** heartbeat/cron (shipped 2026-07-07) + self-scheduling (shipped 2026-07-08) — this extends
both. Origin: the "act when peer state changes" Monitor-style consumer deferred from the bridge protocol
and folded into the heartbeat brainstorm (CLAUDE.md).

## The one idea

Today a heartbeat entry fires on a **schedule** (cron / one-shot epoch). This adds the other trigger kind:
**`when = <condition>`** — fire a self-turn when a Blackboard variable changes or crosses a threshold.
"Watch pi0's card and tell me when its capabilities change", "act when the budget passes 0.8", "react when
the mission state leaves idle". Both creation surfaces ship: **static** manifest entries and **dynamic**
`schedule_task` watches (the agent sets its own at runtime, conversation-driven).

## Evaluation model — poll at tick, never subscribe

The condition is evaluated on the existing **~30s tick scan** against the Blackboard's **latest value**
(`bb_->get(key)` — thread-safe read from the timer thread). NOT a bus subscription: a subscriber handler
runs on the pump thread, mid whoever-holds-the-gate's turn, where driving a new turn is impossible — an
event-driven design collapses back into "set a flag, let the timer thread fire it", i.e. polling with extra
plumbing. Polling at tick is also the MOOS-native shape (`Iterate()` at a rate). Consequence, documented:
**reaction latency is up to ~30s** (one wake period). Sub-30s latency is a non-goal (v1).

## Condition vocabulary — 5 keyword forms

`when = <KEY> <op> [operand]`, whitespace-separated, keyword operators (an `=` inside an inline manifest
value trips the one-kv-per-line fail-loud detector — `==`/`!=` are a footgun; keywords are parser-safe):

| Form | Fires when |
|---|---|
| `KEY changes` | the key's value differs from the last observed value (any change) |
| `KEY is <str>` | the value, as a string, equals `<str>` (edge) |
| `KEY not <str>` | the value, as a string, differs from `<str>` (edge) |
| `KEY above <n>` | the value is a number `> n` (edge) |
| `KEY below <n>` | the value is a number `< n` (edge) |

Value-to-string for `is`/`not`: a JSON string compares as its raw string content; any other JSON value
compares as its compact dump. `above`/`below` require a JSON number (non-number → condition false). Any
Blackboard key is watchable (including `PEER.*`); no key allowlist in v1 — the operator writes the manifest,
and the dynamic path is origin-gated (below). Absent key → condition false (fail-soft, never throws).

**Pure lib:** `include/hades/heartbeat/when.h` + `src/apps/heartbeat/when.cpp` (cron.cpp sibling):

- `struct WhenCond { std::string key; enum class Op { Changes, Is, Not, Above, Below } op; std::string operand; };`
- `std::optional<WhenCond> parse_when(const std::string& expr);` — nullopt on malformed (wrong arity,
  unknown op, non-numeric operand for above/below).
- `bool when_valid(const std::string& expr);` — the fail-loud validator (manifest wiring + the tool).
- `bool when_holds(const WhenCond&, const nlohmann::json* value);` — pure evaluation for `is/not/above/
  below`; `nullptr` (absent key) → false. (`changes` is evaluated in the module against stored state.)

Shared the same way `cron.h` is: compiled into `hades_core` AND into the `schedule_task` binary.

## Entry shape + edge semantics

A heartbeat entry — static (`Heartbeat = <name>` block) or dynamic (store record) — has **exactly one of
`schedule` | `when`** (one-shot `fire_epoch` counts as `schedule`-kind for this rule). Both → `MalConfig`
(static) / `ok:false` (tool); neither → same.

Per-entry edge state, evaluated each tick:

- **`changes`:** the first scan **arms** (records the value's compact dump, no fire). Each later scan whose
  dump differs → fire once, then re-arm on the new value. Boot can never fire (nothing to compare) — a
  restart re-arms on the first scan.
- **`is`/`not`/`above`/`below`:** fire on the **false→true edge** (`now_true && !was_true`); re-arm when the
  condition goes false. A condition **already true at boot fires once on the first scan** (deliberate: "the
  budget is already above 0.8" is exactly what you want to hear about); it will not fire again until the
  condition goes false and comes back.
- **Skip-if-busy does not consume the edge:** if `fire_` reports the turn did not run (TurnGate busy /
  confirm outstanding — the self-scheduling `fire_`-returns-`bool` seam), the edge state is NOT advanced:
  the entry retries on the next tick. A fired-and-ran turn advances the state even if the turn itself
  timed out (same rule as one-shots: a driven turn is consumed).
- **`cooldown_s`** (per entry, default **60**, optional key/arg): after a fire that ran, further fires of
  that entry are suppressed until `now >= last_fire + cooldown_s` — even if new edges occur. Bounds a flappy
  key (a chatty `PEER.*` var, an oscillating threshold) so it cannot spam turns/notifications. Edges that
  occur during the cooldown are absorbed, not queued: when the cooldown expires the entry re-evaluates
  fresh (edge semantics from the current state, no replay).

Everything downstream of the trigger is IDENTICAL to a cron tick: `TURN_ORIGIN = heartbeat:<name>`, the
entry's `prompt` as the `USER_MESSAGE`, all objectives, confirm-band auto-denied, `notify`/`SILENT`
convention, Eventlog visibility.

## Dynamic watches (`schedule_task` gains a 4th kind)

- **`schedule_task`** accepts **exactly one of** `schedule` | `in_minutes` | `at` | **`when`** (string,
  validated by the shared `when_valid` → `ok:false` on malformed). Optional `cooldown_s` (number ≥ 0,
  default 60). Store record: `kind:"when"`, new fields `when` (string) + `cooldown_s`.
- **`list_tasks`** surfaces the `when` string (as it does `schedule`/`at`).
- **`cancel_task`** unchanged (tombstone by id).
- **Module reload:** dynamic when-entries are rebuilt each scan like every dynamic task; their edge state
  survives the rebuild in a `when_state_by_id_` map (last dump + `was_true` + last-fire time), pruned to the
  active id set — the `last_fired_by_id_` pattern from self-scheduling. Static entries keep their edge state
  in the entry itself (never rebuilt).
- The `describe` schema documents `when` with the 5 forms in one line (the agent must know the vocabulary —
  unlike `expect_version` this IS LLM API surface).

## Guardrails (+ the peer-origin hole this closes)

- A when-turn is a normal gated self-turn — `capability_policy`, `avoid_destructive`, `stay_on_budget`,
  confirm auto-deny all apply. `max_tasks` counts watches (they occupy task slots). `cooldown_s` bounds
  fire rate; `min_interval_s` does not apply to watches (no fixed period to floor).
- **`SelfScheduleGuard` broadened (fix folded into this feature):** today it vetoes `schedule_task` only on
  `heartbeat:` origins — but a **`peer:`-origin** turn can call `schedule_task` (the capability is
  allow-band, and peers get exactly the receiver's unconfirmed powers), letting a peer talk the receiver
  into planting standing tasks/watches on its box. The guard now **hard-vetoes `schedule_task` on `peer:`
  origins unconditionally** (not switchable — a peer must never create standing work; bridge philosophy),
  while `heartbeat:` origins stay governed by `allow_self_schedule` and human origins stay free. Same
  guard, one broadened condition, regression tests for all three origins.

## What is deliberately NOT covered (v1)

- **Compound conditions** (`&&`/`||`) — one key, one op per entry; roster two entries instead.
- **Sub-30s latency** — evaluation rides the existing wake period.
- **Regex/contains matching, JSON-path into values** — `is`/`not` compare whole values.
- **Watch expiry/TTL** — a watch lives until cancelled (`cancel_task` / manifest edit).
- **Replay/queueing of absorbed edges** — cooldown drops them by design.
- **Key allowlist for watches** — any bus key; the origin gate + caps + cooldown are the containment.

## Testing

- **`when` lib (pure):** parse all 5 forms + malformed (wrong arity/op/non-numeric threshold); `when_holds`
  for string/number/object values, absent key, non-number under `above`.
- **Module (scripted-bus rig, `tick()` seam):** `changes` arms-then-fires-once-per-change; `is` edge
  fire/re-arm; already-true-at-boot fires once; busy-skip does not consume the edge (gate held); cooldown
  absorbs a flap; absent key never fires; static + dynamic when-entries coexist with cron entries;
  dynamic edge state survives a reload (`when_state_by_id_`), pruned on cancel.
- **`schedule_task` tool:** `when` accepted as the exclusive 4th kind (exactly-one still enforced across
  all four); malformed `when` → `ok:false`; `cooldown_s` stored; `list_tasks` shows the condition.
- **`SelfScheduleGuard`:** peer-origin `schedule_task` vetoed ALWAYS (with and without
  `allow_self_schedule`); heartbeat-origin behavior unchanged; human unchanged.
- **E2E:** post a bus var change → next tick fires the watch → notify carries the reply.

## Pieces (anticipated)

- `include/hades/heartbeat/when.h`, `src/apps/heartbeat/when.cpp` — pure condition lib.
- `include/hades/module/heartbeat_module.h`, `src/apps/heartbeat/heartbeat.cpp` — `when` entry kind, edge
  state, cooldown, `when_state_by_id_`.
- `include/hades/heartbeat/cron_store.h`, `src/apps/heartbeat/cron_store.cpp` — `when`/`cooldown_s` fields.
- `tools/schedule_task_main.cpp` (4th kind), `tools/list_tasks_main.cpp` (display).
- `src/behaviors/standard_behaviors.cpp` — `SelfScheduleGuard` peer-origin veto.
- `app/agent_wiring.cpp` — `when` XOR `schedule` parse for named blocks, `cooldown_s` key, `when_valid`
  fail-loud.
- Docs: `docs/manifest-reference.md` §15 (the `when` key + vocabulary + cooldown + latency note),
  `prompts/soul.md` (one line: watches exist — "watch X and tell me when it changes" is schedulable),
  `CLAUDE.md`. dev.hades commented example.
