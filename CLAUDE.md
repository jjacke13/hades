# hades — project notes (CLAUDE.md)

**hades** = an AI-agent harness in **C++20** that ports the **MOOS-IvP** robotics architecture to a
software, LLM-driven agent. Own git repo, branch `main`, no remote. Build/run only inside the Nix
dev shell (`nix develop`).

## The one idea
A central **Blackboard** (pub/sub: latest-value map + FIFO `pump()`) that modules talk through —
no module calls another directly. The **Arbiter** (the "helm") runs the per-turn loop: ask the LLM →
gate the proposed action through **Objectives** (veto/confirm) → run a tool or answer → loop tool
results back. Tools run as **isolated subprocesses**. See `docs/architecture.md` (Mermaid diagrams).

## MOOS-IvP ↔ hades
| MOOS-IvP | hades |
|---|---|
| MOOSDB | Blackboard (+ append-only Eventlog for history) |
| MOOS app | Module (LLMModule, ToolRunner, ChatModule, HttpServerModule, MemoryModule, Arbiter) |
| pHelmIvP (helm) | Arbiter (v1: LLM decides, objectives gate) |
| behavior | Objective (stay_on_budget, avoid_destructive) — competing goals of ONE agent |
| pAntler | Launcher (reads `Module=` roster, instantiates the module set) |
| .moos mission | Manifest (plain-text MOOS-style blocks, NOT TOML) |
| .alog / alogview | Eventlog / `hades-scope` |
| **a vehicle/community** | **one agent = Blackboard + Arbiter + modules** |
| pShare / pMOOSBridge | a **Bridge** module between Blackboards for agent↔agent (shipped 2026-07-03) |

**Personas/multi-agent:** 1 agent = 1 community (Blackboard+Arbiter+modules). Objectives are that
agent's goals, NOT other agents. More agents = replicate the community; bridge them with a `pShare`-style
Bridge. Levels: (1) separate manifests [today], (2) `/persona` switch, (3) a `Community` struct ×N +
router + Bridge [real multi-agent].

## Current state (2026-07-11)
`main` @ `23f2bd2` + `feat/voice-stt` + `feat/voice-tts` + `feat/bridge-protocol` (**voice STT + TTS shipped** — Telegram voice messages transcribed to text, and a voice-origin reply spoken back as a voice note; no new tool binary — plus the **bridge protocol**: card discovery + typed sharing between agents, see below) + a **heartbeat/cron** self-trigger (the agent runs its own turns on a schedule, see below) + **self-scheduling** (the agent creates its own cron/one-shot tasks at runtime via 3 tools, see below) + a **reactive when= trigger** (heartbeat entries + dynamic watches fire on a Blackboard condition, see below), **662/662 tests** (ASan+UBSan; TSan 614/614 as of feat/simplex — no new thread surface since; suite ~7s), ~9 MB RSS, **live** against PPQ (`gpt-5.5` LLM per dev.hades + `openai/text-embedding-3-small` embeddings; dev.hades ships Vaios's live two-agent bridge config → boot needs `HADES_BRIDGE_SECRET`).
Built: Blackboard+Eventlog · Arbiter v1 (veto/confirm gate, max-steps guard) · **18 tools**
(`fs_read shell write_file list_dir http_fetch save_memory core_memory use_skill save_skill ask_agent` + **dev tools**
`grep glob edit_file git_read run_command` + **self-scheduling** `schedule_task list_tasks cancel_task`, self-describing) · **tool capability
model** (`CapabilityPolicy` objective — scoped fs_read/fs_write/http_fetch/run_command allow/confirm/deny + git_read read-only, see below) + the older
destructive-pattern gate (`avoid_destructive`, kept as backstop) ·
**two memory layers** (core + archival, see below) · a **skills system** (loadable instruction packs, see below) ·
layered **system prompt** (SOUL/USER static +
live core MEMORY) · four front-ends: **stdin REPL** (libedit — arrows/history/Ctrl-A/E, colored
labels), **HTTP `--serve`** (browser web UI + JSON API, see below), a **Telegram bot** (long-poll,
allowlisted, see below), and a **SimpleX front-end** (local `simplex-chat` daemon over an in-house WS
client, see below) — all serialized by one shared **TurnGate** · an **agent↔agent Bridge**
(multi-agent: peer `ask_agent` + inbound `/ask` `/share`, see below) · **worker-offload concurrency**
(see below) · **manifest parser fails LOUD** on packed multi-kv lines (see below).

### Core memory v2 (shipped 2026-07-11, `feat/core-memory`) — bounded, editable core memory
Retires the append-only `pin_fact` for **`core_memory`** (clean break — repo unpublished; binary
`hades-core-memory`, `tools/core_memory_main.cpp`). Three actions on the same `memory/facts.md` line-file:
**`add`** (append a bullet, deduped), **`replace`** (`match` an existing line → new `text`), **`remove`**
(`match` → drop). The Arbiter's every-turn fold of `memory_file` is untouched — edits are live same-session.
- **Cap + consolidation forcing function (the point):** `Session.memory_char_limit` (default **2400** ≈ 600
  tokens, **Hermes-inspired** — the file is in EVERY turn's prompt, so it must stay small). An over-cap `add`/
  `replace` is **refused** with an error that **lists every current entry**, so the model consolidates (merge
  related, drop stale) and retries in the SAME turn — bounded core memory that curates itself instead of
  growing unbounded. `remove` is always allowed (it shrinks).
- **House rules honored:** empty-string args count as **absent** (the exactly-one-of empty-arg convention from
  self-scheduling — weak LLMs fill every schema field); non-string args fail closed; atomic tmp+rename write;
  newlines stripped; parent dir created. Capability = **MemoryAppend → always allow** (the agent's own file —
  curation must be frictionless). Wiring still **requires `Session.memory_file`** (`MalConfig` otherwise) and
  appends `<file> <cap>` to the tool argv (single source of truth). **Widened peer/heartbeat blast radius
  (spec-acknowledged):** allow-band now includes `remove`/`replace`, so a peer `/ask` or heartbeat turn can
  REWRITE or DELETE core memories, not just append — v2 fix = per-origin tool scopes (capability-v2 backlog).
- Pieces: `tools/core_memory_main.cpp`, `include/hades/memory_limit.h` (`kDefaultMemoryCharLimit = 2400`),
  `app/agent_wiring.cpp` (parse `memory_char_limit` + argv), `tests/test_core_memory_{tool,wiring}.cpp`.
  Spec/plan: `docs/superpowers/{specs/2026-07-11-core-memory-design.md,plans/2026-07-11-core-memory.md}`.
  **569/569 tests** green (ASan+UBSan). **Live-smoke pending** (Vaios: add/replace/remove + fill-to-cap → consolidation error).

### Worker-offload (shipped 2026-06-29) — bus stays single-threaded, blocking LLM call offloaded
The bus is **still single-threaded deterministic** (subscriber handlers run ONLY on the pump thread), but
`Blackboard::post()` is now **thread-safe** and there is a `run_until(pred, timeout_s)` event loop, an
`Executor` worker pool (`src/core/executor.cpp`), and **opt-in** `LLMModule::set_executor()`. When set
(the live Manifest path), the blocking `provider_->complete()` HTTP runs on a worker that `post()`s
`LLM_RESPONSE`/`BUDGET_SPENT_USD` back; `run_until` on the pump thread wakes and pumps it. With no
executor (the test `build_agent` overload) the LLM runs inline → all tests byte-identical. **Load-bearing
teardown:** the `Agent`'s `executor` is the **last** struct member (destroyed first → joins workers while
`llm`/`bb` still alive); `hades_main` declares `Blackboard bb` before `Agent agent`. Front-ends drive a
turn via `run_until` (REPL: `turn_done_` flag; HTTP `collect_`: `got_reply_||pending_confirm_`, 180s).
Pieces: `src/core/{blackboard,executor}.cpp`, `src/module/llm_module.cpp`, `app/agent_wiring.*`,
`tests/test_{blackboard,executor,llm_module,offload_e2e}.cpp`.

#### run_until follow-up (shipped 2026-06-30, `main` @ `64f12ca`, 132/132 + TSan clean)
Closed the worker-offload review's Important: **(1) turn-epoch** — Arbiter `++turn_epoch_` per `USER_MESSAGE`
(NOT on tool-loop continuations), stamps `LLM_REQUEST`, LLMModule echoes into `LLM_RESPONSE`, Arbiter drops
stale-epoch responses; **(2) race-free budget** — the offload worker captures only `provider_`/`bb_` (not
`this`) and posts ONLY `LLM_RESPONSE`; `spent_` accrues in a pump-thread `LLM_RESPONSE` handler (single
writer → no race even if calls overlap); **(3) idle timeout** — `pump()` returns a dispatch count and
`run_until` resets its deadline on progress, so a long-but-progressing turn no longer false-times-out (a
stalled turn still fires). **LOAD-BEARING INVARIANT — now CONFIGURABLE + ENFORCED (see Configurable-timeouts
below):** the idle ceiling (`turn_idle_timeout_s`, default **900s**) MUST stay > max single in-flight poster
(`llm_timeout_s`, default 600s; tool 30s inline) — guarantees no worker is alive when `run_until` abandons a
turn. As of the configurable-timeouts feature this is a HARD `MalConfig` at launch (`idle > llm`), not just a
doc note. The epoch was **defense-in-depth** with a dispatch-ordering hole (epoch bumped only on
`USER_MESSAGE` *dispatch*) — **NOW CLOSED** (shipped 2026-06-30, `ac635c9`): see the Turn-abandonment
section below.

#### Turn-abandonment hardening (shipped 2026-06-30, `ac635c9`) — closes the epoch dispatch-ordering hole
On `run_until` idle-timeout (a turn abandoned), the front-ends (both REPL loops + HTTP `collect_`) post a
`TURN_ABANDONED` bus message **and pump it before reading the next user input**, and surface `[timed out]`.
The Arbiter handles `TURN_ABANDONED` → `++turn_epoch_` + `clear_pending()`. So a stale post-abandonment
`LLM_RESPONSE{old_epoch}` is dropped by the existing epoch guard and can never contaminate a SUBSEQUENT
prompt — robust independent of timing (no longer reliant on the idle-timeout>poster invariant, though that
still holds). Why no simpler fix: a slow-legit vs stale-after-abandon response are identical to the Arbiter;
only the front-end knows a turn timed out, so an explicit abandonment signal is irreducible. The formerly-
DISABLED test is now ACTIVE (`Arbiter.StaleResponseAfterAbandonmentIsDropped`). Timeout seam
(`set_turn_timeout_s`/`set_collect_timeout_s`) — now also the live config hook (see Configurable-timeouts).
**Still deferred to tool-offload:** epoch-stamping `TOOL_RESULT` (tools run synchronously today → no stale
tool result; noted in `arbiter.cpp`). Pieces: `src/arbiter/arbiter.cpp` (handler+`clear_pending`),
`src/module/{chat,http_server}_module.cpp` (`abandon_turn_`), `docs/superpowers/*2026-06-30-turn-abandonment-epoch*`.

#### Configurable timeouts (shipped 2026-06-30, `1237ee5`) — manifest-tunable think-time
Two `Session`-block keys (defaults in `include/hades/timeouts.h`): **`llm_timeout_s`** (default **600**) →
the cpr per-call HTTP timeout (`cpr_http`, the real cap on one LLM "think"); **`turn_idle_timeout_s`**
(default **900**) → the front-ends' `run_until` IDLE ceiling (resets on every bus event, so it does NOT cap
total turn time — only a single silent stretch). **Enforced invariant:** `wire_agent` throws `MalConfig` at
launch (before key resolution/side effects) if `turn_idle_timeout_s <= llm_timeout_s` — a slow-but-alive call
must post back before the idle timer abandons the turn. (Necessary+sufficient because ONLY the LLM is
offloaded; tools run inline → can't trip the idle timer.) `--serve` httplib read/write socket timeouts are
set to `idle + 60` so a long turn's connection isn't dropped. Bad/garbage value → default (never 0). dev.hades
ships 600/900. Pieces: `include/hades/timeouts.h`, `src/module/llm_module.cpp` (cpr), `app/agent_wiring.cpp`
(read+validate+set), `src/module/{chat,http_server}_module.cpp` (`effective_*_timeout`), `docs/superpowers/*2026-06-30-configurable-timeouts*`.

#### Session resume (shipped 2026-06-30, `e80be5d`) — restart keeps the conversation
The Arbiter persists the turn-by-turn conversation (`history_`) per session to **`.hades/sessions/<id>.jsonl`**
(`id` = launch timestamp), **append-per-message** (`append_history` replaces the 4 raw `history_.push_back`
sites; UTF-8-replace dump → never throws; IO best-effort). `load_history` reloads it tolerantly (skips
corrupt/blank lines; **sanitizes BOTH a leading orphan `{role:tool}` and a trailing orphan
`assistant(tool_calls)`** from a mid-pair crash — else the resumed request is provider-invalid). **CLI:**
`hades <manifest> [--resume [id]]` — no flag → new session; `--resume` → newest `*.jsonl`; `--resume <id>`
→ specific (`MalConfig` if missing). Session paths are **collision-safe** (`unique_fresh_path` → `-N` suffix;
used by both the initial path and `/new`). **Overflow guard:** `start_turn()` sends only the most-recent
suffix of `history_` within `history_budget_chars` (Session block, default **120000** ≈ ~30k tokens),
**tool-pairing-safe** (never begins on an orphan tool; keeps the `assistant(tool_calls)+tool` pair even if
it alone exceeds budget). Full history stays in memory + on disk; only the request is bounded — this also
fixed a pre-existing unbounded-`history_` latent bug. **`/new`** REPL command → `NEW_SESSION` bus msg →
Arbiter clears history + `clear_pending()` + rotates to a fresh file + `++turn_epoch_` (drops a stale
old-session response); intercepted in both REPL loops (not a USER_MESSAGE). **Web UI:** `--serve --resume` now
**re-renders the resumed transcript** in the browser (see GET /history below — was "silent context, blank page"). Config:
`Session { sessions_dir = .hades/sessions, history_budget_chars = 120000 }`. Pieces:
`src/arbiter/arbiter.cpp` (append/load/window/NEW_SESSION), `include/hades/{session_id.h,history_budget.h}`,
`src/core/session_id.cpp`, `app/hades_main.cpp` (`--resume`), `src/module/chat_module.cpp` (`/new`),
`docs/superpowers/*2026-06-30-session-resume*`. **Deferred (v2):** embeddings
over the session-files corpus (the separate-files design enables it) · `/sessions` list+switch · retention/pruning.

#### GET /history web re-render (shipped 2026-06-30, `main` @ `e916084`, 204/204) — closes session-resume's web gap
`--serve --resume` no longer starts blank. **`GET /history`** (HttpServerModule) returns `{"history":[...raw stored
msgs...]}` read straight off the per-session jsonl via a new pure tolerant reader **`read_session_jsonl(path)`**
(`src/core/session_history.{h,cpp}`; skips blank/corrupt/partial lines; **no orphan-strip** — display, not an LLM
request; empty/missing path → `[]`). `Arbiter::load_history` was **deduped** to reuse it (then still applies its
boundary orphan-strips for provider validity). Endpoint is **disk-read only → no `mu_`** (concurrent pump-thread
append → tolerant parse skips a half-written final line) and **CSRF-gated**: `authorize` promoted to
`static HttpServerModule::authorize` + extended to require `X-Hades` on `GET /history` (static GET / stays exempt).
`web/app.js` **fetches `/history` on load** (with `X-Hades`) and renders user/assistant bubbles + **dim tool-call
(🔧) / tool-result (→, truncated 500c)** entries (all XSS-escaped); `web/style.css` dim `.tool-call`/`.tool-result`.
Wiring: `hades_main` `agent.serve->set_session_path(session_path)` (same path as the Arbiter). **LIVE-VALIDATED**
(Vaios smoked --serve --resume: transcript + dim tool turns render, CSRF 403/200, fresh stays blank). Pieces:
`src/core/session_history.cpp`, `src/module/http_server_module.cpp`, `app/hades_main.cpp`, `web/{app.js,style.css}`,
`tests/test_session_history.cpp` + `test_serve.cpp`, `docs/superpowers/*2026-06-30-get-history-web-render*`.
**Deferred (v2):** web `/new` re-render · `/history` pagination · `/sessions` list+switch.

### Web UI (shipped 2026-06-29) — `--serve` browser front-end
`hades <manifest> --serve [port]` runs `HttpServerModule`: serves static files from `web/` (mounted at
`/`) + the JSON API (`POST /chat`, `POST /confirm`, `GET /health`). Page (`web/{index.html,style.css,app.js}`,
dark terminal theme, plain JS no framework) hits `/chat`, renders user/assistant bubbles, Approve/Deny
for confirm-gated actions. Config: `Serve { host, port, webroot }` block (host default `127.0.0.1`; set
`0.0.0.0` for LAN). `resolve_serve_config()` (`src/config/serve_config.cpp`) resolves it; `--serve` port
optional (overrides block). **Security:** loopback default; **CSRF guard** — `authorize()` pre-routing
seam requires an `X-Hades` header on `POST /chat`+`/confirm` (a cross-origin "simple" request can't add
it without a preflight we never grant), blocking a visited website from driving the loopback agent;
static GET exempt. **No password auth by design** (user's private networking) — the `authorize()` seam
is the one-place add for it later. Seam also set for a future settings UI (`web/settings.html` +
`GET/POST /manifest` — deferred). SSE/WS streaming still deferred (replies arrive whole).

### Telegram front-end (shipped 2026-07-03) — long-poll bot, allowlisted, shares the TurnGate
`Module = telegram` + a `Telegram { token_env allow_users poll_timeout_s }` block adds a third front-end:
`TelegramModule` long-polls the Bot API (`getUpdates`) on **its own thread** and drives whole turns exactly
like the REPL/HTTP surfaces — lock the shared **TurnGate** → post `USER_MESSAGE` → `run_until(reply|confirm)`
→ `sendMessage` (split at 4096). **Concurrency model:** all three front-ends now serialize through ONE
`TurnGate` (`include/hades/turn_gate.h`, Agent's **FIRST** member → destroyed LAST), so the Telegram poll
thread + a `--serve` request + the REPL never pump the bus at once; an idle surface (long-polling / awaiting
a request / blocked on stdin) holds **nothing**. Injected into chat/serve/telegram **before** their
`on_attach` in `wire_agent`. **Turn-owner guard:** the module captures `ASSISTANT_MESSAGE`/`CONFIRM_REQUEST`
only while `my_turn_` (symmetric to ChatModule's stdin guard) — a REPL/web reply is not sent to Telegram.
**Security:** `allow_users` (numeric ids) is **REQUIRED** — `on_start` throws `MalConfig` without it (an open
bot = anyone who finds it can drive your agent); non-allowed senders are **silently dropped**. **v1 is
private-chat-only** (`chat_id == from_id` enforced; group messages dropped — replies would be group-readable).
Token via
`token_env` (default `TELEGRAM_BOT_TOKEN`) **only**, never in the manifest; `hades_main` **redacts** it in
`session.log` (best-effort resolve of the same env var). Keep it in a **gitignored `.env`** you `source`.
**Backlog discard:** the first `poll_once` drains-and-discards the startup backlog (`offset` advanced past it)
so a command queued while the agent was down never replays. **Confirm-gated** actions become an
inline-keyboard `[Approve]/[Deny]` message → `callback_query` → `CONFIRM_RESPONSE`. **Threading/lifecycle:**
the poll thread is started **explicitly** by `hades_main` (`start_polling()`, AFTER the graph is wired — never
in `on_attach`, so tests spawn no thread) and stop+joined in the dtor; `Agent::telegram` is the **LAST**
member → destroyed FIRST, so its dtor finishes any in-flight telegram turn while the Executor + every module
it touches are still alive (see the member comment). A telegram-only roster (no chat/serve) makes `hades_main`
block on `wait()`. Pieces: `src/module/telegram_module.cpp`, `include/hades/{module/telegram_module.h,
turn_gate.h,telegram/*}`, `src/telegram/*`, `app/{agent_wiring,hades_main}.*`, `tests/test_telegram_*.cpp`,
`tests/test_turn_gate.cpp`. **LIVE-VALIDATED 2026-07-03** (Vaios: real bot, phone→reply working).

### SimpleX front-end (shipped 2026-07-11, `feat/simplex`) — fourth front-end over a local simplex-chat daemon
`Module = simplex` + a `Simplex { host port allow_contacts auto_accept notify_contact connect_timeout_s }`
block adds a **fourth** front-end. It talks to a **local `simplex-chat` daemon** over its
**unauthenticated loopback WebSocket API** via an **in-house WS client** (no external WS lib) and drives
whole turns exactly like Telegram: lock the shared **TurnGate** → post `USER_MESSAGE` → `run_until(reply|
confirm)` → `send_text` (split at 4000). **No bot token** — the daemon's WS API is unauthenticated by
design, so it must stay loopback-bound (the security model is "don't expose the port", not a secret).
**Pieces (4 layers):** the **WS codec** (`src/apps/simplex/ws.cpp` frame encode/decode + `WsClient`,
`include/hades/simplex/ws.h`; tests `test_ws_frame`/`test_ws_client`), the **daemon seam** (`SimplexApi`
interface + `parse_simplex_events` + `WsSimplexApi`/`make_ws_simplex_api`, `include/hades/simplex/api.h`;
tests `test_simplex_parse`/`test_simplex_api`) — the TelegramApi precedent so the module is testable with a
scripted `FakeApi`, no socket — and the **module** (`SimplexModule`, `src/apps/simplex/simplex.cpp`,
`include/hades/module/simplex_module.h`; tests `test_simplex_module`/`test_simplex_wiring`).
- **Confirms are TEXT `y/N`** (SimpleX has no inline keyboards): a confirm-gated action prompts the
  contact; the **next message from that same contact** answers it (`y`/`yes` approves, else denies).
- **Security:** `allow_contacts` (comma-separated numeric contact ids **and/or** exact display names) is
  **REQUIRED** — `on_start` throws `MalConfig` without it; non-allowed senders are silently dropped.
  **Manual-accept is the default** (`auto_accept = false`); `auto_accept = true` is an explicit opt-in with
  a documented **name-spoof caveat** — with an open address a stranger can name themselves like an
  allowlisted display name, so **prefer numeric ids** (unspoofable). The Bridge is NEVER a SimpleX peer —
  agent↔agent stays text over its own transport.
- **Notify sink:** `SimplexModule` subscribes **`NOTIFY_USER`** and delivers to `notify_contact` (id resolves
  directly; a name resolves once that contact is seen in an event, else the notify is skipped with a log
  line) — the heartbeat notification path, mirroring Telegram. Both `telegram`+`simplex` rostered with notify
  targets → a `notify=true` heartbeat is delivered on **both**.
- **Reconnect loop:** the event thread reconnects with backoff (base `connect_timeout_s`) so the bot
  survives a daemon restart; the loop is interruptible so the dtor's stop+join never waits it out.
- **Threading/teardown:** the event thread is started EXPLICITLY by `hades_main` (`start()`, AFTER wiring —
  tests spawn no thread, open no socket) and stop+joined in the dtor. `Agent::simplex` is declared **AFTER
  `telegram` and BEFORE `heartbeat`** → teardown tail is **telegram → simplex → heartbeat** (heartbeat first
  so a tick's `NOTIFY_USER` can still reach this module, then simplex joins while the Executor + Arbiter +
  every module an in-flight turn touches are alive). A simplex-only roster (no chat/serve) makes `hades_main`
  block on `wait()`. Wired in `wire_agent` (roster factory `"simplex"`, `Simplex` block, gate + idle timeout)
  next to the Telegram block. Docs: `docs/manifest-reference.md` §16 (6-key table + setup walkthrough +
  security), §2 roster row.
- **Pi = the official aarch64 `simplex-chat` CLI binary as an external runtime dep** (`simplex-chat-ubuntu-2x_04-aarch64`);
  hades does not build/bundle it. Pieces: `src/apps/simplex/{ws,simplex}.cpp`,
  `include/hades/{simplex/{ws,api}.h,module/simplex_module.h}`, `app/{agent_wiring,hades_main}.*`,
  `tests/test_{ws_frame,ws_client,simplex_parse,simplex_api,simplex_module,simplex_wiring}.cpp`.
  **614/614 tests** green (ASan+UBSan) + **TSan 614/614** clean — written write-only (opus transcription),
  compiled later: FIRST build was clean, 613/613. The whole-branch review then caught **1 Critical the suite
  could not see** (C1: the NOTIFY_USER subscriber ran on the POSTING thread — the heartbeat timer — and sent
  over the same persistent socket the event thread was reading; telegram's inline notify is safe only because
  each send is a fresh stateless HTTP call). Fix `e546362`: the subscriber only queues under `notify_mu_`;
  `drain_notifies_()` at the top of `step_once()` sends on the event thread (single-socket-owner invariant;
  delivery lags ≤ ~25 s). Reviewer re-verified CONFIRMED FIXED. **Pattern note: any front-end holding a
  PERSISTENT connection must marshal NOTIFY_USER (and any cross-thread send) onto its own thread.**
- **LIVE-VALIDATED 2026-07-11** (Vaios, desktop): phone → SimpleX → hades reply worked end-to-end on the
  first live smoke after the C1 fix — the full in-house stack (RFC6455 codec → POSIX socket → event parse →
  allowlist → turn → send) against a real `simplex-chat -p 5225` daemon. **`allow_contacts` by DISPLAY NAME
  works** (`/contacts` in the daemon CLI shows only the name; auto_accept off → the name is trustworthy — no
  numeric id needed). The daemon CLI is NOT in nixpkgs (only the desktop app) → the flake ships
  `packages.x86_64-linux.simplex-chat-cli` (official v6.5.6 release binary, autoPatchelf'd: zlib/openssl/gmp/
  glibc) on the devShell PATH (`build: bfbfe8a`); the Pi uses the official aarch64 release binary directly.

### MCP discovery + remote transport (shipped 2026-07-12, `feat/mcp-discovery`)
MCP servers rostered as `Tool = <block> { mcp = <cmd> }` (stdio) or `{ mcp_url = <url>
api_key_env = <ENV> }` (Streamable HTTP, Bearer-only; OAuth servers → `npx -y mcp-remote <url>`
bridge on the stdio path) get their tools DISCOVERED at registry warm (`tools/list`, one
exchange per server) and announced to the LLM as **`<block>__<tool>`** (prefix = no
native-name shadowing; `.` illegal in OpenAI-compat function names). `inputSchema` passes
1:1 into the ToolSpec; specs flow through the existing `wire_agent` → `set_tools` path (zero
Arbiter changes). tools/call sends the server's OWN name via the registry's
`mcp_real_names_` map (never string-split). **Fail-soft:** discovery failure/empty → stderr
line + legacy call-by-block-name path, boot delay bounded per entry (stdio: one timeout_s;
http: worst case ~3× — initialize/request/teardown each bounded).
**Capability:** any `__`-containing name → `Capability::McpTool` → **confirm** by default
(heartbeat/peer auto-deny) unless listed in `capability_policy { mcp_allow = <block>__<tool>
… }` (whitespace list; literal `*` = all — trusts every rostered server). Launch gates
(`MalConfig`): exactly one of `native|mcp|mcp_url`; mcp block names `[A-Za-z0-9_-]{1,64}`
without `__`. HTTP transport: cpr POSTs, `Mcp-Session-Id` lifecycle, plain-JSON or
SSE-framed replies, redirects off, best-effort DELETE; `mcp_url` is operator-set → exempt
from the private-net gate (documented). Per-exchange one-shot (stdio spawn per call) kept —
v2 seam: warm persistent child. Docs: manifest-reference §4 MCP subsection + capability
rows. Pieces: `src/apps/tool_runner/tool_runner.cpp` (transports + discovery),
`include/hades/tool/{registry,mcp_adapter}.h`, `src/behaviors/capability_policy.cpp`
(McpTool), `app/agent_wiring.cpp` (validation + mcp_allow), `tests/test_mcp_{adapter,discovery}.cpp`,
`tests/test_wiring_mcp.cpp`, `tests/fake_mcp_server.sh`. dev.hades example NOT committed
(user's live file) — paste-ready blocks live in manifest-reference §4. **Two review-hardening
fixes landed post-plan:** discovered tool names are charset-gated `[A-Za-z0-9_-]{1,64}` (a
provider-illegal name is skipped, not left to 400 the whole tools array) and duplicate
discovered names dedup first-wins (a server listing a name twice announces it once).

### Voice STT (shipped 2026-07-05, `feat/voice-stt`) — a voice message becomes an ordinary turn
**LIVE-VALIDATED 2026-07-05** (Vaios: Telegram voice note → PPQ `nova-3` transcription → normal turn worked end-to-end).
**PPQ STT model = `nova-3`** (Deepgram Nova-3, PPQ's default), NOT `whisper-1` (that's OpenAI-proper) — the
`resolve_stt` default is `nova-3` as of `385a55e` (was whisper-1; a real bug — PPQ STT is Deepgram). **Telegram's
`.oga` (OGG/Opus) uploads fine to PPQ/Deepgram** despite ogg being absent from PPQ's documented format list —
no transcode needed (v2 fallback if a backend ever 415s: transcode `.oga`→wav). PPQ params: `file`/`model`/`language`
(`en`|`multi`); we omit `response_format` → default `json` → `{"text":…}`.
Speech-to-text so a Telegram **voice** message is transcribed to text and drives a normal turn. **Source-
agnostic provider seam** (Vaios's requirement — a clip transcribes the same from Telegram or a future local
mic), EXACTLY the embedding-provider precedent: one `SttProvider` interface, two transports —
**`provider = http`** (OpenAI-compat multipart `POST <base>/audio/transcriptions`, PPQ nova-3, DEFAULT, over
an injected `SttHttpClient` seam so tests use no socket) and **`provider = command`** (a local whisper/whisper.cpp
wrapper — `tools/whisper_reference.sh`, one-shot via `run_subprocess`, NO shell, transcript off stdout, audio path
appended last). **English-only v1** (`language = en`; http sends it as a form field, command bakes `-l en` into the
wrapper). **Opt-in: no `Stt` block → `Agent.stt == nullptr`, Telegram stays text-only** (no `Module =` line needed —
the block's presence is the switch; `resolve_stt` in `app/agent_wiring.cpp`). The seam is injected into **user-facing
front-ends only** — **the Bridge is NEVER given one** (agent↔agent stays text; a peer cannot send audio). TelegramModule
gained `SttProvider* stt_` + `handle_voice_`: on a `voice` update it `getFile`+downloads the `.oga`→temp→transcribe→
`USER_MESSAGE`, on the **poll thread** (off-bus, no Executor), **fail-soft** (any transcribe error → a "didn't catch that"
text reply, no turn; never crashes). **Teardown order:** `Agent::stt` is declared BEFORE `telegram` so it is destroyed
AFTER it — the telegram dtor joins the poll thread, which may be mid-`transcribe()` touching the provider (do NOT reorder).
`Stt { provider endpoint model api_key_env language timeout_s command }` block (all documented in `docs/manifest-reference.md`
§11); dev.hades ships it **COMMENTED** (text-only default runnable without a whisper backend). Pieces:
`src/stt/stt_providers.cpp`, `include/hades/stt/*`, `app/agent_wiring.cpp` (`resolve_stt` + inject), `src/apps/telegram/telegram.cpp`
(`handle_voice_`/`set_stt`), `tools/whisper_reference.sh`, `tests/test_stt_{providers,wiring}.cpp`. Spec/plan:
`docs/superpowers/{specs/2026-07-05-stt-voice-input-design.md,plans/2026-07-05-voice-stt.md}`. **TTS is the next voice half**
(agent reply → speech → Telegram `sendVoice`, separate later spec; provider TBD piper/API behind a seam like STT).
**Live-smoke pending** (Vaios: send a voice note to the bot with an uncommented `Stt` block + `HADES_API_KEY`).

### Voice TTS (shipped 2026-07-05, `feat/voice-tts`) — a voice-origin reply is spoken back
The other voice half: a Telegram **voice** message gets its reply spoken back as a voice note. **Mirror
modality** — only a **voice-origin** turn speaks (`speak_reply_` set in `handle_voice_`); a typed turn stays
text-only. **Source-agnostic provider seam**, EXACTLY the STT/embedding-provider precedent: one `TtsProvider`
interface (`include/hades/tts/*`, `src/tts/tts_providers.cpp`), two transports — **`provider = http`**
(OpenAI-compat JSON `POST <base>/audio/speech` with `{model,input,voice,response_format:"opus"}`, reuses the
`cpr_http` seam) and **`provider = command`** (a local piper/TTS wrapper — `tools/piper_reference.sh`, reply
TEXT on stdin → ogg-opus bytes on stdout, one-shot via `run_subprocess`, NO shell). **`response_format=opus` /
the wrapper's `ffmpeg -c:a libopus -f ogg`** because Telegram `sendVoice` requires **OGG/Opus**. **Text is the
anchor** — `send_reply_` sends the text first, THEN the voice note is **best-effort** (a synth/`sendVoice`
failure logs and leaves the text as the reply; whole speak path in try/catch — never crashes a turn). **`max_chars`**
(default **4000**) caps spoken length: a longer reply is text-only (no multi-minute synth of a code wall).
**Opt-in: no `Tts` block → `Agent.tts == nullptr`, agent never speaks** (no `Module =` line — the block's
presence is the switch; `resolve_tts` in `app/agent_wiring.cpp`, `max_chars` set via `set_tts_max_chars`).
Injected into **user-facing front-ends only** — **the Bridge is NEVER given one** (agent↔agent stays text; a
peer never receives audio). **Teardown order:** `Agent::tts` is declared BEFORE `telegram` (like `stt`) so it
is destroyed AFTER it — the telegram dtor joins the poll thread, which may be mid-`synthesize()` touching the
provider (do NOT reorder). `Tts { provider endpoint model voice api_key_env max_chars timeout_s command }` block
(documented in `docs/manifest-reference.md` §12); dev.hades ships it **COMMENTED** (text-only default runnable
without a TTS backend). Pieces: `src/tts/tts_providers.cpp`, `include/hades/tts/*`, `app/agent_wiring.cpp`
(`resolve_tts` + inject), `src/apps/telegram/telegram.cpp` (`handle_voice_`/`set_tts`/speak path + `send_voice`),
`include/hades/telegram/api.h` (`send_voice`), `tools/piper_reference.sh`, `tests/test_tts_{providers,wiring}.cpp`.
**Live-smoke pending** (Vaios: voice note to the bot with uncommented `Stt` + `Tts` blocks + a TTS-capable endpoint).

### Bridge protocol (card discovery + typed share) — shipped 2026-07-05, `feat/bridge-protocol`, 450/450 (TSan 132/132)
**LIVE-VALIDATED 2026-07-06 (Vaios, CROSS-MACHINE: desktop `hades1` 192.168.0.107 ↔ Pi Zero 2 W `pi0`
192.168.0.121, both 0.0.0.0:9090, aarch64 static build on the Pi):** "what can pi0 do?" answered STRAIGHT
OFF pi0's card — no tool call — with the real caps summary (`fs_read/fs_write: scoped, exec: none,
net: private-blocked` = exactly pi.hades's capability_policy); "ask pi0 what time it is" did the full
delegation round-trip and hit the **auto-deny path** (pi0's only time source = confirm-band `shell` →
auto-denied for peers, note propagated back) — the documented v1 edge: a peer gets exactly the receiver's
UNCONFIRMED powers. Fix-by-design: add `exec_allow = date` to pi0's capability_policy → `run_command date`
= ExecScoped allow AND the card flips to `exec: scoped` — **CONFIRMED live** (Vaios applied it; re-ask
returned pi0's UTC time via the delegation round-trip, no confirm prompt, no over-grant). NixOS desktop needed the firewall port opened
(`networking.firewall.allowedTCPPorts`); Pi OS Lite ships no firewall; `host = 0.0.0.0` on both (the
default 127.0.0.1 refuses LAN before any firewall matters).
Un-parks **NEXT direction 1** (bridge-as-protocol / standardize the blackboard vars) — agents now exchange
**structured** capability + fact state, not just session text. Backward-compatible (no protocol-aware peer →
old `/ask`+`/share` behavior unchanged). **Two channels:**
- **CARD (pull) — secret-gated `GET /card`.** Each bridged agent serves an A2A-shaped agent-card built on
  demand: `{name, description, url, version, capabilities:{streaming:false}, skills:[{id,description}],
  tools:[{name}], caps:{fs_read,fs_write,exec,net}}`. `skills` reverse-parsed from `SKILLS_ANNOUNCE`, `tools`
  from the roster, **`caps` is a SUMMARY of the `capability_policy` scopes — CATEGORIES ONLY**
  (`"scoped"`/`"none"`/`"public"`/`"private-blocked"`), **never literal fs paths or exec strings** (a peer
  can't learn your allowlist). A discovery timer re-pulls each `Peer`'s `/card` every `discover_interval_s`
  (default **300**; literal **`0` = boot-pull only**, no periodic thread) → posts `PEER.<peer>.card`.
- **TYPED `/share` (push).** The envelope gained a **`type`** field (absent → `"raw"`, legacy unchanged):
  `type=card` → `PEER.<from>.card` (**also the boot self-announce** — an agent pushes its own card to all
  peers on boot + whenever its skills change, so **discovery is boot-order-independent**); `type=fact` →
  `PEER.<from>.fact.<key> = {from,trust,text}` (trust-tiered); `type=raw` → `PEER.<from>.<key>` (legacy).
  Rename-on-arrival holds for ALL types (a peer can never write a non-`PEER.*` local bus key).
- **TRUST tiers:** a `Peer` block gains optional **`trust = trusted | untrusted`** (default `trusted`; all
  manifest peers today). A trusted peer's facts are labeled "`<peer>` reports:", untrusted → "unverified claim
  from `<peer>`:". Untrusted is the seam for future dynamic joiners (unused in v1 — all peers are allowlisted).
- **Consumption (Arbiter):** subscribes `PEER.*`, folds **two blocks** into the leading system message at turn
  start — **"Peers you can delegate to (use `ask_agent` by advertised capability):"** (from `PEER.*.card` —
  name + skills + caps → routes `ask_agent` by advertised capability, not blind) and **"Reported by peers
  (treat as claims, re-verify before acting):"** (from `PEER.*.fact.*`, trust-labeled). Both empty → nothing.
- **New manifest keys:** `Bridge.description` (card persona one-liner, default = the bridge name),
  `Bridge.discover_interval_s` (default 300, `0`=boot-only); `Peer.trust` (default `trusted`).
- **Security:** `/card` is secret-gated (**not public** — a public card + `/.well-known/agent.json` for real
  cross-harness A2A interop is deferred v2); `caps` is a summary so a peer never learns your literal allowlist;
  rename-on-arrival holds for every share type.
Docs: `docs/manifest-reference.md` §13 (card schema, `/share` `type` field, receiver bus vars, security);
`prompts/soul.md` "## Peers" (delegate-by-advertised-capability + treat "Reported by peers" as re-verify claims).

### Heartbeat / cron (self-triggered turns) — shipped 2026-07-07, `feat/heartbeat`, 473/473 (TSan 145/145)
**LIVE-VALIDATED 2026-07-07 on the Pi Zero 2 W (`pi0`, aarch64 static build):** an every-minute `Heartbeat = smoke`
(`notify = true`) fired a self-turn each minute and its reply arrived on Telegram — the full timer→gated-self-turn→
Arbiter→LLM→NOTIFY_USER→Telegram path, cross-machine. Confirmed the three modes from the `notify` flag: `notify=false`
= silent periodic autonomous work (tick runs a full turn incl. tool actions, reply dropped, no message); `notify=true`
= same + reply pushed to Telegram unless the agent replies `SILENT` (so "report only on exceptions" = notify=true +
"reply SILENT if all fine"). Telegram gotcha hit: **one bot token = one poller** — running telegram on both desktop
and Pi with the same `TELEGRAM_BOT_TOKEN` → `getUpdates` 409 conflict; use separate tokens or stop one poller (the
notify *send* doesn't conflict, only the two pollers). Autonomous-work caveat: a tick has no human → confirm-band
AUTO-DENIED → it gets only its UNCONFIRMED powers; give it `fs_write_allow`/`exec_allow` scopes to act unattended.
The **autonomy leg**: hades stops being purely event-driven and runs its OWN turns on a schedule. **Inert unless
the roster lists `Module = heartbeat`** (omit → `Agent.heartbeat==nullptr`, zero coupling; the test `build_agent`
overload is unaffected). `HeartbeatModule` (`src/apps/heartbeat/heartbeat.cpp`) owns a timer thread that wakes
**~every 30s** and, for each `Heartbeat = <name>` block whose **5-field cron** (`min hour dom month dow`;
`src/apps/heartbeat/cron.cpp`, pure `cron_matches`/`cron_valid`) matches the **machine-LOCAL** minute (deduped once
per minute via a year+yday+hour+min stamp), fires a **self-turn**.
- **A tick is a NORMAL gated turn** (`TURN_ORIGIN = heartbeat:<name>` then a `USER_MESSAGE` = the entry prompt):
  the full system prompt (soul + core memory + skills + `PEER.*` folds), all objectives (`capability_policy` /
  `avoid_destructive` / `stay_on_budget`), any tool. `PeerLoopGuard` blocks only `peer:*` origins, so **a heartbeat
  CAN delegate** via `ask_agent`. **Confirm-band actions are AUTO-DENIED** (no human to approve — mirrors the bridge
  peer-turn auto-deny). The Eventlog records every tick → `hades-scope` is the activity log.
- **Skip-if-busy, never queued:** the tick `try_lock`s the shared **TurnGate**; if a human/peer turn holds it the tick
  is **skipped** for that minute (`HEARTBEAT_SKIPPED`), never queued — heartbeats don't pile up behind a conversation.
- **notify (per-entry, default false):** `notify=false` → the reply is **dropped** (a silent scheduled task — the tool
  actions ARE the output). `notify=true` → the trimmed reply is posted to **`NOTIFY_USER`** and forwarded **unless it
  is empty or exactly `SILENT`** (the prompt convention: reply `SILENT` when there's nothing to report). **Sink =
  Telegram (v1):** `TelegramModule` subscribes `NOTIFY_USER` → `send_message` to `allow_users`. A `notify=true`
  heartbeat with **no telegram rostered** posts `NOTIFY_USER` but nothing delivers it — want notifications ⇒ roster
  telegram.
- **New bus keys:** `NOTIFY_USER` (`{text, from}`), `HEARTBEAT_SKIPPED` (entry name), `HEARTBEAT_ERROR` (name +
  reason); new `TURN_ORIGIN` value `heartbeat:<name>`.
- **Block keys** (`Heartbeat = <name>` block; parsed in `wire_agent`): `schedule` (REQUIRED; bad cron → `MalConfig`;
  supports `* N A-B A-B/N */N` + comma lists, AND across fields, minute resolution), `prompt` (inline) **OR**
  `prompt_file` (path; one REQUIRED; unreadable/empty → `MalConfig`), `notify` (bool). Teardown: the timer thread is
  started by `hades_main` (`start()`, AFTER wiring — tests spawn no thread) and stop+joined in the dtor. Docs:
  `docs/manifest-reference.md` §15; example prompt `prompts/daily_summary.txt`. Spec/plan:
  `docs/superpowers/{specs/2026-07-07-heartbeat-cron-design.md,plans/2026-07-07-heartbeat-cron.md}`.
- **Gotchas:** an inline `prompt` containing an `=` (e.g. `set x = 5`) trips the one-kv-per-line parser →
  `MalConfig`, binary refuses to boot ⇒ **use `prompt_file` for any prompt with an `=` or multiple lines** (cron
  values are `=`-free, always safe inline). Cron is **machine-local TZ** — set the box's TZ deliberately. `notify=true`
  delivers **only** if `telegram` is rostered. ~~v2 next: a reactive `when=` trigger~~ — **SHIPPED 2026-07-08**, see
  the Reactive when= trigger subsection below.
- **Live-smoke pending** (Vaios: roster `Module = heartbeat` + a `*/1 * * * *` entry, confirm the self-turn fires and
  `notify`/`SILENT` gate delivery).

### Reactive when= trigger (condition-fired self-turns) — shipped 2026-07-08, `feat/when-trigger`
The Monitor-style consumer deferred from bridge: a heartbeat entry (static OR dynamic) fires on a **Blackboard
condition** instead of a schedule — "watch pi0's card", "act when the budget passes 0.8".
- **Vocabulary (5 keyword forms, parser-safe — no `=`):** `when = KEY changes | KEY is <v> | KEY not <v> |
  KEY above <n> | KEY below <n>`. Pure lib `include/hades/heartbeat/when.h` + `src/apps/heartbeat/when.cpp`
  (`parse_when`/`when_valid`/`when_holds`; trailing-space trimmed, non-finite thresholds rejected). Any bus key
  watchable (incl. `PEER.*`).
- **Poll-at-tick, NEVER subscribe:** evaluated against `bb.get` latest-values on the existing ~30s scan (a bus
  subscriber runs on the pump thread mid-turn and cannot drive a turn) → **reaction latency ≤ ~30s**, documented.
- **Edge-triggered + cooldown:** `changes` arms on first observation, fires once per change; the holds-ops fire on the
  false→true edge, re-arm on false; already-true-at-boot fires once. **Busy-skip does not consume the edge** (retries
  next tick — the `fire_`-returns-ran seam). **`cooldown_s`** (default 60) suppresses re-fires; edges inside the
  cooldown are **absorbed, not queued** (corollary: an alarm that flapped true during a cooldown and stays true is
  edge-latched — no level-retrigger; doc'd in manifest-reference §15). No minute-stamp gate for when-entries.
- **Static + dynamic:** named `Heartbeat = <name> { when = … }` block (**`when` XOR `schedule`**, `MalConfig` on
  both/neither/malformed; optional `cooldown_s`) AND `schedule_task` gained `when` as the 4th exclusive timing kind
  (documented in its describe schema — LLM API surface, unlike expect_version; `cooldown_s` bounded 0..1e9 before the
  ll cast — the in_minutes UB class). Dynamic edge state survives the per-scan reload (`when_state_by_id_`, pruned to
  the active set); store records carry `when`/`cooldown_s` (old records fold tolerantly).
- **`SelfScheduleGuard` broadened (hole closed):** `peer:`-origin `schedule_task` is hard-vetoed **unconditionally**
  (even `allow_self_schedule=true`) — a peer must never plant standing work; `heartbeat:` origin still governed by the
  switch, human free.
- Pieces: `src/apps/heartbeat/{when.cpp,heartbeat.cpp}` (`maybe_fire_when_`/`sync_when_state_`),
  `include/hades/{heartbeat/when.h,module/heartbeat_module.h}`, `tools/{schedule_task,list_tasks}_main.cpp`,
  `src/behaviors/standard_behaviors.cpp`, `app/agent_wiring.cpp`. Spec/plan:
  `docs/superpowers/{specs/2026-07-08-when-trigger-design.md,plans/2026-07-08-when-trigger.md}`.
- **Live-smoke pending** (Vaios: `Heartbeat = watch { when = PEER.pi0.card changes … notify = true }` on the desktop,
  restart pi0 with a changed description → Telegram notify within ~5 min; or agent-driven "watch the budget").
- **v2 seams:** compound conditions (`&&`) · sub-30s latency · contains/regex match · watch TTL · level-retrigger opt-in.

### Self-scheduling (agent creates its own cron/one-shot tasks) — shipped 2026-07-07, `feat/self-scheduling`
The agent **schedules its own future turns** at runtime via 3 native tools — the runtime-mutable complement to the
manifest-static `Heartbeat = <name>` blocks (which are unchanged; dynamic + static coexist). Un-parks the CC
`CronCreate`/`CronList`/`CronDelete` analogue.
- **The constraint solved:** hades tools are **stateless subprocesses** — a tool can't reach the running `HeartbeatModule`.
  Bridge = a **persistence file `.hades/cron.jsonl`** (append-only, `add`/`cancel`/`done` records folded by id — the
  `memory.jsonl` pattern; path wired via argv, single source of truth). `HeartbeatModule` loads+**compacts on boot** and
  **re-reads it every ~30s tick-scan** (`reload_dynamic_`), so adds/cancels take effect within one scan, no restart.
  Dynamic tasks live in `dynamic_` (separate from static `entries_`); dedup keyed by task id across reloads
  (`last_fired_by_id_`, pruned to the active set).
- **A task is a PROMPT** (never a raw command — that would bypass the objective/gate layer + duplicate system cron). It
  fires as the SAME gated self-turn a static heartbeat does (`TURN_ORIGIN=heartbeat:<name>`, all objectives, confirm
  auto-denied). "Run a script" = a prompt that tells the agent to `run_command` (capability-gated).
- **3 tools** (`tools/{schedule_task,list_tasks,cancel_task}_main.cpp`, each compiles `cron_store.cpp`+`cron.cpp` into
  itself, no core link): **`schedule_task`** `{name, prompt, notify?, exactly one of schedule|in_minutes|at}` — cron
  (recurring, `cron_valid`) OR one-shot (`in_minutes` relative / `at` absolute machine-local `YYYY-MM-DDTHH:MM`|`HH:MM` →
  `fire_epoch`); **`list_tasks`** (active dynamic only — static entries are operator-owned, not in the store);
  **`cancel_task`** `{id}` → tombstone.
  - **Exactly-one-of gotcha (fixed 2026-07-10, `833b9aa`, live-smoke bug):** many LLMs fill EVERY schema property,
    sending `""` for the unused timing fields (and `0` for the number) alongside the real one → the old
    presence-by-type check counted 4 "present" fields → "provide exactly one" rejected every call → the model
    retried the identical bad call until max-tool-steps. Now an **empty string / non-positive `in_minutes` = absent**
    (`!str(k).empty()`; `in_minutes` requires `>0`). Any future exactly-one-of tool MUST treat empty as absent.
- **One-shot turns are NEW** (cron was recurring-only): fire once when `now_epoch >= fire_epoch` → the module appends a
  `done` record (folds away next reload). **Skip-if-busy is honored** — a one-shot due while the TurnGate is held is NOT
  tombstoned (retries next free tick; `fire_` returns whether it actually ran). **Catch-up on boot:** a one-shot whose
  time passed while the process was down fires once on the next post-boot scan.
- **Guardrails:** opt-in by **rostering** the tools + a `SelfScheduleGuard` objective (PeerLoopGuard mirror,
  `src/behaviors/standard_behaviors.cpp`) that **hard-vetoes `schedule_task` on a `heartbeat:`-origin turn unless
  `allow_self_schedule=true`** (default false → a tick can't breed more ticks; a **human**-origin turn always may). Caps
  `max_tasks` (default 20) + `min_interval_s` (one-shot floor, default 60) enforced in the tool; `stay_on_budget` still
  bounds cost. New `Capability::SelfSchedule` (→ allow; the guard + caps gate). `MalConfig` if `schedule_task` is rostered
  without `Module = heartbeat` (silent no-op otherwise).
- **Config = the UNNAMED `Heartbeat { cron_store allow_self_schedule max_tasks min_interval_s }` block** (a NAMED
  `Heartbeat = <name>` block stays a static entry; the entry-parse loop skips `name==""`). Docs: `docs/manifest-reference.md`
  §15 self-scheduling subsection; `prompts/soul.md` "## Scheduling your own work". Spec/plan:
  `docs/superpowers/{specs/2026-07-07-self-scheduling-design.md,plans/2026-07-07-self-scheduling.md}`. Pieces:
  `include/hades/heartbeat/cron_store.h`+`src/apps/heartbeat/cron_store.cpp` (pure fold/compact/serialize/parse_at/id),
  `tools/{schedule_task,list_tasks,cancel_task}_main.cpp`, `include/hades/objective/self_schedule_guard.h`,
  `src/apps/heartbeat/heartbeat.cpp` (reload/one-shot), `app/agent_wiring.cpp` (config split + argv + guard).
- **Live-smoke pending** (Vaios: roster the 3 tools + `Module = heartbeat`; "remind me in 2 min", `list_tasks`, `cancel_task`).
- **v2 seams:** raw-`command` kind (deliberately excluded — gate-preserving); static entries in `list_tasks`; `tz` key for
  `at`; store compaction beyond boot; staleness-drop for long-overdue one-shots. ~~reactive when= watch~~ — SHIPPED 2026-07-08 (see below).

### Two memory layers (MemGPT-style, both agent-writable)

### Two memory layers (MemGPT-style, both agent-writable)
- **Archival / searchable** — `save_memory` tool → `.hades/memory.jsonl` (append-only). MemoryModule
  (`type()=="memory"`) keyword-ranks it each turn (`rank_memories`, pure; **v2 seam = embeddings**) and
  posts `RETRIEVED_MEMORY`; Arbiter injects it as an **ephemeral** `{role:system}` labeled memory block
  before the last user msg (see Memory-injection framing below). Config: `Memory { store=… top_n=… }`. **LIVE-VALIDATED** (save→restart→recall).
- **Core / always-on** — `core_memory` tool (`add`/`replace`/`remove`) → `memory/facts.md` (line-edited,
  newlines stripped, atomic tmp+rename, parent dir created). **Char-capped** (`Session.memory_char_limit`,
  default 2400): an over-cap write is refused with the full entry list so the agent consolidates in the SAME
  turn. The Arbiter **re-reads this file every turn** (`read_memory_layer`) and folds it into the **leading**
  `{role:system}` message (after static SOUL/USER) — live same-session. Config: Session
  `memory_file = memory/facts.md`; wiring **requires memory_file when core_memory is present** (MalConfig
  fail-fast) and appends the path + cap to the tool argv (single source of truth).
Pieces: `src/memory/{rank,store}.cpp`, `src/module/memory_module.cpp`, `src/config/prompt.cpp`
(`assemble_system_prompt`=SOUL+USER, `read_memory_layer`=live core), `tools/{save_memory,core_memory}_main.cpp`.

### Memory embeddings (P1+P2, shipped 2026-06-30, `main` @ `20ba94c`, 247/247) — opt-in semantic recall
A third, **opt-in** memory path that semantic-ranks the corpus instead of keyword-matching it. **LIVE-VALIDATED**
(Vaios, PPQ `openai/text-embedding-3-small`, 1536-dim: indexed 2 memories + 10 session turns → `EMBED_INDEX_DONE=true`,
populated `.hades/embeddings/memory.vec.jsonl`).
**Inert unless the manifest roster lists `Module = embedding_memory`** (omit → `Agent.embedding==nullptr`;
dev.hades ships the block COMMENTED so it stays keyword-by-default + runnable without an embedder).
- **`EmbeddingMemoryModule`** (`type()=="embedding_memory"`, `src/module/embedding_memory_module.cpp`):
  on `USER_MESSAGE` embeds the query (warm provider), cosine-ranks the `VectorCache` above `min_similarity`,
  then **splits hits by `src`** → posts `RETRIEVED_MEMORY_SEMANTIC` (archival fact hits) +
  `RETRIEVED_SESSION_SEMANTIC` (past-session excerpts); the Arbiter injects **two labeled sub-blocks** (see
  Memory-injection framing below). Corpus indexed **incrementally** (`index_archival`, stable `memory#i` ids, batched).
- **Providers** (`src/embedding/`): **subprocess** (warm process, one JSON line in/out — see the reference
  embedder `tools/embed_reference.py`, sentence-transformers `all-MiniLM-L6-v2`) **or** **http**
  (OpenAI-compat `/embeddings` — recommended local backends **ollama** `nomic-embed-text` + **llama.cpp**
  `llama-server --embedding`, both documented in `embed_reference.py` + the dev.hades comment).
- **`VectorCache`** is **model-stamped** (`.hades/embeddings/memory.vec.jsonl`): a stamp mismatch → rebuild
  (never compares incomparable vectors). **Fail-soft everywhere** — any embedder error degrades to keyword-only
  (`RETRIEVED_MEMORY_SEMANTIC=""`), never crashes a turn (whole `USER_MESSAGE` handler in try/catch).
- **Wiring** (`app/agent_wiring.{h,cpp}`): `Agent.embedding` member sits among the modules (destroyed before
  `executor`/Blackboard); attached **before the Arbiter** (its semantic post lands on the same pump before
  `start_turn`); the **Executor is set before `on_attach`** so the index runs OFF the bus (executor now
  created before `wire_agent`). Config = `Embedding` block (`provider/command/endpoint/model/cache_dir/
  memory_store/top_n/min_similarity/batch_size/timeout_s`). The test `build_agent` overload leaves
  `embedding` null → existing tests unaffected.
- **P2 (sessions + periodic, shipped):** the past-**session corpus** is indexed too (`extract_session_turns` →
  per-turn `"U: …\nA: …"` units; `index_sessions`, `src="session"`), **live session EXCLUDED** by
  canonical-path compare (`live_session_path_`, set BEFORE `on_attach` via `wire_agent`/`build_agent(session_path)`
  → happens-before the index worker, race-free). A **periodic reindex timer** (`reindex_interval_s`, default
  **86400**=daily; `0`=off) re-runs the incremental index; its `std::thread` is stop+notify+joined in the module
  dtor (before bb dies); concurrent runs serialized by `index_mu_` (no double-append). `run_index_` is
  try/catch-guarded (no `std::terminate`).
Pieces: `src/module/embedding_memory_module.cpp`, `src/embedding/{vec_math,http_embedding_provider,persistent_child,
subprocess_embedding_provider,vector_cache,indexer,session_turns}.cpp`, `include/hades/embedding/*`,
`tools/embed_reference.py`, `tests/test_embedding_{vec_math,memory_module,wiring}.cpp`, `test_{vector_cache,indexer,session_turns,http_embedding_provider,subprocess_embedding_provider}.cpp`.
**Deferred (v2, near-future per Vaios):** **switch the flat `.vec.jsonl` to sqlite + binary vectors (+ ANN index
when the corpus grows)** — the `VectorCache` is the drop-in seam (module/Arbiter untouched). Today = flat
append-only jsonl + brute-force cosine (fine at hundreds–thousands of records; loads whole cache/query). Also:
`dimensions` request param (smaller/cheaper vectors); embed-cost metering (currently untracked by the budget objective).

### Memory-injection framing (shipped 2026-07-01, `main` @ `678a248`, 251/251) — recall reads as the agent's own
Fixes a live bug: the injected memory reached the prompt but the LLM discounted it ("this is our first exchange" /
"you're quoting back a response"). Now the Arbiter injects **two labeled sub-blocks** instead of one bare
`"Relevant memories:"` list: **"Facts from your memory (you saved these earlier; treat as reliable):"**
(= `merge_dedup(RETRIEVED_MEMORY keyword + RETRIEVED_MEMORY_SEMANTIC)`) then **"Excerpts from earlier sessions with
this same user … do NOT say this is a first exchange … may be out of date — re-verify current state before asserting
a past action's result still holds:"** (= `RETRIEVED_SESSION_SEMANTIC`). Facts-first, `\n\n`-joined, both-empty → no
block (backward-compat). To split cleanly, `VectorCache::query` now returns each hit's `src` and the module partitions
its hits into the two keys (`src=="session"`→session, memory/unknown→facts). `prompts/soul.md` corrected (the stale
"keyword-based, not semantic" line) + a standing paragraph telling the model the block IS its own recall (re-verify
stale actions). Pieces: `src/arbiter/arbiter.cpp` (two-block inject), `src/module/embedding_memory_module.cpp` (2-key
split), `src/embedding/vector_cache.{h,cpp}` (`ScoredMemory.src`), `prompts/soul.md`,
`docs/superpowers/*2026-07-01-memory-injection-framing*`. **Live-smoke pending** (Vaios: re-ask a past-session topic).

### Skills system (shipped 2026-07-02) — loadable instruction packs (à la Claude Code skills)
A **skill** = a `<skills_dir>/<name>/SKILL.md` (YAML front-matter `description:` + body instructions; may bundle
scripts). **Inert unless the manifest roster lists `Module = skills`** (dev.hades ships it); omit → `Agent.skills==nullptr`,
zero coupling (the test `build_agent` overload without the module is unaffected). Flow:
- **`SkillsModule`** (`type()=="skills"`, `src/module/skills_module.cpp`): scans the dir (`scan_skills_dir`,
  `src/skills/scan.*`), posts a one-line-per-skill roster on **`SKILLS_ANNOUNCE`** — **event-driven, no per-turn scan**:
  once at `on_attach` (post updates the latest-value map immediately, so the first `start_turn` sees it) and
  **rescans only on a successful `save_skill`** (tracks pending `TOOL_REQUEST{tool=save_skill}` ids → re-announces on
  its `TOOL_RESULT{ok:true}`). Config: `Skills { dir = skills }` (default `skills`).
- **Arbiter fold** (`src/arbiter/arbiter.cpp`): `bb_->get("SKILLS_ANNOUNCE")` (latest-value) folded into the **leading
  `{role:system}` message** (after SOUL/USER + live core MEMORY) as an "Available skills" list — so the LLM knows the
  library each turn without a scan.
- **Two native tools** (isolated subprocesses, self-describing): **`use_skill`** (`tools/use_skill_main.cpp`) loads
  `<dir>/<name>/SKILL.md`; **`save_skill`** (`tools/save_skill_main.cpp`) writes it (**atomic** temp-file+rename so a
  concurrent scan never sees a torn file; newlines→spaces so one skill = one announce line). Both gate the **name** to
  `[A-Za-z0-9_-]{1,64}` (`scan.h`) — no path separators/dots → no traversal outside the dir. The **skills dir is fixed by
  argv** (wiring appends the resolved dir, single source of truth — the LLM can't redirect it).
- **Capability model:** `capability_of` maps `use_skill→SkillRead`, `save_skill→SkillWrite`, both **allow** by default
  (kept as distinct enums so a future policy can confirm-gate `SkillWrite`).
- **`skills/` is git-tracked** (like `memory/facts.md`): the agent authors skills at runtime → working-tree churn to
  review/commit as curated standing skills (or gitignore it).
Pieces: `src/module/skills_module.cpp`, `src/skills/scan.cpp`, `include/hades/skills/scan.h`, `tools/{use_skill,save_skill}_main.cpp`,
`app/agent_wiring.cpp` (roster factory + `Skills` block + dir argv), `src/objective/capability_policy.cpp`,
`tests/test_skills_*.cpp`. Spec/plan: `docs/superpowers/specs/2026-07-02-skills-system-design.md`,
`docs/superpowers/plans/2026-07-02-skills-system.md`.
**Skills v2 idea-list (Vaios 2026-07-02: v1 stays AS-IS short/mid-term — these are parked, not planned):**
1. `delete_skill` / rename tooling (today: user deletes the dir by hand).
2. Relevance hints — embedding module suggests "skill X may apply" alongside memory retrieval.
3. Per-skill capability scopes; **confirm-gate `SkillWrite`** by policy (enum split already supports it, zero code).
4. Skill-declared first-class tools (dynamic tool registration when a skill loads).
5. Announce pagination/grouping once the library grows past dozens of skills.
5b. ~~`save_skill` patch mode~~ — **SHIPPED 2026-07-12** (`feat/save-skill-patch`): save_skill
   gained optional `old_string`/`new_string` (empty=absent mode select; match EXACTLY ONCE, no
   replace_all; post-patch frontmatter validation via the scanner's own `parse_skill_description`
   → a patch that would brick the skill out of the roster is refused, file untouched; atomic
   write; rescan/capability/wiring unchanged — rescan keys on tool name + ok, SkillWrite still
   allow, dir still argv[1]). No staleness expect_version (v1): stale old_string fails the
   live-content match and the error says to re-read. `parse_skill_description` promoted to inline
   in `include/hades/skills/scan.h` (the `valid_skill_name` pattern) so the tool validates with
   the scanner's own parse without linking core. Spec:
   `docs/superpowers/specs/2026-07-12-save-skill-patch-design.md`.
6. From the final review (recorded, non-blocking): symlink-follow in the tools (lexical-not-realpath — same
   documented v1 gap as capability_policy); `avoid_destructive` pattern-scans save_skill BODIES → a skill
   documenting `rm -rf` confirm-gates on save (safe, maybe desirable; exclude skill tools from the arg-scan if
   annoying); `pending_saves_` id leak if tool-offload ever breaks the request→result invariant; possible move
   of `skills/` under `.hades/` (one-line `Skills { dir }` change, no code — Vaios may relocate later).

### Bridge / multi-agent (shipped 2026-07-03, `feat/bridge`) — agent↔agent, pShare-style
Un-parks the Bridge. 1 agent = 1 community (Blackboard+Arbiter+modules); a **BridgeModule** gives it a small
HTTP surface so peers can ask it questions and push it shared variables. **Inert unless the manifest rosters
`Module = bridge`** (omit → `Agent.bridge==nullptr`, zero coupling; the test `build_agent` overload is
unaffected). dev.hades ships the block **COMMENTED** (a single-agent default has no peer — uncommenting on
each machine is the deploy story).
- **Inbound** (`BridgeModule`, `type()=="bridge"`, `src/module/bridge_module.cpp`): an httplib listener on its
  **own thread** exposes **`POST /ask`** and **`POST /share`**. Both are **auth-gated** (shared `secret_env`
  header) + **peer-allowlisted** (`from` must be a configured `Peer` name) → **403** on auth failure (bad secret,
  unknown peer → 403; a malformed body returns 200 + `{ok:false,error}`). `/ask` drives a **normal turn** through THIS agent (lock the
  shared **TurnGate** → post `USER_MESSAGE` prefixed `(from peer agent "name")` → `run_until` → reply); the turn
  passes **this agent's own objectives/gates** (a peer's request is not privileged). `/share` stores the payload
  under **`PEER.<from>.<key>`** (fixed v1 rename-on-arrival, collision-proof — a peer can never inject a local
  bus key; no turn, thread-safe `post()`).
- **Confirm auto-deny:** a confirm-band action inside a peer-driven turn is **auto-denied** with an explanatory
  note appended to the reply (peers cannot approve confirmation prompts — a worker's risky powers come from its
  OWN manifest allow-scopes, set at deploy). `denied_confirm_` tracked per MY turn.
- **LIVE-VALIDATED 2026-07-04** (Vaios, two agents, shared secret): `ask hades2 what time it is` → full
  round-trip worked — delegation out (`ask_agent`→hades2 `/ask`), peer turn on hades2, reply back, asker
  summarized. First smoke exercised the **auto-deny path** (hades2's only answer was the confirm-band `shell`
  tool → auto-denied + note propagated); after a fix it answered the time, a **memory query** (peer turn ran
  hades2's full memory stack — core `facts.md` + archival recall + injection — and returned it), and free-form
  cooperation. Two-agent team confirmed live.
- **v1 confirm-band UX edge (deploy story as designed, but sharp):** a peer gets NOTHING that needs a
  confirm-band tool on the receiver — a peer's powers are exactly the receiver's *unconfirmed* powers.
  `capability_policy` has **no allow-key for `shell`/Exec, `write_file`/FsWrite, or Unknown tools — those are
  ALWAYS confirm** (only FsRead/FsDeny/Net are scope-tunable). To let peers use a worker's tools: give the
  worker a non-confirm path (a dedicated tool + an `allow` row in `capability_of`), OR remove
  `capability_policy` on a deliberately locked-down worker (see the SECRET-EXPOSURE caveat next).
- **SECURITY — a peer can read out whatever the receiver will put in a plain answer.** `/ask` drives a normal
  turn, so anything that reaches the LLM as context (injected **core+archival memory**, folded skills roster,
  file contents already read) can be spoken back to a peer with **no tool call → no capability gate**. The
  memory-query smoke proved it: a peer extracted hades2's pinned facts + archival memories. Only the receiver's
  **soul/persona + objectives** guard this, NOT `capability_policy` (which gates tool *actions*, not answers).
  Corollary: **removing `capability_policy` on a bridged worker is dangerous** — then a peer can also
  `fs_read .env`/`~/.ssh/id_rsa` (the `fs_deny` hard-veto is gone) and get secrets back. Keep it on any
  peer-exposed agent; don't put anything peer-secret in a bridged agent's memory/reachable files. v2 seam:
  a per-peer "what may I answer" policy / memory-scope for peer turns.
- **Outbound / delegation:** the **`ask_agent`** native tool (`tools/ask_agent_main.cpp`, isolated subprocess,
  self-describing — its **description names the known peers** so the LLM sees who it can delegate to) POSTs
  `/ask` to a peer and returns the reply as the tool result. Per-tool **`timeout_s`** override in the `Tool`
  block (default from `include/hades/timeouts.h`).
- **Loop protection:** **`PeerLoopGuard`** objective (`src/objective/peer_loop_guard.cpp`) **hard-vetoes
  `ask_agent`** whenever the current turn's **`TURN_ORIGIN`** is `peer:<name>` — so an agent answering a peer
  can't forward the request onward (v1 `max_hops = 1`; the wire `hops` field is the multi-hop v2 seam). Convention:
  every front-end posts `TURN_ORIGIN` at turn start — `human` (chat/serve/telegram), `peer:<name>` (bridge).
  Auto-registered in `wire_agent` when the bridge is present.
- **Security:** the bridge secret is via **`secret_env`** (default `HADES_BRIDGE_SECRET`) **only**, never in the
  manifest; `hades_main` **redacts** it in `session.log`. `host` default `127.0.0.1` (set `0.0.0.0` for LAN);
  **`port = 0` → ephemeral bind** (tests use it so parallel runs never collide).
- **Wiring/teardown:** `Agent.bridge` sits just before `telegram` in the member list → **destroyed order is
  `…executor, bridge, telegram`** (telegram first, then bridge — its dtor stops+joins the listener thread while
  the Executor + modules it touches are still alive), all after the plain modules. The listener is started by
  `hades_main` after wiring (like telegram), not in `on_attach` (tests spawn no thread). Config: `Bridge
  { name host port secret_env share_out max_hops ask_timeout_s }` + one `Peer = <name> { url }` per peer.
Pieces: `src/module/bridge_module.cpp`, `src/bridge/{protocol,cpr_bridge_http}.cpp`, `src/objective/peer_loop_guard.cpp`,
`include/hades/{bridge/*,module/bridge_module.h,objective/peer_loop_guard.h}`, `tools/ask_agent_main.cpp`,
`app/{agent_wiring,hades_main}.*`, `tests/test_{bridge_module,bridge_protocol,bridge_wiring,ask_agent_tool,peer_loop_guard}.cpp`.
Spec/plan: `docs/superpowers/specs/2026-07-03-bridge-multi-agent-design.md`, `docs/superpowers/plans/2026-07-03-bridge-multi-agent.md`.
**v2 seams (recorded, not built):** per-peer secrets · per-peer share lists · per-peer confirm policy (propagate
to the asker's human) · per-key rename (full pShare) · `max_hops > 1` (wire field already present) · transport
behind a small interface (queue/webhook — **hyperdht-cpp** the self-host-native P2P option, no public IP/certs) ·
discovery (static roster now, registry/mDNS later) · inbound share whitelist · rate limiting · peer presence via
`/health` polling · ask offload (with tool-offload, un-freezing the asker during a peer turn). **Two new v2 ideas
(Vaios↔hades convo 2026-07-05):** (1) **belief-as-report provenance** — a peer's `ask_agent` reply / `/share`
value is a *report from peer X*, NOT local truth; provenance-tag it + re-verify before acting (extends the Bridge
SECURITY note; today the asking LLM can treat a peer reply as fact). (2) **cross-agent safety veto / quorum** — a
dedicated *safety agent* vetoing another agent's action, or a consensus check across agents. NOT possible today —
objectives are strictly per-agent (one helm); a cross-agent veto is a new architecture layer, not a bridge tweak.

## Build / run
```bash
export HADES_API_KEY=<key>                                   # key never in the manifest
nix develop --command cmake -S . -B build -G Ninja           # configure (once)
nix develop --command cmake --build build                    # build
nix develop --command ctest --test-dir build                 # test (558/558, ~5.9s)
nix develop --command ./build/hades manifests/dev.hades --serve      # web UI -> http://localhost:8080/
nix develop --command ./build/hades manifests/dev.hades             # chat REPL
nix develop --command ./build/hades manifests/dev.hades --serve 8080  # HTTP server
nix develop --command ./build/hades-scope session.log              # replay (key redacted)
```
Targets: `hades_core` (lib), `hades` (app), `hades-{fs-read,shell,write-file,list-dir,http-fetch,save-memory,core-memory,use-skill,save-skill,ask-agent,grep,glob,edit-file,git-read,run-command,schedule-task,list-tasks,cancel-task}` (tools),
`hades-scope` (CLI), `hades_tests`. Stack: libcpr, nlohmann_json, **httplib** (nixpkgs attr `httplib`),
**libedit** (REPL line editing via readline-compat API, BSD-3, via pkg-config — swapped from GPL-3 GNU readline 2026-07-13 for the MIT release), gtest, std::thread. Manifest: `manifests/dev.hades`. Persona: `prompts/soul.md`.

### aarch64 static cross-build (shipped 2026-07-05, `feat/aarch64-cross`) — run on a Raspberry Pi / Debian aarch64
```bash
nix build .#hades-aarch64-static     # -> result/ (fully static musl aarch64; builds the static dep tree)
scp -rL result/ pi:hades/            # deploy: one dir, ZERO deps on the Pi
# on the Pi:  cd hades; export HADES_API_KEY=...; ./bin/hades pi.hades
```
`package.nix` = `stdenv.mkDerivation` (cmake/ninja; `doCheck=false`; **builds only the 17 shipped binaries** via
`ninjaFlags`, skipping `hades_tests`/gtest-static); the flake output
`packages.x86_64-linux.hades-aarch64-static = pkgsCross.aarch64-multiplatform.pkgsStatic.callPackage ./package.nix`
builds it. **pkgsStatic = musl, FULLY STATIC** (`ldd` → "not a dynamic executable") → runs on bare Debian aarch64
with no libc-version-mismatch risk. `$out` IS the deploy dir: `bin/` (17 binaries) + `web/ prompts/ tools/*.{sh,py}`
+ **`pi.hades`** (a clean Pi manifest with **deploy-relative** tool paths `./bin/hades-*`, `webroot=web`,
`prompts/soul.md`; core modules on, serve/telegram/stt/tts/bridge commented — command STT/TTS wrappers need
whisper/piper ON the Pi, http providers need nothing). `libcpr-static` builds fine under musl cross (the feared part).
Smoke here (no Pi): `qemu-aarch64 result/bin/hades` runs (ELF machine `0xb7`). **LIVE-VALIDATED on real Pi0 hardware
2026-07-05** (Vaios: `scp`'d the 60MB deploy, `uname -m`=aarch64, all 17 binaries present, `hades-fs-read describe`
→ valid JSON, and **HTTPS `http_fetch https://example.com` → 200 WITHOUT any `SSL_CERT_FILE`** — the Nix static curl
finds Debian's `/etc/ssl/certs` CA bundle on its own, so the feared CA-cert gotcha is MOOT; TLS works zero-config).
Only the live LLM turn is left (needs `HADES_API_KEY` on the Pi). Non-goals: `.deb`, systemd unit, auto-deploy.
Pieces: `package.nix`, `flake.nix` (packages output), `manifests/pi.hades`. Spec: `docs/superpowers/specs/2026-07-05-aarch64-static-cross-design.md`.

## How it's built (process)
Spec → plan → TDD, on feature branches merged ff to `main`. Specs/plans in `docs/superpowers/`;
SDD ledger + per-task reports in `.superpowers/sdd/` (gitignored). Every change: build + `ctest` green
inside `nix develop` before commit. Reviews via the `cpp-reviewer` agent.

## NEXT possible memory work (v2)
**Archival:** embeddings/vector retrieval (drop in behind `rank_memories` — the seam is built) ·
auto-extract per turn (LLM-summarized, vs explicit `save_memory`) · dedup/decay/importance · sqlite.
**Core:** `core_memory_replace`/edit/forget tools (only append today) · size cap / eviction · provenance/audit.

## NEXT possible web work
**SSE/WebSocket streaming** (token-by-token in the web UI — needs provider streaming + Arbiter partial
emits + an SSE endpoint) · **settings UI** (`web/settings.html` + `GET/POST /manifest` to view/edit the
manifest — the static-dir + JSON-API seam is ready) · **auth** (fill in the `authorize()` seam — token/
password) · agent↔agent **Bridge** (pShare-style, needs design — parked).

## Architecture-honesty pass (after expert critique, 2026-06-29..30) — ALL 4 DONE
**(1) DONE** — manifest `Module=` roster drives modules (pAntler, above).
**(2) DONE** (`874544d`) — manifest parser fails LOUD on a single physical line packing >1 `key = value`
(the silent mis-parse that bit us 3×): a leading-whitespace second-kv scanner records a `kMultiKvWarning`
(parser stays pure), and `enforce_manifest()` at the launch boundary (before key resolution + side effects)
promotes it to a hard `MalConfig` → the binary refuses to start on a corrupt manifest. No false-positive on
URLs/base64/the legit single-kv inline (`Tool = fs { native=./x }`). (Header-form packing without braces +
quoted/free-text values are documented v2 limits.) See `docs/superpowers/*2026-06-29-manifest-parser-fail-loud*`.
**(3) DONE** (`1e5f4b6`) — real tool capability model — see the Tool-capability section below.
**(4) DONE** — worker-offload (single-threaded deterministic bus + LLM call offloaded to an `Executor`;
thread-safe `post()` + `run_until()`; opt-in; TSan-clean) + its **run_until follow-up** (turn-epoch,
race-free budget, idle timeout) + **turn-abandonment hardening** (`TURN_ABANDONED` closes the epoch
dispatch-ordering hole independent of timing — see the Worker-offload section). **NEXT options:**
SSE/tool-offload (when tool-offload lands, extend the epoch+abandonment pattern to `TOOL_RESULT`) ·
capability-model v2 (positive net allowlist, realpath/symlink path resolution, DNS-rebind/connect-time
enforcement) · settings UI · embeddings (over the session-files corpus too) · Bridge (parked).

## Tool-capability model (shipped 2026-06-30, `main` @ `1e5f4b6`) — `CapabilityPolicy` objective
Replaces "blocklist-only" tool safety. A built-in **`capability_of(tool)` table** (the AUTHORITY — a tool
cannot grant itself permission; NOT read from its `describe`) maps the tools to capabilities
(`FsRead/FsWrite/Net/Exec/MemoryAppend/SkillRead/SkillWrite/PeerAsk/GitRead/ExecScoped/Unknown`).
`CapabilityPolicy : Objective` reads **scopes from the
manifest** (`Objective = capability_policy { fs_read_allow / fs_deny / fs_write_allow / exec_allow / block_private_net / confirm_unscoped }`,
MULTI-LINE per the (2) parser footgun) and gates at the Arbiter veto seam: **hard-veto** fs_read of a denied
path + http_fetch to a private/loopback host; **confirm** out-of-scope read / write_file / shell / unknown
tool; **allow** in-scope read / public fetch / memory_append. `avoid_destructive` kept as a backstop
(registered AFTER capability_policy; first hard-veto wins). **Inert unless the manifest lists it** → the test
`build_agent` overload is unaffected. **SSRF/secret hardening (all closed):** redirects disabled in
`http_fetch` (no redirect-SSRF); IPv6 link-local/ULA/`::`/IPv4-mapped (dotted+hex `::ffff:`) denied;
empty/unparseable host fails CLOSED (deny); lexical path-normalize (`./.env`→deny, `..`→confirm);
numeric-obfuscated IP (`2130706433`/`0x7f..`/octal) denied; **type-safe veto** (non-string LLM `path`/`url`
args can't crash the bus — `str_arg` is_string guard + a fail-closed `try/catch` around every objective
`veto()` in `dispatch_or_gate`); boundary-aware allow-match (`./workspace` ≠ `./workspace-backup`);
trailing-dot host stripped. **Documented v1 gaps (v2):** DNS-rebinding/TOCTOU (host string checked, cpr
resolves+connects later — needs connect-time enforcement), symlink path-deny bypass (lexical ≠ realpath),
no positive `net_allow` egress allowlist (default-allow-public still permits exfil to arbitrary public
hosts). Pieces: `src/objective/capability_policy.cpp`, `include/hades/objective/capability_policy.h`,
`app/agent_wiring.cpp` (`make_objective` case), `tools/http_fetch_main.cpp` (redirects off),
`tests/test_capability_{policy,wiring}.cpp`.

### Dev-tools capability extension (shipped 2026-07-05, `feat/dev-tools`) — 5 coding tools + 2 new scopes
Adds 5 native coding tools (`grep glob edit_file git_read run_command`) and extends the capability table with
3 caps + 2 manifest scopes: **`fs_write_allow`** (whitespace-list) — `write_file`/`edit_file` write WITHOUT
confirm under an allowed prefix (`fs_deny` still hard-vetoes, `..`→confirm, else confirm; empty scope = every
write confirms, the old behavior); **`exec_allow`** (**COMMA-separated** — the one non-whitespace list, since
command prefixes contain spaces) — `run_command`→**ExecScoped**: shell-metachar/empty→confirm, token-boundary
prefix match→allow, else confirm (run_command never uses a shell — whitespace-split argv + execvp); **GitRead**
(`git_read`) → **always allow**, read-only by construction (fixed argv per op, no shell, leading-dash paths
rejected, `--` before pathspecs). `grep`/`glob`→FsRead (same scoping as fs_read). dev.hades ships
`fs_write_allow = ./workspace` + `exec_allow = cmake --build build, ctest --test-dir build`. Operator caveat:
allowlist SPECIFIC invocations — a prefix whose binary has a run-a-script flag (`ctest -S`, `cmake -P`, `make`
with attacker targets) grants more than its name (trailing args are inside the trust). git_read v1 gap: a
git-tracked+modified file in `fs_deny` still leaks via `diff` (fs_deny gates fs_read paths, not git-surfaced
content — keep real secrets gitignored). Pieces: `src/behaviors/capability_policy.cpp` (caps+ExecScoped/GitRead),
`app/agent_wiring.cpp` (`split_comma_list` for exec_allow), `tools/{grep,glob,edit_file,git_read,run_command}_main.cpp`,
`docs/manifest-reference.md` (§4–5). Spec/plan: `docs/superpowers/{specs,plans}/*dev-tools*`.

## MULTI-AGENT OPERATION — Bridge v1 SHIPPED (2026-07-03, `feat/bridge`)
Un-parked and shipped. 1 agent = 1 community (Blackboard+Arbiter+modules); peers bridge pShare-style. The v1
answer to the old open questions: **processes-with-Bridge** (each agent a normal hades process, `Module = bridge`
adds an HTTP surface); the bridge carries **two kinds of traffic** — a peer **`/ask`** (drives a real
USER_MESSAGE turn on the receiver, gated by ITS objectives — peers are NOT privileged, confirm-band auto-denied)
and a peer **`/share`** (a variable pushed in as `PEER.<from>.<key>`, pShare-style); an agent appears to its peer
as a **TOOL** (`ask_agent`, whose description names the known peers). Loop protection = **`PeerLoopGuard`** +
the **`TURN_ORIGIN`** convention (`peer:<name>` turns can't call `ask_agent` → v1 `max_hops = 1`). Transport reuses
the TurnGate serialization model (peer turns lock the same gate as chat/serve/telegram). Full details: the
**Bridge / multi-agent** subsection under Current state. **v2 seams (recorded, not built):** per-peer secrets ·
per-peer share lists · per-peer confirm policy (propagate to the asker's human) · per-key rename (full pShare) ·
`max_hops > 1` (wire field present) · transport behind a small interface (queue/webhook) · discovery (static
roster now, registry/mDNS later) · inbound share whitelist · peer presence via `/health` polling · ask offload
(with tool-offload, un-freezing the asker during a peer turn). Still open beyond the bridge: the in-process
`Community` ×N + router variant (level 3) and a `/persona` switch (level 2) remain sketches, not built.

## DONE (the 2026-07-01 list — all shipped)
**1. Skills — DONE (shipped 2026-07-02, `feat/skills`).** A skills system for the hades agent: loadable
instruction packs (`<skills_dir>/<name>/SKILL.md`) the agent discovers via the leading-system-message "Available
skills" roster (SkillsModule `SKILLS_ANNOUNCE` fold) and invokes with `use_skill`, authoring new ones with
`save_skill`. `Skills { dir = skills }` block; `Module = skills` (opt-in). See the **Skills system** subsection under
Current state. (Relates to the parked "persona switch" idea — a persona could ship as a skill.)
**2. Chat-app communication — DONE for Telegram (shipped 2026-07-03, `feat/telegram`).** `TelegramModule` is the
first per-app front-end: long-poll `getUpdates`, allowlisted, shared TurnGate, inline-keyboard confirms, token
via env + redacted. See the **Telegram front-end** subsection under Current state. Pattern proven for the next
apps (Signal/WhatsApp/Discord/Matrix): a new front-end Module posting USER_MESSAGE → run_until → reply, per-app
token via env var, confirm-gating over the app. **Still open (v2):** persistent `offset` (in-memory today →
backlog re-drained each launch is fine, but a crash mid-turn loses the update id), one generic "bridge" module
vs per-app modules, message threading vs the single-session model, webhook (vs long-poll) transport.
**3. Memory system v2 (revisit SOON)** — embeddings (P1+P2) + injection framing shipped + live. Vaios:
"we'll have to revisit this memory system pretty soon." **Brainstorm-first — a rethink, not a bolt-on.** Work-list:
1. **Storage:** switch the flat `.hades/embeddings/*.vec.jsonl` → **sqlite + binary vectors** (+ ANN index once the
   corpus grows) — drop-in behind the `VectorCache` seam (module/Arbiter untouched). Today = flat jsonl + brute-force cosine.
2. **Corpus quality (the real weakness — found live):** the agent **rarely saves facts** (core `facts.md` empty, ~3
   archival records), so recall surfaces chit-chat + "I don't remember" turns. **PARTIALLY DONE 2026-07-11:** soul.md
   learn-triggers (`3a8d0e6`) + bounded editable core memory (`core_memory`, see its section) shipped. REMAINING =
   **auto-extract**: a post-turn background review (cheap/aux model, digest not full transcript) that proactively saves
   preference signals / env facts / corrections without an explicit tool call — **design validated by Hermes-agent**
   (`auxiliary.background_review`, 2026-07-11 research). The "learn by itself" leg; brainstorm-first.
2b. **`session_search` tool (Hermes borrow, cheap):** agent-callable full-text search over `.hades/sessions/*.jsonl`
   returning RAW past-session excerpts (no LLM summarization) — complements the auto-injected embeddings recall
   ("did we discuss X last week?"). Grep-level at our scale; sqlite FTS5 only if/when item 1 lands.
3. **Session-unit granularity:** each session unit embeds the **FULL assistant answer** (`"U:…\nA:<whole answer>"`) →
   bloated/noisy injection. Truncate or **summarize** long turns before embedding.
4. **Retrieval tuning:** `min_similarity=0.45` may be high for `text-embedding-3-small` (try 0.35); consider re-ranking.
5. **Cheaper/metered:** `dimensions` request param (smaller vectors); **embed-cost metering** (currently untracked by
   the budget objective — PPQ embeds hit the balance unmetered).
6. **Freshness:** `/new` does NOT re-point `live_session_path_` (documented gotcha) — a proper session-lifecycle rethink.
(GET /history — DONE `e916084`. Memory embeddings — DONE `20ba94c`. Memory-injection framing — DONE `678a248`.)

## Voice — STT + TTS (decided 2026-07-05, Vaios) — BOTH SHIPPED 2026-07-05
**WhatsApp DROPPED** (2026-07-05): Cloud API is webhook-push → mandatory inbound public HTTPS endpoint (TLS
certs / tunnel) no matter what; unofficial whatsapp-web.js is Node + QR + ToS-risky. Vaios is P2P/self-host
(`hyperdht-cpp`) — TLS/tunnel setup is a non-starter. (Future self-host-native multi-agent path = a
**hyperdht-based Bridge transport** behind the bridge v2 "transport seam" — agent↔agent over DHT, no public IP,
no certs. Parked.)
**Voice = two independent parts. BOTH SHIPPED** (STT first, then TTS).
- **STT — SHIPPED 2026-07-05, `feat/voice-stt`** (405/405, TSan clean; spec
  `docs/superpowers/specs/2026-07-05-stt-voice-input-design.md`, off `main` @ `5fe5f3c`; see the **Voice STT**
  subsection under Current state for the shipped detail). **Source-agnostic provider seam** (Vaios's requirement — a
  clip transcribes the same from Telegram or a future local mic), EXACTLY the embedding-provider precedent: one
  `SttProvider` interface, two transports — `provider = http` (OpenAI-compat `POST <base>/audio/transcriptions`,
  PPQ whisper, DEFAULT — same base-url gotcha as embedding's `/embeddings`) + `provider = command` (local
  **whisper**/whisper.cpp via a reference wrapper `tools/whisper_reference.sh`, one-shot, no warm child).
  **`qwen3_asr_rs` DROPPED** — forked experiment, NOT Vaios's. **English-only v1** (`language = en`; auto-detect
  deferred). `Stt { provider endpoint model api_key_env language timeout_s command }` block, opt-in (no block →
  `Agent.stt==nullptr`, Telegram stays text-only). Seam
  injected into USER-FACING front-ends only — **Bridge excluded** (agent↔agent is text). TelegramModule (text-only
  today, skips `voice`) gains `SttProvider* stt_` + `handle_voice_`: `getFile`+download `.oga`→temp→transcribe→
  `USER_MESSAGE`, on the poll thread (off-bus, no Executor), fail-soft (bad transcribe → text reply, no turn).
  `stt` declared before `telegram` in Agent (teardown: poll thread may be mid-transcribe). dev.hades ships the
  `Stt` block COMMENTED (text-only default); reference wrapper `tools/whisper_reference.sh`; docs `manifest-reference.md` §11.
- **TTS — SHIPPED 2026-07-05, `feat/voice-tts`** (426/426, TSan clean; see the **Voice TTS** subsection under
  Current state for the shipped detail) — a voice-origin reply is spoken back as a Telegram `sendVoice` voice note
  (mirror modality; typed turns stay text). **Source-agnostic provider seam** like STT: one `TtsProvider`, two
  transports — `provider = http` (OpenAI-compat `POST <base>/audio/speech`, `response_format=opus`, DEFAULT — same
  base-url gotcha) + `provider = command` (local **piper** via `tools/piper_reference.sh` → `ffmpeg` ogg-opus).
  Text-anchored + best-effort + fail-soft; `max_chars` (default 4000) caps spoken length. `Tts { provider endpoint
  model voice api_key_env max_chars timeout_s command }` block, opt-in (no block → `Agent.tts==nullptr`, never
  speaks). **Bridge excluded** (a peer never gets audio). `tts` declared before `telegram` (teardown). dev.hades
  ships the `Tts` block COMMENTED; docs `manifest-reference.md` §12.
Future self-host-native multi-agent: a **hyperdht-based Bridge transport** (bridge v2 "transport seam", agent↔agent
over DHT, no public IP/certs) — the P2P path, parked.

## NEXT (decided 2026-07-05 evening, Vaios): bridge-as-protocol + heartbeat/cron — brainstorm-first
Two directions set after the aarch64/Pi + voice batch. BOTH brainstorm-first (not yet designed).
1. **Bridge → a real protocol; standardize the blackboard variables. — SHIPPED 2026-07-05, `feat/bridge-protocol`**
   (card discovery + typed `/share` + trust tiers + Arbiter delegation/reports fold — see the **Bridge protocol**
   subsection under Current state). The original brainstorm follows for the record / v2 seams.
   Today agents share only session TEXT
   (`/ask` = a turn; `/share` = an OPAQUE `PEER.<from>.<key>` value). Vaios wants agents to share MORE — e.g. an
   agent advertises **what skills/tools/capabilities it has** by publishing them on its blackboard (note:
   `SKILLS_ANNOUNCE` is ALREADY a blackboard var; the tool roster + capabilities are known too). **Core idea:
   STANDARDIZE the well-known blackboard variables + value schemas** (a registry of canonical keys — identity,
   skills, tools, capabilities, …) so the bridge can share/mirror STRUCTURED state across peers, not just text.
   This makes the bridge protocol-like (typed, discoverable). **Directly fed by the agent-comms research**
   (`docs/research/2026-07-05-agent-comms-landscape.md`): borrow-ables = **A2A agent-card** (capability discovery
   at `/.well-known/agent.json`), **typed `/share` payloads** (a `type` field, FIPA-ontology idea), **`PEER.*`
   prefix bus subscriptions** (reactive consume of peer-shared vars). So this un-parks 3 of the 5 recorded
   bridge-v2 borrow ideas. Design Qs for brainstorm: which blackboard keys are "standard" (schema), how the bridge
   selects/mirrors them (share-list → typed keys), discovery (agent-card vs a WELL_KNOWN vars query), backward-compat
   with `/ask`+`/share`.
2. **Heartbeat / cron — the agent acts ON ITS OWN. — SHIPPED 2026-07-07, `feat/heartbeat`** (timer → cron →
   gated self-turn; notify/drop with a SILENT sentinel; Telegram `NOTIFY_USER` sink — see the **Heartbeat / cron**
   subsection under Current state). The original brainstorm follows for the record / v2 seams. Today hades is purely
   EVENT-DRIVEN (a turn fires only on
   `USER_MESSAGE` / peer `/ask`). Vaios wants a **self-trigger**: periodic or scheduled internal turns so the agent
   does background/proactive work (monitoring, scheduled tasks, "wake up and check X"). MOOS analog = a timer-driven
   app / `Iterate()` at a rate. Design Qs for brainstorm: tick source (a timer thread posting a `HEARTBEAT`/`TICK`
   bus event → Arbiter runs a self-turn, serialized through the **TurnGate** like every front-end); scheduling model
   (fixed interval vs cron-expressions vs one-shot delays; a `Cron`/`Heartbeat` manifest block); WHAT the agent does
   on a tick (a configured prompt/task? run a skill? check a condition?); guardrails (budget objective + capability
   gates still apply to self-turns; a runaway-loop cap; `TURN_ORIGIN = heartbeat`); interaction with idle-timeout +
   the offload model. This is the "autonomy" leg — turns hades from reactive assistant into a standing agent.

### Email skill (shipped 2026-07-10, `feat/email-tools`) — mailbox access via himalaya, NO code
Read/send the user's mail. Decided as a **skill, not a module/tools** (Vaios ladder: front-end→tools→skill;
himalaya is known/tested/Rust/lightweight/OAuth2-capable, and the skills system exists for exactly this):
`skills/email/SKILL.md` teaches the agent to drive the **himalaya** CLI through the EXISTING tools —
`run_command himalaya envelope list -o json`/`message read <id>` to read, and a `write_file` draft +
`shell himalaya message send < workspace/email_draft.txt` to send. **Zero C++, zero credentials in hades**
(accounts/tokens/keyring live in himalaya's config). Gating falls out of the shipped capability model:
reads are unattended IFF the operator adds `himalaya envelope list, himalaya message read` to
`exec_allow` (else confirm); sends go via `shell` = always confirm-band → auto-denied on heartbeat/peer
turns (no unattended mail, exfil surface shut). Skill is OS-generic (nix/brew/cargo/scoop install ladder),
written for weaker LLMs (file-based send avoids shell-quoting), short (token-frugal). himalaya added to the
flake devShell (runtime dep, not linked); dev.hades ships a commented email-read `exec_allow` example.
**Accepted trade-off (documented):** no MailReadGuard — `exec_allow` reads are allow-band for **peer**-driven
turns too, so a peer could read the user's mail; keep himalaya out of `exec_allow` on bridged workers, OR
the capability-v2 **per-origin exec scopes** fix it properly (added to that backlog). The native-tools design
(3 curl/himalaya tool binaries + MailReadGuard + MailSend=confirm) is the SUPERSEDED alternative in
`docs/superpowers/specs/2026-07-10-email-tools-design.md`; the email **front-end** (agent commanded by /
replying over email, Telegram-equivalent — Auth-Results gate, backlog drain-and-discard, In-Reply-To
threading, NOTIFY_USER email sink) stays a future item with its seeds in that spec.

### Status line (shipped 2026-07-13, `feat/status-module`) — `Module = status`, AGENT_STATUS + dim REPL line
Terminal-cosmetics side quest, design A of the brainstorm (data producer ≠ surface — the terminal has ONE
writer, ChatModule; a second module printing mid-edit would corrupt the libedit line). **`StatusModule`**
(`src/apps/status/status.cpp`, `type()=="status"`, zero-config, no thread) subscribes the traffic every turn
already produces — `USER_MESSAGE` (turn count), `LLM_REQUEST` (model), `LLM_RESPONSE` (**ctx_tokens =
prompt+completion of the LAST call** — the real context measure), `BUDGET_SPENT_USD` (re-posts so sibling
subscriber order can't lag the spend), `NEW_SESSION` (resets ctx/turn; spend stays process-cumulative) — and
posts **`AGENT_STATUS`** `{ctx_tokens, spent_usd, turn, model, line}` (latest-value). ChatModule renders
`line` **dim** under each reply via `print_status_()` (`bb_->get`, the SKILLS_ANNOUNCE pattern): no status
module → key absent → output byte-identical (locked by test). Format:
`[ctx 12.4k tok · $0.0372 · turn 9 · gpt-5.5]` (`format_status`, pure, tested). Raw fields are the seam for
a web/telegram consumer later. Docs: manifest-reference §2 row. dev.hades NOT touched (user adds
`Module = status` himself). **libedit prompt gotcha (found same day, `b2af9eb`):** libedit DROPS an
invisible `\001..\002` block at the very END of the prompt — the kReset never printed and typed input
rendered prompt-colored; fix = reset block BEFORE the prompt's trailing visible space. Verified headless by
driving the real binary through a pty (`script -qec` + raw `\033[A`/`\033[D` bytes) — the arrows/history/
width test method for any future REPL change.

## Other open work
Memory system v2 (work-list above — Vaios: revisit soon) · persona switch · prompt caching · SSE streaming · settings UI · capability-v2
(positive net allowlist / realpath / DNS-rebind / **per-origin exec scopes** — so a `peer:` turn gets a
narrower exec_allow than a human, the proper fix for the email-skill peer-read caveat) · telegram v2 (UTF-8-aware 4096 split · group chats ·
persistent offset · webhook · more apps: Signal/Matrix/Discord on the TurnGate + api-seam pattern) · bridge v2
(per-peer secrets/share-lists/confirm-policy · per-key rename · max_hops>1 · transport seam · discovery ·
inbound-share whitelist · /health presence · ask-offload · **per-peer answer/memory-scope** so a peer turn
can't read out the receiver's full memory — see the Bridge SECURITY note) · **full TUI front-end** (noted
2026-07-13, design C of the status-line brainstorm: ncurses alternate-screen front-end module — persistent
status bar + scrolling chat region; a NEW front-end, fights libedit, weeks not hours; AGENT_STATUS is
already the data feed) · **manifest-parse hardening trio** (from the 2026-07-13 newcomer audit — doc'd
as gotchas, code fix pending: `stay_on_budget` cap `0`/absent bricks the agent → MalConfig or
treat-as-disabled; unknown `Embedding.provider` silently falls back to subprocess → MalConfig like
Stt/Tts; `Tts.max_chars` bare `stoul` accepts `10x`→10 and wraps negatives → strict parse).

### Noted 2026-07-10 (Vaios) — three new future items
1. **Context-full behavior — DECIDE.** Today when a session grows past `history_budget_chars` (default
   120000 chars ≈ ~30k tokens) the Arbiter silently sends only the most-recent tool-pairing-safe suffix
   per request; full history stays on disk. Nothing is summarized, the user is never told, and the dropped
   prefix is only recoverable via embeddings recall (if rostered). Decide the deliberate behavior:
   compact-and-continue (LLM-summarize the dropped prefix into a standing context block, CC `/compact`
   analogue — natural memory-v2 tie-in: auto-extract facts BEFORE they fall off), auto-rotate to a fresh
   session (`/new` + carry-summary), a warning to the user, or a manifest-tunable mix. Brainstorm-first;
   overlaps memory v2's auto-extract work-list item.
2. **Mongoose for HTTP — RESEARCHED 2026-07-13, verdict REJECT** (opus web-verified;
   `docs/research/2026-07-13-mongoose-http-research.md`, commit `434be9e`). Three independent kills:
   (1) **license** — GPLv2-only/commercial dual; linking makes every distributed BINARY GPLv2 regardless
   of MIT source headers → trap for downstream given the MIT publishing goal (civetweb is the MIT fork
   that exists because mongoose left MIT); commercial = bespoke enterprise pricing; not in nixpkgs
   (vendored amalgamation would put GPLv2 files in-tree). (2) **no "one dependency" win** — mongoose's
   HTTP client can't replace cpr/curl (600s LLM POSTs, multipart, proxies, system-CA TLS) → curl stays,
   only httplib would be replaced. (3) **architecture mismatch** — single-threaded event loop where a
   blocking handler stalls all connections vs our minutes-blocking `run_until` handlers → `mg_wakeup()`
   rewrite of both listeners; plus built-in TLS took 3 preauth-RCE CVEs (CVE-2026-5244/45/46, Apr 2026).
   **Decision: keep httplib+cpr (both MIT). SSE streaming when picked up = httplib
   `set_chunked_content_provider("text/event-stream", …)` — the real work is Arbiter partial-token
   emits, HTTP-lib-independent. Server WS if ever needed: extend in-house RFC6455 codec or civetweb (MIT).**
3. **Multiple concurrent sessions per agent — IF/HOW.** Today ONE agent = ONE live conversation: a single
   Arbiter `history_`, one session jsonl, and the TurnGate serializes chat/serve/telegram/bridge into that
   one thread of context (a Telegram turn and a web turn interleave into the SAME conversation). Question:
   can/should one agent hold N independent sessions (per front-end? per Telegram chat-id? per web tab?)
   — needs per-session history/epoch/pending state (a `Session` object the Arbiter switches on a session
   key), session-scoped memory injection, and a routing convention on USER_MESSAGE. Relates to the deferred
   `/sessions` list+switch, telegram-v2 group chats, and the level-2 `/persona` switch. Big architectural
   question — brainstorm-first, likely after memory v2.
4. **User + multiple agents GROUP CHAT (Vaios 2026-07-10).** One conversation with the human AND several
   agents (e.g. Vaios + hades1 + pi0) — today's shapes are strictly pairwise: human↔one agent (front-ends)
   and agent↔agent (bridge `/ask`, one serialized request-reply, max_hops=1, peers can't even be prompted
   mid-turn). A group chat needs: a shared conversation surface (a "room" — who hosts it? one agent as
   moderator/router, or a room object above the agents bridging N Blackboards?), turn-taking (who speaks
   when — free-for-all is chaos + budget burn; address-by-name `@pi0`? moderator picks? round-robin?),
   how a peer's contribution enters each agent's history (today a peer reply is a TOOL_RESULT, not a
   conversation participant), loop/echo containment (PeerLoopGuard generalizes to rooms?), per-agent
   budget/confirm in a shared room, and transport (bridge v2 seam — a `/room` surface or a shared-bus
   relay; telegram group-chat v2 could be the human-facing front half). Builds ON items 3 (multi-session)
   + telegram-v2 groups + bridge v2; the moderator-router variant is close to the parked level-3
   `Community` ×N + router sketch. Far-future, brainstorm-first.

### Pre-publishing doc audit (Vaios 2026-07-08 — do BEFORE publishing the repo)
hades is **moving towards publishing**; docs must be publication-grade first. A dedicated session:
1. **Full-doc review pass, newcomer's eyes:** read `docs/manifest-reference.md` + `docs/architecture.md`
   end-to-end as a first-time operator would; check EVERY claim against the code (the post-bridge audit
   method — grep the key, read the default, confirm the behavior). Fix drift inline.
2. **Per-feature ship-audits keep happening** (the 2026-07-08 mini-audit pattern, commit `b85d885`): when a
   feature ships, verify its reference-doc section AND the cross-cutting spots — the §2 roster table, the §4
   argv-append table, worked examples, defaults/bounds in tables vs code. Gaps found that day: new tools
   missing from the argv table, roster wording stale, bounds not reflected after review hardening.
3. **Also before publishing:** README (none exists), `docs/architecture.md` freshness vs the 2026-07 feature
   wave (bridge protocol/heartbeat/self-scheduling/staleness/when-trigger), dev.hades comments read as the
   first manifest a stranger sees, soul.md tone, LICENSE decision, `.env`/secret conventions documented
   (Appendix B exists — verify), and a pass over `hades --help`/CLI flags vs Appendix A.

### Staleness guard (shipped 2026-07-08, `feat/staleness-guard`) — lost-update protection for edit/write
The CC-gap-analysis backlog item, built as a hybrid of its two options (Arbiter-threaded version token): a file
changed on disk since the LLM last observed it is **refused, untouched**, with a self-healing error. The
`old_string`-must-match-once contract was already enforced; this closes the OTHER gap — staleness.
- **Mechanism:** `fs_read`/`edit_file`/`write_file` results carry **`version`** (FNV-1a 64 → 16-hex of the
  content read/written; `include/hades/tool/file_version.h`, header-only, no core link). The **Arbiter** keeps
  `file_versions_` (lexically-normalized path → version, harvested from successful tracked TOOL_RESULTs via
  `pending_file_ops_` id→path) and at dispatch **strips any LLM-supplied `expect_version`** (hallucination-proof)
  then **injects** the recorded one into `edit_file`/`write_file` args — before the veto loop, so the confirm
  path's `pending_` snapshot carries it too. The **tool** verifies inside its own subprocess right before the
  atomic rename: mismatch → `ok:false` `"file changed on disk since you last read it — fs_read it again and
  retry"` → the LLM re-reads (map updates) and retries. **`expect_version` is NOT in any describe schema**
  (Arbiter plumbing, invisible to the LLM; visible in the Eventlog's TOOL_REQUEST → `hades-scope` observability).
- **Semantics:** no record → no injection → old behavior (**staleness only**, no read-before-edit rule). Each
  successful write updates the map → edit→edit chains work without re-reads. Deleted-since-read → refuse.
  Correctness gate, NOT a confirm — no human wakeup, works identically on heartbeat/peer turns. Map is in-memory,
  survives `/new` (describes disk state, not conversation), cleared by restart (degrade to unguarded).
- **`write_file` is now atomic** (tmp+rename, mode-preserved — edit_file pattern; no more torn files on crash).
- **Not tracked (v1):** `grep`/`glob`/`git_read`/`shell` (a shell write is invisible until the next `fs_read`
  re-syncs); lexical path keys (symlink aliasing — capability-model parity); no config switch (always-on).
- Pieces: `include/hades/tool/file_version.h`, `tools/{fs_read,edit_file,write_file}_main.cpp`,
  `src/apps/arbiter/arbiter.cpp` (strip+inject+harvest, `track_file_op_`), `tests/test_{file_version,staleness_e2e}.cpp`.
  Spec/plan: `docs/superpowers/{specs/2026-07-08-staleness-guard-design.md,plans/2026-07-08-staleness-guard.md}`.
**Gap-analysis convergence (kept for the record):** the leaked CC daemon **"KAIROS"** (periodic `<tick>` →
decide-whether-to-act) was our heartbeat — independent design confirmation. CC's `Monitor` (stream a command /
WebSocket mid-conversation) = the reactive "act when peer state changes" consumer → now the `when=` trigger
brainstorm (next).

### Codebase-organization + docs backlog (Vaios 2026-07-04 — revisit, not yet scheduled)
Three intents about making the **MOOS-IvP mapping legible in the source layout itself** (today the mapping lives
in this doc, not the tree):
1. **Fewer, app-shaped src files.** Consolidate the many small `src/**/*.cpp` into fewer units, each as close as
   possible to ONE MOOS-IvP **app** (module). Tension to resolve deliberately: the house "many small files" rule
   vs "one file ≈ one app" — pick the app-granularity where it clarifies (e.g. a module + its small helpers in
   one TU) without making an 800-line monster. Candidates: the `src/embedding/*` fleet (7 files → 1–2 behind the
   `VectorCache`/provider seams), `src/bridge/*`, `src/telegram/*`. Keep the pure/testable seams intact.
2. **Mark behaviors vs apps in the layout.** Make the tree state which code is a **behavior** (`Objective` —
   `stay_on_budget`, `avoid_destructive`, `capability_policy`, `peer_loop_guard`: competing goals of ONE agent's
   helm) vs an **app** (`Module` — LLM, ToolRunner, Arbiter, Memory, EmbeddingMemory, Skills, Chat, HttpServer,
   Telegram, Bridge). Idea: `src/behaviors/` (objectives) + `src/apps/` (modules) — or at least a header/doc index
   — so the MOOS-IvP behavior↔app distinction is visible without reading CLAUDE.md. (Note: tools are NOT apps —
   they're transient sandboxed subprocess *actions/actuators*; ToolRunner is the app that runs them.)
3. **Manifest reference doc.** One file (`docs/manifest-reference.md`?) listing EVERY exact manifest key, grouped
   by block, with what it does + default + gotchas. Blocks to cover in full: `Session` (provider/endpoint/model/
   api_key_env/price_per_mtok/system_prompt_file/user_file/memory_file/llm_timeout_s/turn_idle_timeout_s/
   sessions_dir/history_budget_chars) · `Module =` roster · `Tool = <name> { native|mcp, timeout_s }` · `Objective
   = {stay_on_budget|avoid_destructive|capability_policy}` (+ each one's keys) · `Memory` · `Embedding` · `Skills`
   · `Serve` · `Telegram` · `Bridge` + `Peer` · `Arbiter`. Single source of truth for operators (the info is today
   scattered across CLAUDE.md subsections + code defaults).

## Gotchas
- **src/ reorganized 2026-07-04** (`refactor/src-apps`): `src/apps/<name>/` (one dir per Module —
  MOOS one-dir-per-app), `src/behaviors/` (Objectives), `src/core/` (shared infra incl. config/
  session/subprocess). 45→22 files; headers/tests/app/tools untouched; `Pieces:` paths in
  OLDER sections above are pre-reorg (find code via `src/apps/<module-name>/`).
- nixpkgs renamed `cpr`→`libcpr` and cpp-httplib's attr is **`httplib`**.
- The manifest `Module =` lines **drive the module set** (pAntler): `build_agent(Manifest)` →
  `Launcher.instantiate` (MalConfig on unknown type) → `take_as` into the Agent → `wire_agent` (null-guarded,
  dependency order). Omit a module → it's absent (`agent.X==nullptr`); binary errors if `llm`/`arbiter`/the
  requested front-end is missing. Cross-wiring (Arbiter←tools/objectives/model/prompt) stays explicit in
  `wire_agent`. dev.hades roster = llm/tool_runner/memory/chat/arbiter/serve/skills/embedding_memory.
- API key: env var only, redacted in the Eventlog; never put it in the manifest.
- Single-threaded **dispatch** — subscriber handlers run ONLY on the pump thread (the determinism
  invariant). `post()` is thread-safe (workers call it); the blocking LLM call is offloaded to an
  `Executor` worker when set. HTTP server still serializes whole turns under one mutex. All four
  front-ends (REPL/serve/telegram/simplex) serialize whole turns through one shared **TurnGate** (Agent's
  FIRST member → destroyed LAST). **Teardown order is load-bearing:** the member tail is declared
  `telegram → simplex → heartbeat` (heartbeat LAST → destroyed FIRST, then simplex, then telegram); each
  thread-owning member's dtor stop+joins its own thread so any in-flight turn/tick finishes while the
  Executor + modules are still alive; `executor` sits before that tail (joined before the plain
  modules+Blackboard); `bb` declared before `agent` in `hades_main`. Do NOT reorder.
- **Manifest parser is one-kv-per-line.** A single-line block with two `k = v` pairs mis-parses (first
  `=` wins, rest swallowed). Use **multi-line blocks only** (like `Session`/`Memory`/`Serve`/`capability_policy`).
  As of feature (2) this **now fails LOUD** — a packed line → `kMultiKvWarning` → `enforce_manifest` throws
  `MalConfig` at launch (no more silent mis-parse; the legit single-kv inline `Tool = fs { native=./x }` and
  `Objective = avoid_destructive { veto = true }` are still fine). Lock tests parse the shipped `dev.hades`.
- **Tool calls are capability-gated** (feature (3)): `CapabilityPolicy` objective (built-in `capability_of`
  table + manifest scopes) hard-vetoes secret-path reads + private-host fetches, confirm-gates write/shell/
  unscoped, allows in-scope read/public-fetch/memory-append; `avoid_destructive` is the backstop. **Inert
  unless the manifest lists `Objective = capability_policy`** (multi-line block). v2 gaps documented:
  DNS-rebind/TOCTOU, symlink path-deny, no positive net allowlist. `http_fetch` no longer follows redirects.
  Dev-tools scopes: `fs_write_allow` (write/edit without confirm) is whitespace-separated like the other path
  lists, but **`exec_allow` is COMMA-separated** (the one non-whitespace list — command prefixes contain spaces,
  e.g. `cmake --build build, ctest --test-dir build`). `git_read` is always-allow (leaks a modified fs_deny file
  via `diff` — keep secrets gitignored); `grep`/`glob` scope like `fs_read`.
- `save_memory`/`core_memory` store paths must contain **no whitespace** (tool argv is whitespace-split) —
  wiring throws `MalConfig` if they do.
- `core_memory` tool **requires** `memory_file` in the Session block (wiring throws `MalConfig` otherwise) —
  else edits would write a file the Arbiter never reads (silent drift; caught by the final review).
- Web UI: `webroot` is **cwd-relative** (default `web/`) → run `--serve` from the repo root (warns if the
  dir is missing). The page sends an `X-Hades` header; the `authorize()` CSRF seam requires it on
  `POST /chat`+`/confirm` (don't strip it client-side). `/.hades/` and runtime stores are gitignored.
- **Telegram** (`Module = telegram`): `allow_users` (numeric ids) is **REQUIRED** — `on_start` throws
  `MalConfig` without it (an open bot lets anyone drive the agent); non-allowed senders are silently
  dropped. **Private-chat-only (v1):** `chat_id == from_id` enforced; group messages are dropped (replies
  would be group-readable). Bot token via `token_env` (default `TELEGRAM_BOT_TOKEN`) env var **only** — never in the
  manifest; redacted in `session.log`; keep it in a **gitignored `.env`** you `source`. The poll thread is
  started by `hades_main` AFTER wiring (`start_polling()`), NOT in `on_attach` (tests never spawn a thread);
  the dtor's join can wait up to one `poll_timeout_s` cycle for `getUpdates` to return (default 50s → shorten
  it if `/quit` feels slow). First `poll_once` drains-and-DISCARDS the startup backlog (offset in-memory, v1)
  → a command queued while the agent was down never replays. Telegram-only roster (no chat/serve) → `hades_main`
  blocks on `wait()`.
- **Simplex** (`Module = simplex`): fourth front-end over a **local `simplex-chat` daemon** (in-house WS
  client, `src/apps/simplex/{ws,simplex}.cpp`). **No token** — the daemon's WS API is **unauthenticated by
  design**, so keep it **loopback-only** (`host` default `127.0.0.1`; don't expose the port). `allow_contacts`
  (comma-separated numeric ids and/or exact display names) is **REQUIRED** — `on_start` throws `MalConfig`
  without it; non-allowed senders are silently dropped. **`auto_accept = false` is the default** (accept
  contacts in the CLI); `auto_accept = true` is an opt-in with a **name-spoof caveat** (a stranger can name
  themselves like an allowlisted display name → **prefer numeric ids**). Confirms are plain **`y/N` text**
  (no inline keyboards) — the next message from that contact answers. **Notify:** subscribes `NOTIFY_USER`,
  delivers to `notify_contact` (id direct; name resolves once seen, else skipped) — heartbeat sink, same as
  Telegram; both rostered → delivered on both. Event thread started by `hades_main` (`start()`) AFTER wiring,
  never `on_attach` (tests spawn no thread/socket); reconnects with `connect_timeout_s` backoff. Simplex-only
  roster (no chat/serve) → `hades_main` blocks on `wait()`. `Agent::simplex` sits between `telegram` and
  `heartbeat` (teardown tail telegram → simplex → heartbeat). Docs: manifest-reference §16. Pi runtime dep =
  the official aarch64 `simplex-chat` CLI binary.
- **Bridge** (`Module = bridge`): the `Bridge` block is the agent's **identity** (its `name` is embedded in bus
  keys + the `peer:<name>` TURN_ORIGIN) — it's meaningful even without peers, but `ask_agent` needs both
  `Bridge.name` AND ≥1 `Peer` block to delegate anywhere. The shared **`secret_env`** (default
  `HADES_BRIDGE_SECRET`) is **REQUIRED** when the module is rostered — never in the manifest; redacted in
  `session.log`; keep it in the gitignored `.env` (same secret on every fleet member, v1). Inbound `/ask` and
  `/share` are auth + peer-allowlist gated → **403** on bad secret / unknown `from` (malformed body → 200 + `{ok:false,error}`). **TURN_ORIGIN
  convention:** every front-end MUST post `TURN_ORIGIN` at turn start (`human` for chat/serve/telegram,
  `peer:<name>` for bridge) — `PeerLoopGuard` reads it to hard-veto `ask_agent` on peer-origin turns (no forward =
  no loop; v1 `max_hops = 1`). `port = 0` → ephemeral bind; listener thread started by `hades_main` after wiring
  (like telegram), NOT in `on_attach`.
- **Bridge protocol** (card discovery + typed `/share`): `discover_interval_s` **literal `0` = discovery
  OFF** (boot-pull only, no periodic thread) — any positive value is the re-pull period (default 300s). The
  card's **`caps` is a SUMMARY** (`"scoped"`/`"none"`/`"public"`/`"private-blocked"`) — **never** literal
  `fs_*_allow`/`exec_allow` paths or command strings (a peer can't learn your allowlist). **`GET /card` is
  secret-gated** (same shared secret as `/ask`) — a **public** card + `/.well-known/agent.json` is deferred v2.
  A **`Peer` block with `trust`** (or any 2nd key) MUST be **multi-line** — the one-kv-per-line parser
  **fails loud** (`MalConfig`) on `Peer = watcher { url = …  trust = untrusted }`; write `url`/`trust` on
  separate lines. `type=raw`/absent keeps legacy `/share` (`PEER.<from>.<key>`); rename-on-arrival holds for
  all types (no peer can write a local bus key).
- **Heartbeat** (`Module = heartbeat`): a tick is a normal gated self-turn (`TURN_ORIGIN=heartbeat:<name>`) — all
  objectives apply, confirm-band actions **auto-deny** (no human), a tick clashing with a live human/peer turn is
  **skipped** (`HEARTBEAT_SKIPPED`, try_lock on the TurnGate), never queued. **Inline `prompt` with an `=` (`set x
  = 5`) → one-kv-per-line parser fails loud (`MalConfig`), binary won't boot ⇒ use `prompt_file`** for any prompt
  with an `=` or multiple lines (cron values are `=`-free, safe inline). Cron is **machine-LOCAL TZ** (5-field
  `min hour dom month dow`, AND across fields, minute resolution) — set the box's TZ deliberately. `notify=true`
  delivers via `NOTIFY_USER` → **Telegram only (v1)**, so **needs `Module = telegram` rostered** or nothing shows;
  reply is dropped when empty or exactly `SILENT`. Timer thread started by `hades_main` after wiring (not
  `on_attach` → tests spawn none). A named entry takes **`when` XOR `schedule`** (reactive watch, shipped
  2026-07-08 — 5 keyword forms, edge-triggered, `cooldown_s` absorbs flaps, ≤~30s latency; see the Reactive
  when= trigger subsection). A `peer:`-origin turn can NEVER `schedule_task` (unconditional guard veto).
- Core memory (`memory/facts.md`) is **git-tracked** and the agent mutates it at runtime → expect
  working-tree churn; review/commit the agent's pins as curated standing facts (or gitignore it).
- `skills/` is **git-tracked** the same way — the agent authors skills at runtime via `save_skill`, so
  agent-written `skills/<name>/SKILL.md` show as working-tree churn to review/commit (or gitignore it).
- Interactive REPL uses libedit (readline-compat) only when stdin is a **real TTY**; piped/test input falls back to
  `std::getline` (keeps the injected-stream test seam). Arrow-key editing verified live 2026-06-29.
- **Embedding `endpoint` must be the BASE url, NOT `.../embeddings`** — the HTTP provider appends `/embeddings`.
  PPQ: `endpoint = https://api.ppq.ai/v1` (→ `…/v1/embeddings`). Setting `…/v1/embeddings` → `…/embeddings/embeddings`
  → every embed fails → fail-soft (`EMBED_INDEX_DONE=false`, `RETRIEVED_MEMORY_SEMANTIC=""`, no cache file). Bit Vaios once.
- **Stt `endpoint` is the BASE url too** (http provider) — it appends `/audio/transcriptions` (same footgun as
  embedding's `/embeddings`). PPQ: `endpoint = https://api.ppq.ai/v1`. STT is opt-in (no `Stt` block → text-only,
  `Agent.stt==nullptr`), fail-soft (bad transcribe → "didn't catch that", never a crash), and injected into
  user-facing front-ends ONLY — the **Bridge is never given one** (a peer can't send audio). dev.hades ships the
  `Stt` block COMMENTED.
- **Tts `endpoint` is the BASE url too** (http provider) — it appends `/audio/speech` (same footgun as Stt's
  `/audio/transcriptions` + embedding's `/embeddings`). PPQ: `endpoint = https://api.ppq.ai/v1`; OpenAI:
  `https://api.openai.com/v1`. **PPQ TTS model = `deepgram_aura_2`** (Deepgram Aura) or ElevenLabs
  (`eleven_multilingual_v2`/`eleven_flash_v2_5`), NOT `tts-1`; **voice = `aura-2-arcas-en`** (Aura voices
  `aura-2-{arcas,thalia,andromeda,helena,apollo,aries}-en`) — `resolve_tts` defaults are `deepgram_aura_2`/
  `aura-2-arcas-en`. **PPQ char limits: 2000 (Deepgram) / 5000 (ElevenLabs)** → set `max_chars` ≤ that (over → 422
  → fail-soft skip). **Telegram `sendVoice` requires OGG/Opus, so the provider MUST yield it** — the http provider
  sends `response_format=opus`; **but PPQ's `/audio/speech` docs DON'T list `response_format`** → if PPQ ignores it
  and returns mp3, `sendVoice` rejects → silent skip (text stands); #1 TTS smoke risk, v2 fix = module mp3→opus
  transcode. The `command` wrapper must emit ogg-opus on stdout (`tools/piper_reference.sh` = piper → `ffmpeg -c:a libopus -f ogg`).
  TTS is opt-in (no `Tts` block → `Agent.tts==nullptr`, never speaks), **mirror modality** (only voice-origin turns
  speak; typed stays text), text-anchored + best-effort + fail-soft, `max_chars` (default 4000) caps spoken length,
  and injected into user-facing front-ends ONLY — the **Bridge is never given one** (a peer never gets audio).
  dev.hades ships the `Tts` block COMMENTED.
- **Embedding live-session exclusion is fixed at launch.** `/new` rotates the Arbiter's session but does NOT
  re-point the embedding module's `live_session_path_` (set once, before `on_attach`, to avoid a cross-thread
  write). So a periodic reindex after a `/new` may index the now-live post-`/new` session mid-write — parser-safe
  (tolerant read skips a torn line; completed pairs are append-stable) and self-heals next launch. Accepted v1.
- **Embedding cost is NOT metered** by the budget objective (`price_per_mtok` meters only the LLM). HTTP-provider
  embed calls (e.g. PPQ) hit the key's balance unmetered; indexing is incremental + the query is 1 short embed/turn.
