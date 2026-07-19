# Tool-offload + background tool execution — design

**Date:** 2026-07-19 · **Branch:** `feat/tool-offload` · **Status:** approved (Vaios)

CC tool-gap item 4 ("Background tool execution"). Two staged halves in one branch:

1. **Phase 1 — foreground tool-offload:** tool execution moves off the pump thread onto the
   shared `Executor` (the LLM-offload pattern). The bus stays live during a tool run; the
   turn still waits for `TOOL_RESULT`. Ships the documented prerequisite: the turn-epoch +
   `TURN_ABANDONED` pattern extended to `TOOL_RESULT` (the deferral noted in `arbiter.cpp`).
2. **Phase 2 — `background: true`:** the LLM opts a call into background per invocation.
   It immediately receives `{started: true, task_id}` and the turn continues; the real
   result is delivered cross-turn via a system-message fold (the todo/skills fold pattern).

## Goals / non-goals

**Goals:** long-running tools (shell builds, `ask_agent` peer turns, big fetches) stop
freezing the process; the agent can start work and keep conversing; results survive into
later turns (including heartbeat ticks).

**Non-goals (v1, documented):** background tasks do NOT survive restart (in-memory
registry); no kill/cancel tool for a running bg task; no streaming partial output; no
persistence of finished results beyond the in-memory ring; no per-task notify (delivery is
the fold — a self-turn-on-completion variant was considered and rejected for v1: burns a
turn per completion, needs TurnGate retry logic; the fold reaches the agent on whatever
turn comes next).

## Phase 1 — foreground offload

### ToolRunner execution path

Mirror `LLMModule` exactly (`src/apps/llm/llm.cpp`):

- The `TOOL_REQUEST` handler resolves everything that touches mutable registry state **on
  the pump thread**: `find_by_tool_name` (its `ensure_warm` lazily mutates caches),
  `mcp_real_name`, the effective timeout, the argv split. The worker closure captures only
  **value copies** of those + the non-owning `Blackboard* bb_` — it runs
  `run_subprocess`/`mcp_call`, parses, and posts `TOOL_RESULT`. It reads **no module
  field**.
- `ToolRunner::set_executor(Executor*)` seam, null by default. No executor → run inline →
  every existing test byte-identical. The live Manifest path (`wire_agent`) sets it,
  same place `LLMModule` gets its executor.
- MCP note: `mcp_call` for `mcp_http` uses cpr (thread-safe per-call); stdio spawns a
  subprocess. Both safe off-thread; the `ToolEntry*` captured is a pointer into
  `ToolRegistry::tools_`, which is **append-only before warm and immutable after** —
  document that invariant at the capture site.

### Epoch on TOOL_RESULT (closes the arbiter.cpp deferral)

- Arbiter stamps **every** `TOOL_REQUEST` with `{"epoch", turn_epoch_}` — both post sites
  (`dispatch_or_gate` and the approved-confirm path in `on_confirm`).
- ToolRunner echoes the request's epoch into `TOOL_RESULT` (both offloaded and inline
  paths, like `LlmRequest.epoch`).
- `on_tool_result` drops a mismatched epoch: post
  `DROPPED_STALE_TOOL_RESULT {epoch, current}` and return — **after** the staleness-guard
  version harvest (below), **before** the history append and `start_turn()` continuation.
- `TURN_ABANDONED` already bumps `turn_epoch_` + `clear_pending()` — unchanged; it now
  also invalidates in-flight tool results for free.

### Orphan-history fix (new live bug surface)

With offload, an abandoned turn can leave a trailing `assistant(tool_calls)` in
`history_` whose tool result never arrives (or arrives stale and is dropped) → the next
request would be provider-invalid. Fix: the `TURN_ABANDONED` handler pops trailing
`assistant(tool_calls)` messages from `history_` — the same sanitize `load_history`
already applies on resume. (The on-disk session file keeps the orphan line; resume's
existing sanitize handles it there. Divergence is one dangling line, accepted.)

### Staleness-guard harvest on stale results

A stale-but-successful tracked write (`fs_read`/`edit_file`/`write_file`) still updates
`file_versions_` — the write DID happen; the version is disk truth. `pending_file_ops_`
erase also still happens. Only the history append + turn continuation are skipped.

### New launch invariant

Today's `MalConfig` check (`turn_idle_timeout_s > llm_timeout_s`) was sufficient ONLY
because tools ran inline. Now a silent stretch can be a foreground tool run. Extend the
check: **`turn_idle_timeout_s > max(llm_timeout_s, every foreground-effective tool
timeout)`** — i.e. the runner default (`Tools.timeout_s`, 30) and every `Tool` block's
`timeout_s` override. Computable at boot in `wire_agent` from `tools_resolved`; violation
→ `MalConfig` naming the offending tool. `background_timeout_s` is **exempt** — no
`run_until` waits on a background task (delivery is cross-turn by design).

### Front-ends

**Zero changes.** `run_until` wakes on any post and resets its idle deadline on dispatch
progress; a worker-posted `TOOL_RESULT` resumes the turn exactly as `LLM_RESPONSE` does.
Skip-if-busy TurnGate semantics unchanged — offload is within a turn, not across turns.

### SkillsModule

No `pending_saves_` leak: every `TOOL_REQUEST` still gets exactly one `TOOL_RESULT`
(timeout-bounded), and stale-epoch results are dropped by the **Arbiter** only — they
still reach the bus, so the SkillsModule erases its pending id as before.

## Phase 2 — background tasks

### Announce

`ToolRegistry` appends to **every** announced tool schema (native + MCP) at warm:
`"background": {"type": "boolean", "description": "run in background: returns {started,
task_id} immediately; result appears in the Background tasks block when done (default
false)"}`. Uniform mechanism — no eligibility list. `background` is **harness-owned**:
ToolRunner strips it from the args before building the subprocess call / MCP arguments;
tool binaries never see it. Non-bool values = absent (house rule).

### Flow

On `TOOL_REQUEST` with `args.background == true` (and an executor set — no executor →
ignore the flag, run inline foreground; keeps tests deterministic):

1. Enforce cap: `running >= max_background` → immediate `TOOL_RESULT {ok:false, error:
   "too many background tasks (N running, cap M) — wait for one to finish"}`.
2. Assign `task_id` = `bg-<counter>`; record a running entry `{task_id, tool, started_at}`
   in the registry map (pump-thread-only state).
3. Submit the worker with timeout **`max(per-tool timeout_s, background_timeout_s)`**
   (default 600 — a 30s default would kill any build).
4. **Immediately** post `TOOL_RESULT {id, ok:true, content:{started:true, task_id,
   note:"result will appear in the Background tasks block"}}` with the request's epoch —
   the history assistant/tool pair completes at once; the turn continues.

Worker on completion posts **`BG_DONE {task_id, ok, content}`** — epoch-**free**
(cross-turn by design), captures only `bb_` + values. A ToolRunner `BG_DONE` subscriber
(pump thread — single writer of the registry) moves running → finished ring:

- Finished ring cap **5** (oldest dropped); per-result output truncated to **2000 bytes**
  (UTF-8-safe walk-back, `trunc_utf8` precedent) with a `(truncated)` marker.
- Posts **`BG_TASKS`** (latest-value): a preformatted text block — running entries
  (`task_id · tool · age`) + finished entries (`task_id · tool · ok · output`). Empty
  registry → `""`.

### Arbiter fold

`start_turn()` folds `BG_TASKS` (via `bb_->get`, the SKILLS_ANNOUNCE pattern) into the
leading system message after the todo fold, labeled
`Background tasks (started earlier with background:true):`. Absent/non-string/empty → no
block. Rebuilt on every `start_turn` including mid-turn tool-loop continuations, so a
completion landing mid-turn is visible on the very next LLM round-trip. FIFO pump order
guarantees a `BG_DONE` queued before a `USER_MESSAGE` is folded into that turn.

### Config — new optional `Tools` block

ToolRunner currently gets `on_start(Block{})`; wire the block through:

```
Tools
{
  timeout_s            = 30     # runner-wide default per-tool timeout (was hardcoded)
  max_background       = 4      # concurrent background tasks; over-cap call refused
  background_timeout_s = 600    # bg run timeout: max(per-tool timeout_s, this)
}
```

All keys optional; absent block → today's behavior + defaults above. Garbage values →
default (`set_pos_double_on_string` pattern).

`kExecutorThreads` 2 → **8** (1 LLM + 1 auto-extract + 1 foreground tool + 4 bg + slack;
idle workers cost only a blocked thread).

### Gates and origins

- **Confirm:** unchanged — objectives gate before dispatch; an approved confirm-band call
  with `background:true` then runs in background. Heartbeat/peer confirm auto-deny
  unchanged. `background` does not alter `capability_of`.
- **Peer/heartbeat turns:** may start background tasks within their existing allow-band
  powers; the fold appears in every subsequent turn regardless of origin (same class as
  the todo-fold peer caveat, documented).

### Documented v1 edges

- Restart kills running bg tasks and drops finished results (in-memory only).
- A background `write_file`/`edit_file` completion does NOT update `file_versions_`
  (`BG_DONE` is not `TOOL_RESULT`) → the next edit of that file is refused stale → the
  agent re-reads. Self-healing, documented.
- A background `save_skill` completes without triggering the skills rescan (the rescan
  keys on the immediate started-`TOOL_RESULT`, which precedes the write). Backgrounding
  fast local tools is pointless — soul.md says so; not blocked by code.
- `stay_on_budget` unaffected (tools are not metered; a bg `ask_agent`'s cost lands on the
  peer).

### soul.md guidance

New short section: use `background:true` for long operations (builds, `ask_agent`
delegations, large fetches); results arrive in the "Background tasks" block on a later
turn — check it before re-running work; never background quick reads/writes.

## Testing

- Fake slow tool (a `sleep`-then-echo script) for offload e2e: result continues the turn;
  bus stays responsive (a second bus post dispatches while the tool runs).
- Stale-drop: abandon a turn mid-tool → `DROPPED_STALE_TOOL_RESULT`, no history append,
  no continuation; trailing `assistant(tool_calls)` popped.
- Version harvest on stale successful write.
- Invariant: `Tool { timeout_s > idle }` → `MalConfig` at boot.
- Background: started-result shape, `BG_DONE` → `BG_TASKS` → fold content, cap refusal,
  output truncation (UTF-8 boundary), no-executor fallback to inline foreground.
- Both lanes green (ASan+UBSan AND TSan) — the offload worker + `BG_DONE` path are
  TSan-relevant.

## Pieces

`src/apps/tool_runner/tool_runner.cpp` (+ `include/hades/module/tool_runner.h`):
offload, background registry, `BG_DONE`/`BG_TASKS`, schema append, strip, `Tools` cfg ·
`src/apps/arbiter/arbiter.cpp` (+ `include/hades/arbiter.h`): epoch stamp/gate, orphan
pop, `BG_TASKS` fold · `app/agent_wiring.cpp`: `Tools` block resolve, `set_executor`,
extended invariant, `kExecutorThreads` · `prompts/soul.md` · `docs/manifest-reference.md`
(Tools block section + invariant note) · tests as above.
