# Heartbeat / cron — self-triggered autonomous turns — design

**Date:** 2026-07-07
**Status:** Approved (Vaios — brainstorm 2026-07-07, sections 1–5 approved)
**Branch:** `feat/heartbeat` (off `main`)
**Direction:** NEXT direction 2 (the autonomy leg). Turns hades from a purely event-driven assistant
(a turn fires only on `USER_MESSAGE` / peer `/ask`) into a standing agent that acts on its own.

## Problem

hades is purely **event-driven**: a turn happens only when a human sends a message or a peer calls
`/ask`. Vaios wants the agent to **do things on its own** — periodic monitoring that notifies him when
something's worth reporting, and scheduled background tasks (summaries, upkeep, peer polling) that run
without a human present. MOOS analog: a timer-driven app / `Iterate()` at a rate. The Claude Code leak
surfaced the same unshipped idea ("KAIROS": a periodic `<tick>` → decide-whether-to-act daemon) —
convergent design confirmation.

## Decision summary (from the brainstorm)

- **First uses = both** monitor-and-notify **and** scheduled self-contained tasks. One mechanism covers
  both: a tick drives a **normal self-turn** through the Arbiter (tools + gates + memory + peer folds
  all apply); its reply is **forwarded to the user** or **dropped** per a per-entry `notify` flag.
- **Schedule = full 5-field cron** (`minute hour dom month dow`), minute resolution, machine-local time.
- **Notify = per-entry `notify` flag + a `SILENT` sentinel:** `notify=false` drops the reply (task);
  `notify=true` forwards the reply to `NOTIFY_USER` unless it's empty or `SILENT` (quiet monitor).
- **Sink = Telegram (v1)** — subscribes `NOTIFY_USER`, sends to `allow_users`.
- **A tick is a normal gated turn** — capability_policy/avoid_destructive/stay_on_budget all apply;
  confirm-band actions are **auto-denied** (no human to approve); `TURN_ORIGIN=heartbeat:<name>`.
- **Reactive/condition triggers, webhooks, timezones, sub-minute intervals = deferred v2.**

## Architecture

### `HeartbeatModule` (`type()=="heartbeat"`)

Opt-in: `Module = heartbeat` + ≥1 `Heartbeat` block. Omit → `Agent.heartbeat == nullptr`, zero coupling
(the `build_agent` test overload leaves it null; the established module pattern). Owns **one timer
thread** (the embedding-reindex lifecycle: stop-flag + condition-variable + join in the dtor; started by
`hades_main` **after** wiring, **never** in `on_attach` — so tests spawn no thread).

**Tick loop.** The thread wakes ~every 30 s (cv-interruptible), computes `std::localtime`, and calls the
testable seam `tick(now_tm)`. `tick` evaluates every entry's cron against `now_tm` and fires each entry
that **matches** and whose matched minute ≠ its `last_fired_minute_` (per-entry dedup → exactly once per
matching minute; a minute missed while busy/asleep is simply skipped — no catch-up backfill).

**Firing a self-turn** (mirrors every front-end's turn drive, exactly like `BridgeModule::handle_ask`):

```
if (!turn_gate.try_lock()) { post HEARTBEAT_SKIPPED; stamp minute; return; }  // skip if a turn is running
my_turn_ = true;                       // capture the reply (RAII reset on every exit path)
got_reply_ = false; denied_confirm_ = false;
post TURN_ORIGIN = "heartbeat:<name>"; // provenance; PeerLoopGuard ignores non-"peer:" origins
post USER_MESSAGE = "(scheduled heartbeat \"<name>\") " + entry.prompt;
run_until(got_reply_ || abandoned, idle_timeout);
// notify decision (below); RAII unlock
```

**Key simplification — no KAIROS-style state injection needed.** A tick is a normal `USER_MESSAGE`
turn, so the Arbiter already assembles the full leading system message (soul + live core memory + skills
roster + the `PEER.*` capability/report folds) and the agent can call any tool — including `ask_agent`
to delegate to a peer. The entry's `prompt` is just the task ("check pi0 health and report if
degraded"); the disposition ("decide whether to act, reply SILENT if nothing to do") lives in the prompt.

**The Eventlog is the activity log.** Every tick's `USER_MESSAGE`/`ASSISTANT_MESSAGE`/tool calls are
recorded with `TURN_ORIGIN=heartbeat:<name>`, replayable via `hades-scope` — this already is KAIROS's
"append-only daily log". No new log.

### Cron matcher (pure)

`bool cron_matches(const std::string& expr, const std::tm& t)` — 5 space-separated fields
`minute(0-59) hour(0-23) dom(1-31) month(1-12) dow(0-6, 0=Sun)`. Each field supports `*`, `N`, `A-B`,
`*/N`, and comma lists `A,B,C` (and combinations, e.g. `1-5,10`). **AND** across all five fields (the
Vixie dom/dow-OR quirk is skipped — it only differs when both dom and dow are restricted, rare;
documented). Tolerant: a malformed field/expression → `false` (never throws). A field count ≠ 5 →
`false`. `*/N` with `N<=0` → `false`.

Minute resolution is sufficient (monitoring/scheduling granularity); a sub-minute `every_s` escape hatch
is a v2 add.

### Schedule / timezone

Evaluation uses the **machine's local time** (`std::localtime`) — Pi = UTC, desktop = EET, so "6 am"
differs per host; documented. A `tz` key (per-entry or global) is v2. The timer wakes every 30 s so a
minute boundary is never missed; the per-entry minute-stamp prevents a double-fire within the same minute.

### Notify path

After `run_until` returns:
- **`notify == false`** → reply **dropped** (scheduled task; the turn's tool actions were the output).
- **`notify == true`** → let `r = trim(last_reply_)`:
  - `r.empty()` or `r == "SILENT"` (case-sensitive; the prompt instructs the exact token) → **dropped**.
  - else → post **`NOTIFY_USER = {"text": r, "from": "heartbeat:<name>"}`**, then `pump()` (on the
    heartbeat thread, which is the pump thread for its own turn while holding the gate) so a subscribing
    front-end delivers it synchronously on that thread.
- An **abandoned/errored** tick (idle timeout, no reply) → no notification, logged `HEARTBEAT_ERROR`.

**Sink (v1) = Telegram.** `TelegramModule` subscribes `NOTIFY_USER` and `sendMessage`s the text to every
`allow_users` id (small, all trusted). Best-effort / **fail-soft** — a send failure logs, never crashes.
`NOTIFY_USER` is a generic bus key; Telegram is the only async-push sink today (a web SSE push sink and a
`notify_users` subset are v2). A `notify=true` heartbeat with no Telegram rostered posts `NOTIFY_USER`
but nothing delivers it (logged) → **want notifications ⇒ roster Telegram**.

### Guardrails

- **Confirm-band auto-deny.** No human present, so while `my_turn_` a `CONFIRM_REQUEST` → immediate
  `CONFIRM_RESPONSE{approved:false}` + a note appended to the reply (mirror `BridgeModule`). A tick gets
  exactly its **unconfirmed** powers; it can't be steered into a destructive confirm-band action. To let
  a tick run a specific risky tool, give it a non-confirm scope (`exec_allow`/`fs_write_allow`).
- **Skip-if-busy.** `try_lock` the TurnGate; a human/peer turn holding it → skip this tick
  (`HEARTBEAT_SKIPPED`, minute stamped). A self-turn never makes the user wait; stale ticks never queue.
- **Capability gates + budget apply unchanged.** `capability_policy`/`avoid_destructive` gate a tick's
  tool calls like any turn; `stay_on_budget` meters its LLM spend against the same `hard_cap_usd` (a
  runaway monitor is bounded — the operator sizes cron × budget). No new security surface — a tick is a
  normal gated turn plus the auto-deny.
- **Delegation allowed.** `TURN_ORIGIN=heartbeat:<name>` is not a `peer:` origin, so `PeerLoopGuard`
  does not veto `ask_agent` — a tick can delegate to a peer (monitor pi0). No loop risk (the peer turn is
  peer-origin and cannot forward back).
- **Abandonment reuse.** Same `run_until` idle-timeout + `TURN_ABANDONED` + turn-epoch machinery: a stuck
  tick self-heals, its late response is dropped, the gate is freed.
- **Teardown.** The timer thread is stop+notify+**joined in `~HeartbeatModule`**, in the front-end
  teardown group — joined while the Executor + Arbiter + ToolRunner + Blackboard are still alive (a tick
  drives a full turn through them). Started by `hades_main` after wiring. A tick in-flight at shutdown:
  the join waits for the current tick to finish or hit its idle ceiling.

## Config (manifest)

```
Module = heartbeat

Heartbeat = monitor
{
  schedule = */10 * * * *          # every 10 minutes
  prompt   = Check pi0 is reachable and healthy (ask it). Reply exactly SILENT if all is fine.
  notify   = true
}

Heartbeat = daily
{
  schedule    = 0 6 * * *          # 06:00 machine-local, daily
  prompt_file = prompts/daily_summary.txt
  notify      = false
}
```

| Key | What it does | Default | Notes |
|---|---|---|---|
| `schedule` | 5-field cron (`min hour dom month dow`). | — | **Required.** Bad cron → `MalConfig` at launch. Minute resolution, machine-local time. |
| `prompt` | Inline tick task (the `USER_MESSAGE`). | — | One of `prompt`/`prompt_file` **required**. **Must not contain ` word = word`** — the one-kv-per-line fail-loud detector would refuse to boot; use `prompt_file` for anything with an `=` or multi-line. |
| `prompt_file` | Path to a file holding the tick task. | — | cwd-relative; unreadable → `MalConfig`. The robust form for long/`=`-bearing prompts (mirrors `system_prompt_file`). |
| `notify` | `true` → forward the reply to `NOTIFY_USER` (unless empty/`SILENT`); `false` → drop it. | `false` | `true` needs a `NOTIFY_USER` sink (Telegram v1). |

**Parser footgun (documented).** Cron values are safe (no `=`). An inline `prompt` containing ` x = y `
trips the multi-kv fail-loud guard → use `prompt_file`.

## Components (file boundaries)

| File | Responsibility |
|---|---|
| `include/hades/heartbeat/cron.h`, `src/apps/heartbeat/cron.cpp` (or folded into the module TU) | pure `cron_matches(expr, tm)` |
| `include/hades/module/heartbeat_module.h`, `src/apps/heartbeat/heartbeat.cpp` | `HeartbeatModule` — entries, `tick(now)` seam, self-turn drive, notify decision, confirm auto-deny, timer thread + teardown |
| `src/apps/telegram/telegram.cpp`, `include/hades/module/telegram_module.h` | subscribe `NOTIFY_USER` → `sendMessage` to `allow_users` (fail-soft) |
| `app/agent_wiring.{h,cpp}` | `Agent.heartbeat` member, `heartbeat` factory, parse `Heartbeat` blocks (schedule/prompt/prompt_file/notify) + cron validation, inject TurnGate, member slot in the front-end teardown group |
| `app/hades_main.cpp` | `agent.heartbeat->start()` (timer thread) after wiring |
| `manifests/dev.hades`, `prompts/daily_summary.txt` (example), `docs/manifest-reference.md`, `CLAUDE.md` | ship: commented `Heartbeat` example + docs |

## Testing

TDD. Baseline **450/450** (post bridge-protocol). New:
- **`cron_matches`** (pure): `*`, `N`, `A-B`, `*/N`, `A,B`, combos, field bounds; `0 6 * * *` matches
  06:00 only; `*/10` matches minutes 0/10/20…; dow; wrong field-count / garbage → `false`.
- **`HeartbeatModule`** via the `tick(now_tm)` seam (no real thread/clock; scripted echo agent on the
  bus, like the bridge rig): fires once per matching minute (dedup across two ticks in the same minute);
  a non-matching time fires nothing; `notify=false` → no `NOTIFY_USER`; `notify=true` + non-silent reply
  → `NOTIFY_USER` posted; `SILENT`/empty → none; skip-if-busy (gate pre-locked → `HEARTBEAT_SKIPPED`, no
  turn); confirm auto-deny (a `CONFIRM_REQUEST` in the tick → `CONFIRM_RESPONSE{approved:false}`);
  `TURN_ORIGIN=heartbeat:<name>` posted before `USER_MESSAGE`.
- **Wiring:** `Module = heartbeat` + blocks → `agent.heartbeat` non-null, entries parsed, `prompt_file`
  read, bad cron / missing prompt → `MalConfig`; no-heartbeat roster → `agent.heartbeat == nullptr`,
  starts no thread.
- **Telegram:** post `NOTIFY_USER` → fake api `sendMessage` called with the text to `allow_users`.
- **TSan** at feature end (timer thread + the turn it drives + the Telegram send).

## Deferred / v2

- **Reactive / condition triggers** — a `Heartbeat` entry with a `when = <bus condition>` (fire on a
  `PEER.*` change, a bus var crossing a threshold, etc.) instead of `schedule`. This is the **Monitor-
  style between-turns reactive `PEER.*` consumer** deferred from the bridge protocol — the natural next
  step once cron ticks work.
- Webhook inbound triggers (KAIROS webhook subscriptions — an authenticated HTTP endpoint that fires a
  tick) · `tz` key (per-entry/global timezone) · `every_s` sub-minute interval · missed-tick
  catch-up/backfill · notify sinks beyond Telegram (web SSE push, REPL) · `notify_users` subset · Vixie
  dom/dow-OR semantics · per-entry budget/rate-limit.

## Non-goals

Cron seconds-field or `@daily`/`@reboot` macros · multiple/distributed timezones · a fleet-wide cron ·
webhooks · any state-injection beyond what a normal turn already assembles.
