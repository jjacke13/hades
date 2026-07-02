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
| pShare / pMOOSBridge | (planned) a **Bridge** module between Blackboards for agent↔agent |

**Personas/multi-agent:** 1 agent = 1 community (Blackboard+Arbiter+modules). Objectives are that
agent's goals, NOT other agents. More agents = replicate the community; bridge them with a `pShare`-style
Bridge. Levels: (1) separate manifests [today], (2) `/persona` switch, (3) a `Community` struct ×N +
router + Bridge [real multi-agent].

## Current state (2026-07-02)
`feat/skills` (off `main` @ `678a248`), **284/284 tests** (ASan+UBSan + **TSan** clean; suite ~3.0s), ~9 MB RSS, **live** against PPQ (`claude-haiku-4.5` LLM + `openai/text-embedding-3-small` embeddings).
Built: Blackboard+Eventlog · Arbiter v1 (veto/confirm gate, max-steps guard) · **9 tools**
(`fs_read shell write_file list_dir http_fetch save_memory pin_fact use_skill save_skill`, self-describing) · **tool capability
model** (`CapabilityPolicy` objective — scoped fs_read/http_fetch allow/confirm/deny, see below) + the older
destructive-pattern gate (`avoid_destructive`, kept as backstop) ·
**two memory layers** (core + archival, see below) · a **skills system** (loadable instruction packs, see below) ·
layered **system prompt** (SOUL/USER static +
live core MEMORY) · two front-ends: **stdin REPL** (GNU readline — arrows/history/Ctrl-A/E, colored
labels) and **HTTP `--serve`** (browser web UI + JSON API, see below) · **worker-offload concurrency**
(see below) · **manifest parser fails LOUD** on packed multi-kv lines (see below).

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

### Two memory layers (MemGPT-style, both agent-writable)

### Two memory layers (MemGPT-style, both agent-writable)
- **Archival / searchable** — `save_memory` tool → `.hades/memory.jsonl` (append-only). MemoryModule
  (`type()=="memory"`) keyword-ranks it each turn (`rank_memories`, pure; **v2 seam = embeddings**) and
  posts `RETRIEVED_MEMORY`; Arbiter injects it as an **ephemeral** `{role:system}` labeled memory block
  before the last user msg (see Memory-injection framing below). Config: `Memory { store=… top_n=… }`. **LIVE-VALIDATED** (save→restart→recall).
- **Core / always-on** — `pin_fact` tool → `memory/facts.md` (append-only, newlines stripped, parent dir
  created). The Arbiter **re-reads this file every turn** (`read_memory_layer`) and folds it into the
  **leading** `{role:system}` message (after static SOUL/USER) — live same-session. Config: Session
  `memory_file = memory/facts.md`; wiring **requires memory_file when pin_fact is present** (MalConfig
  fail-fast) and appends the path to the tool argv (single source of truth).
Pieces: `src/memory/{rank,store}.cpp`, `src/module/memory_module.cpp`, `src/config/prompt.cpp`
(`assemble_system_prompt`=SOUL+USER, `read_memory_layer`=live core), `tools/{save_memory,pin_fact}_main.cpp`.

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

## Build / run
```bash
export HADES_API_KEY=<key>                                   # key never in the manifest
nix develop --command cmake -S . -B build -G Ninja           # configure (once)
nix develop --command cmake --build build                    # build
nix develop --command ctest --test-dir build                 # test (284/284, ~3.0s)
nix develop --command ./build/hades manifests/dev.hades --serve      # web UI -> http://localhost:8080/
nix develop --command ./build/hades manifests/dev.hades             # chat REPL
nix develop --command ./build/hades manifests/dev.hades --serve 8080  # HTTP server
nix develop --command ./build/hades-scope session.log              # replay (key redacted)
```
Targets: `hades_core` (lib), `hades` (app), `hades-{fs-read,shell,write-file,list-dir,http-fetch,save-memory,pin-fact,use-skill,save-skill}` (tools),
`hades-scope` (CLI), `hades_tests`. Stack: libcpr, nlohmann_json, **httplib** (nixpkgs attr `httplib`),
**readline** (REPL line editing, GPL-3, via pkg-config), gtest, std::thread. Manifest: `manifests/dev.hades`. Persona: `prompts/soul.md`.

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
cannot grant itself permission; NOT read from its `describe`) maps the 9 tools to capabilities
(`FsRead/FsWrite/Net/Exec/MemoryAppend/Unknown`). `CapabilityPolicy : Objective` reads **scopes from the
manifest** (`Objective = capability_policy { fs_read_allow / fs_deny / block_private_net / confirm_unscoped }`,
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

## NEXT (decided 2026-07-01, Vaios) — in order
**1. Skills — DONE (shipped 2026-07-02, `feat/skills`).** A skills system for the hades agent: loadable
instruction packs (`<skills_dir>/<name>/SKILL.md`) the agent discovers via the leading-system-message "Available
skills" roster (SkillsModule `SKILLS_ANNOUNCE` fold) and invokes with `use_skill`, authoring new ones with
`save_skill`. `Skills { dir = skills }` block; `Module = skills` (opt-in). See the **Skills system** subsection under
Current state. (Relates to the parked "persona switch" idea — a persona could ship as a skill.)
**2. Chat-app communication** — talk to the agent via other apps (Telegram/Signal/WhatsApp/Discord/Matrix…).
Architecturally = **new front-end Module(s)** alongside ChatModule/HttpServerModule (same pattern: post
USER_MESSAGE → run_until → reply), likely long-polling/webhook per app; per-app auth/token via env var
(never in the manifest); confirm-gating must work over the chat app (Approve/Deny). **Brainstorm-first**:
which app first (Telegram bot API is the easiest: pure HTTPS long-poll, no LAN exposure), one generic
"bridge" module vs per-app modules, message threading vs the single-session model.
**3. Memory system v2 (revisit SOON)** — embeddings (P1+P2) + injection framing shipped + live. Vaios:
"we'll have to revisit this memory system pretty soon." **Brainstorm-first — a rethink, not a bolt-on.** Work-list:
1. **Storage:** switch the flat `.hades/embeddings/*.vec.jsonl` → **sqlite + binary vectors** (+ ANN index once the
   corpus grows) — drop-in behind the `VectorCache` seam (module/Arbiter untouched). Today = flat jsonl + brute-force cosine.
2. **Corpus quality (the real weakness — found live):** the agent **rarely saves facts** (core `facts.md` empty, ~3
   archival records), so recall surfaces chit-chat + "I don't remember" turns. Options: a soul.md nudge to `save_memory`/
   `pin_fact` more; OR **auto-extract** salient facts per turn (LLM-summarized) instead of relying on explicit tool calls.
3. **Session-unit granularity:** each session unit embeds the **FULL assistant answer** (`"U:…\nA:<whole answer>"`) →
   bloated/noisy injection. Truncate or **summarize** long turns before embedding.
4. **Retrieval tuning:** `min_similarity=0.45` may be high for `text-embedding-3-small` (try 0.35); consider re-ranking.
5. **Cheaper/metered:** `dimensions` request param (smaller vectors); **embed-cost metering** (currently untracked by
   the budget objective — PPQ embeds hit the balance unmetered).
6. **Freshness:** `/new` does NOT re-point `live_session_path_` (documented gotcha) — a proper session-lifecycle rethink.
(GET /history — DONE `e916084`. Memory embeddings — DONE `20ba94c`. Memory-injection framing — DONE `678a248`.)

## Other open work
MCP tool discovery (MCP servers can be called but aren't announced to the LLM) · persona switch · prompt
caching · SSE streaming · settings UI · capability-v2 (positive net allowlist / realpath / DNS-rebind) ·
agent↔agent Bridge (parked).

## Gotchas
- nixpkgs renamed `cpr`→`libcpr` and cpp-httplib's attr is **`httplib`**.
- The manifest `Module =` lines **drive the module set** (pAntler): `build_agent(Manifest)` →
  `Launcher.instantiate` (MalConfig on unknown type) → `take_as` into the Agent → `wire_agent` (null-guarded,
  dependency order). Omit a module → it's absent (`agent.X==nullptr`); binary errors if `llm`/`arbiter`/the
  requested front-end is missing. Cross-wiring (Arbiter←tools/objectives/model/prompt) stays explicit in
  `wire_agent`. dev.hades roster = llm/tool_runner/memory/chat/arbiter/serve/skills/embedding_memory.
- API key: env var only, redacted in the Eventlog; never put it in the manifest.
- Single-threaded **dispatch** — subscriber handlers run ONLY on the pump thread (the determinism
  invariant). `post()` is thread-safe (workers call it); the blocking LLM call is offloaded to an
  `Executor` worker when set. HTTP server still serializes whole turns under one mutex. **Teardown order
  is load-bearing:** `Executor` joined before modules+Blackboard (Agent's `executor` is the last member;
  `bb` declared before `agent` in `hades_main`).
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
- `save_memory`/`pin_fact` store paths must contain **no whitespace** (tool argv is whitespace-split) —
  wiring throws `MalConfig` if they do.
- `pin_fact` tool **requires** `memory_file` in the Session block (wiring throws `MalConfig` otherwise) —
  else pins would write a file the Arbiter never reads (silent drift; caught by the final review).
- Web UI: `webroot` is **cwd-relative** (default `web/`) → run `--serve` from the repo root (warns if the
  dir is missing). The page sends an `X-Hades` header; the `authorize()` CSRF seam requires it on
  `POST /chat`+`/confirm` (don't strip it client-side). `/.hades/` and runtime stores are gitignored.
- Core memory (`memory/facts.md`) is **git-tracked** and the agent mutates it at runtime → expect
  working-tree churn; review/commit the agent's pins as curated standing facts (or gitignore it).
- `skills/` is **git-tracked** the same way — the agent authors skills at runtime via `save_skill`, so
  agent-written `skills/<name>/SKILL.md` show as working-tree churn to review/commit (or gitignore it).
- Interactive REPL uses readline only when stdin is a **real TTY**; piped/test input falls back to
  `std::getline` (keeps the injected-stream test seam). Arrow-key editing verified live 2026-06-29.
- **Embedding `endpoint` must be the BASE url, NOT `.../embeddings`** — the HTTP provider appends `/embeddings`.
  PPQ: `endpoint = https://api.ppq.ai/v1` (→ `…/v1/embeddings`). Setting `…/v1/embeddings` → `…/embeddings/embeddings`
  → every embed fails → fail-soft (`EMBED_INDEX_DONE=false`, `RETRIEVED_MEMORY_SEMANTIC=""`, no cache file). Bit Vaios once.
- **Embedding live-session exclusion is fixed at launch.** `/new` rotates the Arbiter's session but does NOT
  re-point the embedding module's `live_session_path_` (set once, before `on_attach`, to avoid a cross-thread
  write). So a periodic reindex after a `/new` may index the now-live post-`/new` session mid-write — parser-safe
  (tolerant read skips a torn line; completed pairs are append-stable) and self-heals next launch. Accepted v1.
- **Embedding cost is NOT metered** by the budget objective (`price_per_mtok` meters only the LLM). HTTP-provider
  embed calls (e.g. PPQ) hit the key's balance unmetered; indexing is incremental + the query is 1 short embed/turn.
