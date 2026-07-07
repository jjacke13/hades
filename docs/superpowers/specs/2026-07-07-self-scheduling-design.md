# hades self-scheduling — design (agent creates its own cron/one-shot tasks)

**Date:** 2026-07-07
**Status:** approved (Vaios)
**Branch:** `feat/self-scheduling` off `main` @ `6631338`
**Depends on:** the shipped heartbeat/cron feature (`feat/heartbeat`, merged) — this extends `HeartbeatModule`.

## The one idea

Today hades is scheduled only by the **operator**: `Heartbeat = <name>` blocks are parsed once in
`wire_agent` and are immutable at runtime. This feature lets the **agent** create, list, and cancel its
own scheduled turns at runtime — recurring (cron) or one-shot ("in 10 min" / "at 09:00 tomorrow") — via
three native tools, so a human-set goal can spawn its own monitors and reminders, and (opt-in) a tick can
schedule follow-up work.

Maps to Claude Code's `CronCreate`/`CronList`/`CronDelete` + `ScheduleWakeup` and the leaked KAIROS
periodic-tick daemon. The agent-comms gap-analysis (`docs/research/2026-07-05-agent-comms-landscape.md`)
flagged self-scheduling as the missing autonomy primitive.

## The hard constraint

hades tools are **stateless subprocesses** — a `schedule_task` tool CANNOT reach the running
`HeartbeatModule` to add an in-memory entry. The bridge is a **persistence file**: the tool appends to
`.hades/cron.jsonl`, and the `HeartbeatModule` loads it on boot and **re-reads it on every ~30s tick-scan**,
so an add/cancel takes effect within one scan without a restart. This mirrors the existing
`save_memory`/`pin_fact` model exactly (a stateless tool + a module/Arbiter that re-reads the file each
turn), and reuses the `memory.jsonl` append-only + tolerant-parse store pattern.

## What a scheduled task IS

**A task is a PROMPT to the LLM**, never a raw command. A tick is a normal gated Arbiter self-turn
(`TURN_ORIGIN=heartbeat:<name>`, the exact path a static heartbeat uses): post the prompt as a
`USER_MESSAGE` → the LLM decides → the LLM calls a tool (`run_command`/`shell`/…) → reply. "Run a script"
is expressed as an instruction (*"run `./backup.sh`, tell me if it fails"*); the agent's `run_command` call
then passes `capability_policy` (`exec_allow` scope, `fs_deny`, confirm auto-deny for the heartbeat origin).

**No raw `command`-kind (deliberate non-goal).** A direct-exec task would be a second execution path that
bypasses the whole objective/gate layer (no Arbiter turn → no `capability_policy` veto, no `avoid_destructive`,
no confirm auto-deny) and would duplicate system `crontab`. Prompt-only keeps ONE gated execution path and
earns its keep over dumb cron by the agent *deciding and reacting* to output. Dumb periodic exec belongs in
the OS crontab, not here.

## Gating (the runaway guard)

A self-scheduling agent is a runaway risk. Three layers:

1. **Rostering is opt-in.** The three tools do nothing unless the manifest rosters them (`Tool = schedule_task`,
   like every other tool). Absent → the agent cannot self-schedule at all.
2. **`SelfScheduleGuard` objective** (a `PeerLoopGuard` mirror): hard-vetoes `schedule_task` when the current
   turn's `TURN_ORIGIN` starts `heartbeat:` **and** `allow_self_schedule` is false (default). So:
   - a **human-origin** turn (your conversation) may schedule freely — set a complex goal, the LLM sets its
     own monitors/reminders;
   - a **heartbeat-origin** tick may schedule more tasks **only** when `allow_self_schedule = true` — off by
     default, so a tick cannot breed more ticks (recursion contained).
   The guard covers **only the create path** (`schedule_task`); `list_tasks`/`cancel_task` are always allowed
   (read/cancel own tasks — low risk).
3. **Caps** (always on, enforced in the `schedule_task` tool by reading the store): `max_tasks` (refuse a new
   add when the active dynamic count is at the cap) and `min_interval_s` (reject a one-shot whose delay is
   below the floor). Plus `stay_on_budget` bounds cost as for any turn. Cron entries are minute-resolution
   with per-entry dedup → an inherent 60s/task floor, so we do not compute arbitrary cron min-gaps.

## Store — `.hades/cron.jsonl` (append-only, tombstone fold)

One JSON record per line. Three ops:

```json
{"op":"add","id":"t1751900000-a3f9","name":"nightly","kind":"cron","schedule":"0 3 * * *","fire_epoch":null,"prompt":"summarize the day","notify":true,"created":1751900000}
{"op":"add","id":"t1751900050-b1c2","name":"remind","kind":"once","schedule":null,"fire_epoch":1751986800,"prompt":"ping Vaios about the PR","notify":true,"created":1751900050}
{"op":"cancel","id":"t1751900000-a3f9"}
{"op":"done","id":"t1751900050-b1c2"}
```

- **`add`** — written by `schedule_task`. `kind` is `"cron"` (recurring; `schedule` set, `fire_epoch` null) or
  `"once"` (one-shot; `fire_epoch` set = machine-local epoch seconds, `schedule` null).
- **`cancel`** — written by `cancel_task` (a tombstone).
- **`done`** — written by the **module** when a one-shot fires (records completion so it never re-fires,
  survives restart).

**Fold:** iterate records in file order; `add`→insert into an id→record map; `cancel`/`done`→erase. The
active set = surviving map values. Parse is **tolerant** (skip blank/corrupt/partial lines — the
`memory.jsonl` / session-jsonl precedent). Multi-writer safety: the tool subprocesses and the module thread
only ever **append** short lines; POSIX `O_APPEND` is atomic for writes ≤ PIPE_BUF (4096), so lines never
interleave, and a rare torn final line is tolerated.

**Compact on boot:** at startup the module rewrites `cron.jsonl` to just the folded active set (drops
`cancel`/`done`/superseded records, and a one-shot whose `fire_epoch` already passed and is `done`). Safe
because the module is effectively the sole writer at boot; a schedule racing boot is rare and self-heals at
the next boot. (`ponytail`: compaction beyond boot is a v2 concern.)

**`id`** — generated by `schedule_task`: `t<epoch_seconds>-<rand4hex>` (e.g. `t1751900000-a3f9`). Stable,
listable, cancelable. Uniqueness is per-second + 16 bits of `std::random_device` — sufficient at this scale.

## The three tools (native subprocess binaries, `pin_fact`/`save_memory` pattern)

Each reads one JSON line (`{"call":"describe"|"<tool>","args":{…}}`), writes one JSON line, is
self-describing (`describe`), type-guarded (malformed/adversarial → `ok:false`, never throws), and takes its
store path (and caps) from `argv` — the **single source of truth**, wired in, never chosen by the LLM.

### `schedule_task` — create
argv: `<cron_store> <max_tasks> <min_interval_s>`.
args: `name` (req, string), `prompt` (req, string), `notify` (bool, default false), and **exactly one** of:
- `schedule` (string) — a 5-field cron; validated with `cron_valid()` (shared `hades/heartbeat/cron.h`
  header — same validator the module uses). → `kind=cron`.
- `in_minutes` (number ≥ 1) — relative delay; `fire_epoch = now + in_minutes*60`. Rejected if
  `in_minutes*60 < min_interval_s`. → `kind=once`.
- `at` (string) — absolute, machine-local. Accepts `YYYY-MM-DDTHH:MM` (or with seconds) and bare `HH:MM`
  (the next future occurrence of that clock time). Parsed to `fire_epoch`. → `kind=once`.

Behavior: validate exactly-one-timing + non-empty name/prompt; read+fold the store and **refuse** with
`ok:false` if the active count ≥ `max_tasks`; generate `id`; append the `add` record; return
`{"ok":true,"result":{"id","name","kind","when"}}` (`when` = the cron string or an ISO of `fire_epoch`).
Fail-closed on any bad/ambiguous args (`ok:false` + message).

### `list_tasks` — read
argv: `<cron_store>`. args: none. Folds the store; returns the active **dynamic** tasks:
`{"ok":true,"result":{"tasks":[{id,name,kind,schedule|fire_epoch(as ISO),prompt,notify}...]}}`. Static
manifest `Heartbeat` blocks are **not** in the file and are **not** listed (they are operator-owned; the tool
description says so). Empty/missing store → `{"tasks":[]}`.

### `cancel_task` — delete
argv: `<cron_store>`. args: `id` (req, string). Folds the store; if `id` is active, append a `cancel` record
and return `{"ok":true,"result":{"cancelled":true,"id"}}`; if not active, `{"ok":false,...}` (unknown id).

## `HeartbeatModule` changes

- **New members:** `std::string cron_store_` (set via `set_cron_store`, wired); `std::vector<HeartbeatEntry>
  dynamic_` (reloaded from the store, kept separate from the static `entries_`); `std::map<std::string,long
  long> last_fired_by_id_` (per-id minute dedup that survives a reload — a reloaded recurring entry must not
  re-fire the same minute).
- **`HeartbeatEntry` gains:** `std::string id` (empty for static manifest entries), `bool one_shot = false`,
  `long long fire_epoch = 0`.
- **Boot** (in `start()` / first attach): load + compact the store into `dynamic_`.
- **Each ~30s scan:** reload+fold the store → rebuild `dynamic_`, carrying each surviving id's
  `last_fired_minute` from `last_fired_by_id_`. Then `tick()` iterates **both** `entries_` (static) and
  `dynamic_`.
- **Matching in `tick(std::tm now)`:** a `cron` entry matches via `cron_matches(schedule, now)` (unchanged); a
  `one_shot` entry matches when `now_epoch >= fire_epoch`, where `now_epoch = mktime(&now_copy)` (machine-local
  epoch — consistent with the machine-local cron convention). `tick` computes `now_epoch` once.
- **After a one-shot fires:** append `{"op":"done","id":…}` to the store and mark the id locally, so it cannot
  re-fire before the next reload.
- **`fire_` is otherwise unchanged:** try_lock the shared TurnGate (skip-if-busy → `HEARTBEAT_SKIPPED`),
  `confirm_outstanding_` skip, `TURN_ORIGIN=heartbeat:<name>`, `USER_MESSAGE`, `run_until`, notify/drop with
  the `SILENT` sentinel + confirm-auto-deny note. Dynamic and static ticks are identical from here.

## Manifest (`Heartbeat` block, extended) + wiring

```
Module = heartbeat
Heartbeat
{
  cron_store          = .hades/cron.jsonl   # dynamic-task store (default)
  allow_self_schedule = false               # default; true = heartbeat ticks may create tasks
  max_tasks           = 20                  # cap on active dynamic tasks (default)
  min_interval_s      = 60                  # one-shot delay floor, seconds (default)
}
Tool = schedule_task { native = ./build/hades-schedule-task }
Tool = list_tasks    { native = ./build/hades-list-tasks }
Tool = cancel_task   { native = ./build/hades-cancel-task }
```

Wiring (`app/agent_wiring.cpp`, `wire_agent`):
- Resolve `cron_store` (default `.hades/cron.jsonl`); `reject_ws` (argv is whitespace-split, `pin_fact`
  precedent → `MalConfig` on a whitespace path).
- Append the store (+ caps for `schedule_task`) to each tool's `native` argv — single source of truth.
- If `agent.heartbeat`: `set_cron_store(cron_store)`.
- Register `SelfScheduleGuard(allow_self_schedule)` as an objective when heartbeat is present **and**
  `schedule_task` is rostered.
- Capability table (`capability_of`): `schedule_task`/`list_tasks`/`cancel_task` → a new
  `Capability::SelfSchedule` enum value → **allow** by default (the `SelfScheduleGuard` + tool caps do the
  gating; the capability layer stays permissive like `pin_fact`/`MemoryAppend`). A distinct enum keeps the
  table honest so a future policy can confirm-gate scheduling without code churn — the `SkillRead`/`SkillWrite`
  precedent.

The test `build_agent(bb, provider, …)` overload leaves `agent.heartbeat == nullptr` and does not register the
guard — existing tests unaffected (heartbeat opt-in precedent).

## Restart / `--resume`

`cron.jsonl` is independent of the session jsonl and **persists across restarts** — the whole point of "remind
me tomorrow 9am". A one-shot whose `fire_epoch` passed while the process was **down** fires on the first
post-boot scan (**catch-up**), then `done`; it is not silently dropped. Recurring cron resumes normally.
`--resume` (session reload) and the cron store are orthogonal — reloading a conversation does not touch the
task store, and vice versa. (v2: a staleness cap so a task overdue by days is dropped rather than fired.)

## Testing

- **Store fold/compact (pure lib):** add→active; add+cancel→gone; add+done→gone; superseded add; torn/blank
  lines skipped; compact yields the folded set.
- **`schedule_task` tool:** `describe` schema; cron path (`cron_valid` accept/reject); `in_minutes` →
  `fire_epoch` + `min_interval_s` refuse; `at` ISO + `HH:MM` next-occurrence parse; exactly-one-timing
  enforced; `max_tasks` refuse; bad/non-string args → `ok:false`; returns a non-empty `id`.
- **`list_tasks` / `cancel_task` tools:** list active; cancel active → tombstone + gone from a re-list; cancel
  unknown id → `ok:false`; empty store → `{tasks:[]}`.
- **Module reload:** a dynamic cron entry fires; a one-shot fires exactly once and writes a `done`; dedup by id
  across a reload (no double-fire same minute); static `entries_` + `dynamic_` coexist; catch-up fires an
  overdue one-shot once.
- **`SelfScheduleGuard`:** heartbeat-origin `schedule_task` vetoed when switch off, allowed when on;
  human-origin always allowed; `list_tasks`/`cancel_task` never vetoed.
- **Wiring:** argv append (store + caps reach the binaries); whitespace `cron_store` → `MalConfig`; guard
  registered only when heartbeat + `schedule_task` present; `build_agent` test overload leaves it all null.

## Non-goals (v1)

- **Raw `command`-kind** — prompt-only, gate-preserving (above).
- **Static heartbeats in `list_tasks`** — operator-owned, not in the file.
- **`tz` key for `at`** — machine-local, like cron.
- **Compaction beyond boot** — append-only growth is bounded in practice; compact-on-boot suffices.
- **Staleness-drop for long-overdue one-shots** — v1 fires catch-up; a grace cap is v2.
- **`/list` or web surfacing of tasks** — the agent uses `list_tasks`; an operator reads `cron.jsonl` / the
  Eventlog.

## Pieces (anticipated)

- `include/hades/heartbeat/cron_store.h` + `src/apps/heartbeat/cron_store.cpp` — pure store types + fold +
  compact + record (de)serialize + `at`/`in_minutes` → `fire_epoch` helpers.
- `tools/{schedule_task,list_tasks,cancel_task}_main.cpp` — the three binaries.
- `include/hades/module/heartbeat_module.h` + `src/apps/heartbeat/heartbeat.cpp` — reload, `dynamic_`,
  one-shot match + `done`, `set_cron_store`.
- `include/hades/behaviors/self_schedule_guard.h` + `src/behaviors/self_schedule_guard.cpp` — the origin veto.
- `app/agent_wiring.{h,cpp}` — argv, `set_cron_store`, guard registration, capability table entries.
- `manifests/dev.hades` (commented example) + `docs/manifest-reference.md` §15 extension + `prompts/soul.md`
  (a line telling the agent it can schedule its own follow-ups) + `CLAUDE.md`.
