# GET /history web re-render — design spec

**Date:** 2026-06-30
**Status:** approved (brainstorm via Q&A; design confirmed "all good"). Ready for plan.

## Goal

When `hades <manifest> --serve --resume` reloads a prior session, the agent regains full context but the
browser page starts **blank** (a documented gap from the session-resume feature). This adds a `GET /history`
JSON endpoint + a one-time fetch-on-load in the web UI so the resumed conversation is **rendered** in the
browser, including tool turns. The endpoint reads the persisted per-session jsonl from disk — fully
decoupled from the Arbiter (no cross-module coupling, no lock).

## Current state (what exists)

- **Session resume** persists the Arbiter's turn-by-turn `history_` to `.hades/sessions/<id>.jsonl`,
  **append-per-message** (one raw history message per line: `{role:user,content}`,
  `{role:assistant,content}`, `{role:assistant,content:null,tool_calls:[...]}`, `{role:tool,tool_call_id,
  content}`). The system prompt + retrieved memory are injected per-turn and are **never** stored, so the
  file is exactly the conversation. `--serve --resume` loads it into `history_` (agent has context) but the
  page does not fetch/render it — **silent context, blank page** (the explicit v1 deferral).
- **`HttpServerModule`** (`src/module/http_server_module.cpp`) serves the static UI from `webroot` (mounted
  at `/`) + a JSON API: `POST /chat`, `POST /confirm`, `GET /health`. It holds `bb_` and a `mu_` that
  serializes whole turns; `history_` is mutated **only** on the pump thread, and the pump runs only inside
  `collect_` under `mu_`. The module does **not** currently know the session path.
- **`authorize()`** (a `set_pre_routing_handler` chokepoint) is the CSRF seam: it requires an `X-Hades`
  header on `POST /chat` + `POST /confirm`; all GETs (incl. the static mount) are exempt so the UI loads.
- **`Arbiter::load_history()`** (`src/arbiter/arbiter.cpp`) already does a tolerant jsonl parse (skip
  blank/corrupt lines) + an orphan-pair sanitize (strip a leading `{role:tool}` and a trailing
  `{role:assistant,tool_calls}`) before the request is sent to the provider. That sanitize is for **LLM
  request validity** — not needed for display.
- **`web/app.js`** renders user/assistant bubbles via `addMessage(role, text)` and confirm gates via
  `addConfirm(id, prompt)`. It has **no** fetch-on-load. `postJson` already sends `X-Hades: 1`.
- **`web/style.css`** styles `.msg.user` / `.msg.assistant` / `.confirm`.

## Components

### 1. `read_session_jsonl(path) -> std::vector<nlohmann::json>` — pure tolerant reader
- New unit: `include/hades/session_history.h` + `src/core/session_history.cpp`.
- Reads the jsonl at `path`, one message per line, **tolerant**: skip blank lines and any line that does not
  parse to a JSON **object** (covers a corrupt interior line and a partial trailing line from a concurrent
  append). Returns the raw stored messages **as-is** — **no orphan-pair strip** (this feeds the browser for
  display, not a provider request; showing a dangling tool-call turn is harmless and even informative).
- `path` empty or file missing → returns an **empty vector** (not an error).
- **Optional targeted cleanup (in-scope if tests stay green):** refactor `Arbiter::load_history()` to call
  `read_session_jsonl(session_path_)` and then apply its existing leading/trailing orphan-strip — removing
  the duplicated tolerant-parse loop. Low risk (identical parse); gated behind the full suite staying green.
  If anything is unclear during implementation, keep `load_history` untouched and only add the new function.

### 2. `HttpServerModule::GET /history` + session-path member
- Add `void set_session_path(std::string p)` + member `std::string session_path_`.
- New route: `srv.Get("/history", ...)` → responds `{"history": read_session_jsonl(session_path_)}`
  (an empty path or missing file yields `{"history": []}`). `application/json`.
- **Threading:** the handler runs on the httplib thread and only **reads a file** — it touches no shared
  mutable module state, so it needs **no `mu_`**. The Arbiter may `append` to the same file on the pump
  thread concurrently; a reader that catches a half-written final line simply skips it (the tolerant parse).
  No lock, no coordination.

### 3. `authorize()` — gate `/history`
- Extend the chokepoint so `GET /history` requires the `X-Hades` header (in addition to the existing
  `POST /chat` + `POST /confirm`). A custom request header makes the GET a **non-simple** cross-origin
  request → the browser issues a preflight we never grant → a visited malicious site cannot send it at all.
  The static mount (`GET /`, `/app.js`, `/style.css`) stays exempt so the page loads.
- Keep the helper readable: the predicate is "require `X-Hades` when (`POST` and path is `/chat`|`/confirm`)
  **or** (path is `/history`)".

### 4. `wire_agent` — pass the resolved session path to the serve module
- In `app/agent_wiring.cpp` (or wherever the resolved session path is handed to the Arbiter), also call
  `agent.serve->set_session_path(<resolved session path>)` (null-guard `serve` — a roster may omit it).
  This is a **config string**, not a module coupling: the serve module reads the file, it does not call the
  Arbiter. The path is the same one already resolved for `arbiter->set_session_path(...)` — single source.

### 5. `web/app.js` — fetch-on-load + render
- On page load, `GET /history` with the `X-Hades` header (mirror `postJson`), then render each message **in
  order** by role:
  - `{role:"user", content:<string>}` → `addMessage('user', content)`
  - `{role:"assistant", content:<string>}` (non-null) → `addMessage('assistant', content)`
  - `{role:"assistant", tool_calls:[...]}` (content null/absent) → for each call `addToolCall(name, args)`
    (a **dim** entry; `name` from `tool_calls[i].function.name`, `args` from `.function.arguments`)
  - `{role:"tool", content:<string>}` → `addToolResult(content)` (a **dim** entry; **truncate** long
    content to a fixed cap, e.g. 500 chars, with an ellipsis)
  - any other shape → skip (tolerant; never throw)
- New render helpers `addToolCall(...)` / `addToolResult(...)` reuse the `log` container + `escapeText` +
  `scrollDown`. After rendering history, the live composer behaves exactly as today.
- A `/history` fetch failure → `addError('error: ...')` and the page stays usable (live chat still works).
- A fresh (non-resume) launch returns `{"history": []}` → nothing rendered → identical to today.

### 6. `web/style.css` — dim tool entries
- Add `.msg.tool-call` / `.msg.tool-result` (or a shared `.tool`) styling: dimmed/muted color, monospace,
  visually subordinate to user/assistant bubbles. Keep the existing dark terminal theme.

## Data flow

```
page load
  → GET /history  (header X-Hades: 1)
    → authorize(): path==/history requires X-Hades  → ok
    → handler: read_session_jsonl(session_path_)     → [ ...raw messages... ]
    → 200 {"history":[...]}
  → app.js: for each msg → addMessage / addToolCall / addToolResult (in order)
  → live composer continues (POST /chat, POST /confirm) as today
```

## Error handling

- No session path set (sessions absent from the roster) or file missing → `{"history": []}` → blank page
  (no regression vs today's behavior).
- Corrupt interior line / partial trailing line (concurrent append) → skipped by the tolerant parse.
- Large tool-result content (e.g. file contents) → **truncated in the render** (dim entry); payload itself
  is unbounded but this is a loopback localhost surface.
- `/history` fetch error in the browser → surfaced via `addError`, page remains interactive.
- The session file is conversation, not secrets (the api key never enters `history_`); it lives under the
  gitignored `.hades/`. Tool results may contain file/fetch content — same trust boundary as the Eventlog,
  and the endpoint is CSRF-gated.

## Testing (TDD)

- **Unit (`tests/test_session_history.cpp`, new):** `read_session_jsonl` round-trips a written jsonl into a
  vector of the exact messages; returns `[]` for a missing file and an empty path; **skips** a blank line, a
  corrupt/non-object interior line, and a truncated trailing line (no throw). Asserts no orphan-strip (a
  trailing `{role:assistant,tool_calls}` survives in the returned vector).
- **Serve (`tests/test_serve.cpp`, extend):** `GET /history` with `X-Hades` returns the persisted messages
  for a module whose `session_path_` points at a written jsonl; `GET /history` **without** `X-Hades` → 403
  (CSRF gate); an empty/unset path → `{"history": []}`.
- **Wiring (if a seam exists):** `build_agent` on a roster with `serve` + a session path sets the serve
  module's session path (assert via a getter or the endpoint). Otherwise covered by the serve test + manual.
- **If the `load_history` refactor is taken:** the existing **197** tests stay green (identical parse;
  orphan-strip preserved) — this is the gate for keeping the refactor.
- **JS render:** the web UI is static plain JS with **no** test harness; the `app.js` + `style.css` changes
  are verified by **inspection + a manual `--serve --resume`** smoke (documented in the task), not an
  automated test.

## Out of scope (noted)

- SSE/WebSocket streaming of the live turn (still whole-reply).
- A `/history` pagination/windowing param (full history returned; v1 sessions are small — YAGNI).
- A web `/sessions` list + switch (REPL-deferred too).
- Re-rendering on `/new` in the browser (the REPL rotates; the web page is not yet `/new`-aware).
- Persisting/displaying the ephemeral system-prompt / retrieved-memory blocks (never stored by design).

## MOOS-IvP framing

The session jsonl is the helm's resumable mission log; `GET /history` is the **operator console redrawing
the mission so far** when it reconnects — the console reads the log, it does not interrogate the helm. Same
separation as `hades-scope` reading the Eventlog offline: observability surfaces read the durable record,
they do not couple to the running community.
