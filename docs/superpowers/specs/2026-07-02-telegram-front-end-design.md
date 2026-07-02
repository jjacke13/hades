# hades Telegram Front-End — Design

**Date:** 2026-07-02
**Status:** Approved (design review with Vaios, 2026-07-02)

## Goal

Talk to the hades agent from a phone: a **Telegram front-end module** alongside the stdin REPL
(ChatModule) and the web UI (HttpServerModule). Pattern set by Vaios: **one module per chat app**
(`Module = telegram` in the roster — enabled = used), so later apps (Signal/Matrix/Discord…) each
get their own module reusing the same seams.

**MOOS framing:** a comms-interface app — the same community, reachable over another radio
(pAcommsHandler analogue). The agent, its memory, and its safety gates are unchanged; only the
surface the human talks through is new.

## Decisions (from design review)

| Question | Decision |
|---|---|
| Architecture | **One module per app** (Vaios); Telegram first (pure HTTPS long-poll — no LAN exposure, no webhook/TLS setup) |
| Concurrency | **Simultaneous front-ends, shared turn-lock**: one `TurnGate` serializes whole turns across REPL + web + Telegram |
| Sessions | **One shared conversation** — all front-ends feed the same Arbiter history (phone ↔ browser continuity). Per-chat sessions = v2 |
| Auth | **Allowlist required, fail-fast**: `allow_users` numeric Telegram user IDs; module present without it → MalConfig. Non-allowed senders silently ignored (don't reveal the bot is alive) |
| Confirm gating | **Inline keyboard** `[Approve] [Deny]` → `callback_query` → `CONFIRM_RESPONSE` |
| Secrets | Bot token via **env var only** (`token_env`, default `TELEGRAM_BOT_TOKEN`); alongside `HADES_API_KEY` in a gitignored `.env` the user sources (systemd `EnvironmentFile=` later). Token added to Eventlog **redaction** (it appears inside API URLs) |

## Manifest

```
Module = telegram
Telegram
{
  token_env      = TELEGRAM_BOT_TOKEN     # name of the env var holding the bot token
  allow_users    = 123456789 987654321    # REQUIRED: whitespace-separated numeric user IDs
  poll_timeout_s = 50                     # getUpdates long-poll timeout (Telegram max 50)
}
```

- `token_env` unset in env → MalConfig at start (same as the LLM key).
- `allow_users` absent or empty while `Module = telegram` is rostered → **MalConfig** (pin_fact
  precedent: refuse to run silently-insecure — an open bot means anyone who finds the username
  can drive shell/write tools).

## Components

### 1. `TurnGate` (new, `include/hades/turn_gate.h`)

A tiny shared turn serializer: `struct TurnGate { std::mutex mu; };`. Owned by `Agent` as its
**first** member (destroyed last — outlives every module that holds a pointer). Wiring injects
`&gate` into ChatModule, HttpServerModule, and TelegramModule (`set_turn_gate`).

- **HttpServerModule:** its private `mu_` is **replaced** by the shared gate (same proven
  pattern — `handle_message`/`handle_confirm` lock the gate around post + collect). Null gate →
  a module-local fallback mutex (test overload unchanged).
- **ChatModule (REPL):** locks the gate around each turn (post `USER_MESSAGE` → `run_until`).
  Idle at the prompt holds nothing — Telegram/web turns proceed while the REPL waits for input.
- **Invariant preserved:** only one thread pumps at a time — the single-threaded-dispatch
  guarantee now rests on the shared gate instead of "one front-end per process".
- Known cosmetic effect (accepted): a Telegram-driven turn's `ASSISTANT_MESSAGE` also prints in
  an open REPL (subscribers run on the pumping thread) — cross-front-end echo.

### 2. `TelegramApi` seam (new, `include/hades/telegram/api.h`)

Interface so the module is testable without network (exact `HttpClient`-in-provider precedent):

```cpp
// One parsed update. kind=="message": text + from_id + chat_id set. kind=="callback":
// callback_id (answer handle) + callback_data ("approve:<id>" / "deny:<id>") + from_id set.
struct TgUpdate {
  long long   update_id = 0;
  std::string kind;            // "message" | "callback" (anything else was skipped at parse)
  long long   from_id = 0;     // sender user id (allowlist check)
  long long   chat_id = 0;     // where to reply
  std::string text;            // message text
  std::string callback_id;     // callback_query.id (for answer_callback)
  std::string callback_data;   // "approve:<confirm_id>" | "deny:<confirm_id>"
};
class TelegramApi {
 public:
  virtual ~TelegramApi() = default;
  virtual std::vector<TgUpdate> get_updates(long offset, double timeout_s) = 0;  // long-poll
  virtual bool send_message(long long chat_id, const std::string& text) = 0;
  virtual bool send_confirm(long long chat_id, const std::string& prompt,
                            const std::string& confirm_id) = 0;   // inline keyboard
  virtual void answer_callback(const std::string& callback_query_id) = 0;
};
```

Real impl `CprTelegramApi` (`src/telegram/cpr_telegram_api.cpp`): `https://api.telegram.org/bot<token>/…`
via cpr (already a dependency), nlohmann parse, **fail-soft** (network/parse errors → empty
result/false, never throw). Tests inject a scripted fake.

### 3. `TelegramModule` (new, `type() == "telegram"`)

- **on_start:** read config; resolve token from env (MalConfig if unset); parse+require
  `allow_users`; build `CprTelegramApi` unless a test injected one.
- **on_attach:** subscribe `ASSISTANT_MESSAGE` + `CONFIRM_REQUEST` (capture, HttpServerModule
  pattern). Does NOT start the thread.
- **start_polling():** called explicitly by hades_main (never from on_attach — no surprise
  threads in tests). Spawns the poll thread; **stop+notify+join in the dtor** (embedding-timer
  precedent, before the Blackboard dies).
- **Poll loop:** drain-and-**discard** the startup backlog first (stale commands queued while
  the agent was down must not replay — Telegram keeps updates 24h). Then loop `get_updates`:
  - update from a **non-allowed user → silently dropped** (no reply).
  - text message → lock `TurnGate` → reset capture state → post `USER_MESSAGE` (source
    `"telegram"`) → `run_until(reply || confirm, turn_idle_timeout)`; on idle-timeout post
    `TURN_ABANDONED` + pump (existing abandonment contract) and send `[timed out]`.
  - reply captured → `send_message`, **split at 4096 chars** (Telegram hard limit), plain text
    v1 (no parse_mode — no entity-parse failures).
  - confirm captured → `send_confirm` with inline keyboard; the pending confirm id is
    remembered by the module.
  - `callback_query` (from allowed user, matching the pending id) → `answer_callback` → lock
    gate → post `CONFIRM_RESPONSE{id, approved}` → `run_until` → send the resulting reply.
    Callback for an unknown/stale id → just `answer_callback` (dismiss the spinner).
  - Any API/network error → log to stderr, sleep 5s backoff, continue. The loop never throws.
- Update offset is in-memory only (v1); the startup drain makes restart behavior safe.

### 4. Wiring + hades_main

- `Agent.telegram` member (after `serve`, before `executor` — executor stays LAST);
  `TurnGate gate` becomes the Agent's FIRST member.
- Launcher factory `"telegram"`; `Telegram` block via `m.of("Telegram")`; wire in a `2e` step:
  `set_turn_gate` + `on_start` + `on_attach` (thread NOT started here).
- hades_main: token redacted in the Eventlog (like the LLM key, resolved before the Blackboard
  exists — but only when the roster lists telegram; absence of the env var must not break
  non-telegram manifests). After wiring: `if (agent.telegram) agent.telegram->start_polling();`
  then the existing REPL/`--serve` choice runs as today. **Telegram-only roster** (no chat, no
  --serve): main thread blocks waiting on the poll thread (Ctrl-C to exit).
- Test `build_agent` overload: `a.telegram` stays null (embedding/skills precedent) — existing
  tests unchanged.

### 5. dev.hades

Ships the block **commented** (like the embedding block used to be): Vaios uncomments + sets
his user id + exports `TELEGRAM_BOT_TOKEN` to go live. Keeps the default manifest runnable
without a bot token.

### 6. .gitignore

Add `.env` (the user keeps `HADES_API_KEY` + `TELEGRAM_BOT_TOKEN` in a sourced env file — must
never be committable).

## Error handling

- Fail-fast at launch: missing token env, missing/empty `allow_users`, whitespace-in-values
  where argv/list rules apply.
- Fail-soft at runtime: every poll-loop iteration wrapped try/catch; API errors → backoff;
  malformed updates skipped; send failures logged, turn still completes locally (history is
  already persisted by the Arbiter).
- Bus handlers (capture subscriptions) type-guarded, never throw (house rule).

## Testing

- **TurnGate:** two threads contend for turns → whole turns serialized (no interleaved pumps);
  REPL + serve + telegram wiring all lock the same gate.
- **TelegramModule (fake api):** allowed message → USER_MESSAGE posted + reply sent; non-allowed
  → nothing posted, nothing sent; backlog drained-discarded at start; 4096 split; confirm flow
  (CONFIRM_REQUEST → send_confirm; callback → CONFIRM_RESPONSE → final reply); stale callback →
  answered but no post; api errors → loop survives.
- **Wiring:** roster factory; MalConfig on missing allow_users / token env; telegram absent →
  null member; manifest lock test (dev.hades still parses, commented block).
- **HttpServerModule regression:** behavior identical through the shared gate.

## Deferred (v2)

- Per-chat sessions / threading (needs multi-session Arbiter).
- Markdown formatting, media (photos/documents/voice), webhook mode (needs public HTTPS).
- `/new` and `--resume` control from Telegram; offset persistence across restarts.
- Second app module (Signal/Matrix/Discord) reusing TurnGate + the api-seam pattern.
- Streaming replies (blocked on SSE/provider streaming work).
