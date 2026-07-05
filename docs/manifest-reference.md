# hades manifest reference

The operator reference for the hades manifest (`manifests/*.hades`). Every key below is
extracted from the code that actually reads it — not from prose. Defaults are the value the
code uses when the key is absent; where a default lives in a header constant it is named.

A manifest is a plain-text, MOOS-style file of blocks. It decides which modules exist, how the
LLM is reached, which tools/objectives are wired, and how the front-ends behave. The API key,
bot token and bridge secret are **never** in the manifest — the manifest only names the *env var*
that holds each (see the appendix).

Run: `hades <manifest> [--serve [port]] [--resume [id]]`. See `manifests/dev.hades` for a worked
example.

---

## 1. Manifest syntax

Parser: `src/core/config.cpp`.

**Block forms.** A block is `Section = name` followed by a `{ … }` body:

```
Session                     # header-only, no name; body opens on the next line
{
  model = gpt-5.5           # one key = value per line
}

Serve { host = 127.0.0.1 }  # single-kv inline form (one pair only, on the header line)

Module = llm                # name-only line, no body — the roster form
Tool   = fs { native = ./build/hades-fs-read }   # named block, single-kv inline
```

- **`Section = name`** — the text left of the first `=` is the section, right of it is the name
  (`Tool = fs` → section `Tool`, name `fs`). Header-only lines (`Session`) have an empty name.
- **`{ … }`** — the body. It can open on the same line, on the header line after the name, or as
  a bare `{` on the following line (attaches to the most recent block). `}` on its own line closes it.
- Section names are matched **case-insensitively** (`Session`/`session` both work); keys are
  lower-cased before lookup. Values keep their original case and are trimmed of surrounding whitespace.
- **Comments:** everything from `#` to end of line is stripped (anywhere on the line).
- Blank lines are ignored.

**THE one-key-per-line rule (load-bearing).** Inside a `{ … }` body, put exactly one `key = value`
per physical line. Only the first `=` on a line is the separator; a second `key = value` packed onto
the same line is a fatal error:

```
# WRONG — two pairs on one line:
Session { model = gpt-5.5  price_per_mtok = 5.0 }
```

At launch `enforce_manifest()` (`app/agent_wiring.cpp`) scans for this pattern and throws
`MalConfig` — **the binary refuses to start**. The detector requires whitespace before the second
`key =`, so a `=` inside a single token (a URL query `?a=b`, base64 padding `x==`) does **not**
trip it. The single-kv inline forms above (`Tool = fs { native = ./x }`) are fine because they
carry only one pair. When in doubt, use a multi-line block.

**Value typing.** Helpers in `manifest.cpp`:
- doubles: parsed strict (whole string must be numeric); a garbage value is **ignored → the default
  is kept** (never 0). Most numeric keys additionally require **> 0** (`set_pos_double_on_string`);
  a few keys parse 0 specially (noted where they do).
- bools: `true`/`1` → true, `false`/`0` → false; anything else keeps the default.
- lists: a few keys are whitespace-separated lists (capability scopes, `allow_users`,
  `share_out`) — see each.

---

## 2. Module roster — `Module = <name>`

`app/agent_wiring.cpp` (Launcher factories). Each `Module = X` line adds module `X` to the agent.
Omit a line → that module is absent (`agent.X == nullptr`, zero coupling). An **unknown** name →
`MalConfig` at launch.

| Module name | What it is | Omitting it |
|---|---|---|
| `llm` | LLM bridge (calls the provider). **Required.** | Binary errors: "missing llm/arbiter — cannot take a turn". |
| `arbiter` | The helm: runs the turn loop, gates actions. **Required.** | Same error as above. |
| `tool_runner` | Runs `Tool` blocks as subprocesses. | No tools available; Arbiter gets an empty tool list. |
| `memory` | Keyword-ranked archival recall (reads `Memory` block). | No keyword memory retrieval. |
| `embedding_memory` | Semantic recall (reads `Embedding` block). | No semantic recall (keyword-only). |
| `skills` | Announces the skills library each turn (reads `Skills` block). | No skills roster; `use_skill`/`save_skill` still run if rostered as tools. |
| `chat` | stdin REPL front-end. | No REPL. |
| `serve` | HTTP/web front-end (`--serve`). | `--serve` errors "no serve module". |
| `telegram` | Telegram long-poll bot (reads `Telegram` block). | No bot. |
| `bridge` | Agent↔agent HTTP listener (reads `Bridge` block). | No inbound peer surface. |

**Front-end requirement.** After building, the binary needs *some* front-end or it errors:
`--serve` uses `serve`; otherwise it runs `chat`'s REPL; if there is no `chat`, it blocks on
`telegram` (poll) or `bridge` (listener); with none of those it errors "no chat module".

dev.hades roster: `llm tool_runner memory chat arbiter serve skills embedding_memory bridge`.

---

## 3. `Session` block

The core config: the LLM provider, prompt files, memory file, timeouts, session persistence.
Read across `app/agent_wiring.cpp`, `src/apps/llm/llm.cpp` (`on_start`),
`src/core/config.cpp`, `app/hades_main.cpp`.

| Key | What it does | Default | Notes |
|---|---|---|---|
| `model` | Model id sent in every LLM request. | `""` (empty) | Passed to the provider and each `LLM_REQUEST`. |
| `endpoint` | Base URL of the OpenAI-compatible LLM API. | `""` | e.g. `https://api.ppq.ai/v1`. |
| `api_key_env` | **Name of the env var** holding the API key. | `HADES_API_KEY` | Key resolved from the env; unset → `MalConfig`. Redacted in `session.log`. |
| `price_per_mtok` | USD per million tokens, for the budget objective. | `0` | Only meters the LLM (not embeddings). |
| `llm_timeout_s` | Per-call LLM HTTP timeout (cpr). The real cap on one "think". | `600` (`kDefaultLlmTimeoutS`) | Bad/0 → default. |
| `turn_idle_timeout_s` | Front-end `run_until` **idle** ceiling. Resets on every bus event → bounds a single silent stretch, not total turn time. | `900` (`kDefaultTurnIdleTimeoutS`) | **MUST be > `llm_timeout_s`** or `MalConfig` at launch (see below). |
| `system_prompt_file` | SOUL persona file, prepended to every turn. | none | Unreadable path → `MalConfig`. |
| `user_file` | USER profile file, appended after SOUL. | none | Optional; unreadable path → `MalConfig`. |
| `memory_file` | Core "always-on" memory file; the Arbiter re-reads it each turn; `pin_fact` writes it. | `""` | **Required if the `pin_fact` tool is rostered** (else `MalConfig`). Path must be whitespace-free. |
| `sessions_dir` | Directory of per-session conversation `.jsonl` files. | `.hades/sessions` | `--resume` reads from here; `/new` rotates within it. |
| `history_budget_chars` | Max chars of history sent per LLM request (full history still kept on disk). | `120000` (`kDefaultHistoryBudgetChars`) | ~30k tokens. Very high → effectively "send whole session". |
| `provider` | *(currently unread)* | — | The LLM module always builds an OpenAI-compatible provider; this key is decorative. |

**Gotchas.**
- `turn_idle_timeout_s > llm_timeout_s` is enforced with a hard `MalConfig` before anything heavy
  is built. Rationale: only the LLM call is offloaded to a worker; a slow-but-alive call must post
  back (resetting the idle deadline) before `run_until` abandons the turn. Tools run inline and
  can't trip the idle timer.
- `system_prompt_file`/`user_file`/`memory_file` are **cwd-relative** — run hades from the repo root.
- `provider = openai_compat` in dev.hades does nothing; the transport is fixed.

---

## 4. `Tool` blocks — `Tool = <name> { native = … | mcp = … }`

`src/apps/tool_runner/tool_runner.cpp` (`add_from_block`), plus argv appends in
`app/agent_wiring.cpp` (`wire_agent`).

| Key | What it does | Default | Notes |
|---|---|---|---|
| `native` | Path (+args) to a subprocess tool binary. | — | Split on whitespace into argv; probed once with `{"call":"describe"}`. |
| `mcp` | MCP server command (alternative to `native`). | — | MCP tools are **not** announced to the LLM yet (discovery deferred). |
| `timeout_s` | Per-tool subprocess cap. | `0` → the runner default **30s** (`ToolRunner::timeout_s_`) | Overrides the 30s default for this tool only. |

A `Tool` block with neither `native` nor `mcp` is silently ignored.

**The runner-wide default (30s) is NOT manifest-tunable.** `wire_agent` calls the ToolRunner's
`on_start` with an **empty** block, so its `timeout` key never sees the manifest. Use per-tool
`timeout_s` to change a tool's cap.

**Argv appends — why store paths must be whitespace-free.** For certain tools the wiring appends a
resolved path to the tool's `native` command (single source of truth so the LLM can't redirect it).
Because argv is whitespace-split, these appended paths are rejected (`MalConfig`) if they contain
whitespace:

| Tool | Appended to its argv | Source key | Extra rule |
|---|---|---|---|
| `save_memory` | archival store path | `Memory.store` (default `.hades/memory.jsonl`) | store path must be whitespace-free |
| `pin_fact` | core memory file | `Session.memory_file` | **requires `Session.memory_file`** (else `MalConfig`) |
| `use_skill` / `save_skill` | skills dir | `Skills.dir` (default `skills`) | dir must be whitespace-free |
| `ask_agent` | `<own_name> <secret_env> <ask_timeout_s> <peer=url>…` | `Bridge` + `Peer` blocks | see rules below |

**`ask_agent` wiring rules** (`wire_agent`): the tool requires **at least one `Peer` block** *and*
a valid `Bridge.name` — else `MalConfig` ("nobody to call" / "requires Bridge { name }"). Its
`timeout_s` is auto-set to `Bridge.ask_timeout_s + 10` so the tool reports its own timeout instead
of being killed mid-write.

**Dev tools (`grep`, `glob`, `edit_file`, `git_read`, `run_command`).** The five coding tools take
**no** argv append — their scoping comes from `capability_policy` (see §5), not a wired path, so
they need no extra block. `run_command` carries `timeout_s = 600` in dev.hades (builds/tests run
long); the other four use the 30s runner default. All five are plain `native` blocks.

---

## 5. `Objective` blocks — `Objective = <name> { … }`

`app/agent_wiring.cpp` (`make_objective`). Each objective gates proposed actions at the Arbiter's
veto seam. Unknown objective names are skipped (a warning, never fatal). **Inert unless rostered** —
no `Objective =` line means no gating from that objective.

Registration order matters: the first *hard* veto wins. If the `bridge` module is present a
`PeerLoopGuard` is auto-registered first (not manifest-optional). Then manifest objectives in file
order. dev.hades registers `capability_policy` before `avoid_destructive` so a capability hard-veto
short-circuits first, leaving `avoid_destructive` as the destructive-pattern backstop.

### `stay_on_budget`
| Key | What it does | Default |
|---|---|---|
| `hard_cap_usd` | Hard-veto once cumulative LLM spend reaches this many USD. | `0` |

Spend is metered from `Session.price_per_mtok` × tokens. Embeddings are **not** metered.

### `avoid_destructive`
Confirm-gates destructive shell idioms (`rm -r` — which also covers `rm -rf` — plus `mkfs`,
`dd if=`, fork-bomb, `> /dev/`, `shutdown`, `reboot`, `chmod -R 000`; 9 substring patterns) and
**always confirm-gates `write_file`**. Best-effort heuristic, not a security boundary.

| Key | What it does |
|---|---|
| `veto` | *(currently unread)* — the block takes no config; `veto = true` in dev.hades is decorative. |

### `capability_policy`
The real tool-safety gate. Reads scopes from the manifest and maps each tool to a verdict via a
built-in `capability_of` table (the authority — a tool can't grant itself permission).

| Key | What it does | Default |
|---|---|---|
| `fs_read_allow` | Whitespace-separated path prefixes `fs_read`/`list_dir`/`grep`/`glob` may read silently. | empty |
| `fs_deny` | Whitespace-separated prefixes hard-vetoed for **both** read and write. | empty |
| `fs_write_allow` | Whitespace-separated path prefixes `write_file`/`edit_file` may write **without** confirm. Empty → every write confirms (the pre-scope behavior). `fs_deny` still hard-vetoes. | empty |
| `exec_allow` | **Comma-separated** command prefixes `run_command` may run without confirm — the **one non-whitespace list** (prefixes contain spaces, e.g. `cmake --build build, ctest --test-dir build`). Matched at a token boundary (`ctest` matches `ctest --x`, never `ctest-evil`). | empty |
| `net_deny_hosts` | Whitespace-separated host substrings hard-vetoed for `http_fetch`. | empty |
| `block_private_net` | Hard-veto fetches to loopback / RFC1918 / link-local / obfuscated-numeric hosts (SSRF guard). | `true` |
| `confirm_unscoped` | An out-of-`fs_read_allow` read → confirm (`true`) or hard-veto (`false`). | `true` |

Path matching is **lexical** (`./x`, `././x`, `x` all normalize together); a surviving `..`
→ confirm. Allow-matching is path-boundary-aware (`./workspace` does not also allow
`./workspace-backup`). Host matching is case-insensitive substring.

**`exec_allow` — allowlist SPECIFIC invocations and know your binaries.** A prefix whose binary has
a run-a-script flag (`ctest -S`, `cmake -P`, `make` with attacker-chosen targets) grants more than
its name suggests — the trailing arguments are inside the granted trust. Allow the exact invocation
you mean (`ctest --test-dir build`), not a bare tool name that can be steered into running code.

#### Capability verdict table (what each tool gets)

This is the table that confuses operators: **`shell` and unknown tools are ALWAYS confirm-gated
regardless of your scopes.** File reads/writes, `http_fetch` and `run_command` are scope-tunable
(`fs_read_allow` / `fs_write_allow` / `net_deny_hosts` / `exec_allow`); `git_read` is always allow.

| Tool(s) | Capability | Verdict |
|---|---|---|
| `fs_read`, `list_dir`, `grep`, `glob` | FsRead | deny if under `fs_deny`; confirm if it escapes via `..`; **allow** if under `fs_read_allow`; else confirm (or deny if `confirm_unscoped=false`). Scope-tunable. |
| `http_fetch` | Net | deny if host empty/unparseable; deny if private and `block_private_net`; deny if a `net_deny_hosts` substring; else **allow**. Scope-tunable. |
| `write_file`, `edit_file` | FsWrite | deny if under `fs_deny`; confirm if it escapes via `..`; **allow** if under `fs_write_allow`; else confirm. Scope-tunable. |
| `run_command` | ExecScoped | confirm if command empty/non-string; confirm if it has shell metacharacters (`;\|&$\`()<>`); **allow** if it matches an `exec_allow` prefix at a token boundary; else confirm. Scope-tunable. |
| `git_read` | GitRead | **always allow** — read-only by construction (fixed argv per op, no shell, leading-dash paths rejected, `--` before pathspecs). |
| `shell` | Exec | **always confirm**. |
| unknown tool | Unknown | **always confirm**. |
| `save_memory`, `pin_fact` | MemoryAppend | **always allow** (append-only to the agent's own files). |
| `use_skill` | SkillRead | **always allow**. |
| `save_skill` | SkillWrite | **always allow** (enum kept distinct so a future policy can confirm-gate it). |
| `ask_agent` | PeerAsk | **always allow** (the receiving agent's own gates are the real protection). |

**Documented v1 gaps (v2):** DNS-rebind/TOCTOU (host string checked, cpr connects later),
symlink path-deny bypass (lexical, not realpath), no positive net egress allowlist. `git_read` is
**always allow**: a git-TRACKED and MODIFIED file listed in `fs_deny` still surfaces its content via
`diff` (`fs_deny` gates `fs_read` paths, not git-surfaced content). Keep real secrets **gitignored**.

---

## 6. `Memory` block

`src/apps/memory/memory.cpp` (`on_start`). Requires `Module = memory`.

| Key | What it does | Default |
|---|---|---|
| `store` | Archival memory `.jsonl` file (keyword-ranked each turn). | `.hades/memory.jsonl` |
| `top_n` | How many ranked memories to inject per turn. | `5` |

`store` is also appended to the `save_memory` tool's argv (single source of truth) — keep it
whitespace-free.

---

## 7. `Embedding` block

`src/apps/embedding_memory/embedding_memory.cpp` (`on_start`). Requires `Module = embedding_memory`
(opt-in; omit to stay keyword-only).

| Key | What it does | Default |
|---|---|---|
| `provider` | `http` (OpenAI-compat `/embeddings`) or `subprocess`. | `subprocess` |
| `command` | Subprocess embedder command (when `provider = subprocess`). | `""` |
| `endpoint` | **Base** URL (when `provider = http`). | `""` |
| `api_key_env` | Env var name for the embed key (http provider). | `HADES_EMBED_KEY` |
| `model` | Embedding model id (http provider). | `""` |
| `memory_store` | Archival file to index. | `.hades/memory.jsonl` |
| `cache_dir` | Where the vector cache lives (`<dir>/memory.vec.jsonl`). | `.hades/embeddings` |
| `sessions_dir` | Session `.jsonl` dir to index (when `index_sessions`). | `.hades/sessions` |
| `index_sessions` | Also index past sessions (semantic recall of prior chats). | `true` |
| `top_n` | Hits injected per turn. | `5` (`kDefaultEmbedTopN`) |
| `min_similarity` | Cosine floor for a hit. | `0.25` (`kDefaultMinSimilarity`) |
| `batch_size` | Embed batch size while indexing. | `32` (`kDefaultEmbedBatch`) |
| `timeout_s` | Per-embed HTTP/subprocess timeout. | `120` (`kDefaultEmbedTimeoutS`) |
| `reindex_interval_s` | Periodic reindex interval; **`0` = off** (index once at launch). | `86400` (`kDefaultReindexIntervalS`) |

**Gotchas.**
- **`endpoint` must be the BASE url, NOT `.../embeddings`** — the http provider appends
  `/embeddings`. PPQ: `endpoint = https://api.ppq.ai/v1`. Getting this wrong fails every embed and
  silently degrades to keyword-only (`EMBED_INDEX_DONE=false`, no cache file).
- **Cross-block path drift:** `memory_store` and `sessions_dir` are the Embedding block's *own*
  keys. If you change `Memory.store` or `Session.sessions_dir` you must change these too, or the
  embedder indexes the wrong file/dir. dev.hades relies on all three defaulting to the same paths.
- `api_key_env` defaults to **`HADES_EMBED_KEY`** here (not `HADES_API_KEY`) — set it explicitly to
  reuse your LLM key (dev.hades sets `HADES_API_KEY`).
- `reindex_interval_s` parses `0` specially (off); a negative/garbage value keeps the daily default.
- Everything fails soft: any embedder error degrades to keyword-only, never crashes a turn.
- Embedding cost is **not** metered by `stay_on_budget`.
- dev.hades sets `min_similarity = 0.45` (higher than the `0.25` default) and `top_n = 3`.

---

## 8. `Skills` block

`src/apps/skills/skills.cpp` (`resolve_skills_dir`). Requires `Module = skills`.

| Key | What it does | Default |
|---|---|---|
| `dir` | Directory of `<name>/SKILL.md` skill packs. | `skills` |

`dir` is appended to the `use_skill`/`save_skill` tool argv (single source of truth) — keep it
whitespace-free. Skill names are gated to `[A-Za-z0-9_-]{1,64}` in the tools (no path traversal).
The dir is git-tracked in this repo (the agent writes skills at runtime → working-tree churn).

---

## 9. `Serve` block

`src/apps/serve/serve.cpp`. Used by `--serve`.

| Key | What it does | Default |
|---|---|---|
| `host` | Bind address. | `127.0.0.1` |
| `port` | Bind port (1–65535). | `8080` |
| `webroot` | Static web dir served at `/`. | `web` |

**Gotchas.**
- `--serve <port>` on the CLI overrides `Serve.port` (out-of-range CLI port is ignored).
- `webroot` is **cwd-relative** — run `--serve` from the repo root.
- Loopback by default; set `host = 0.0.0.0` for LAN. There is **no password auth** — the web UI
  and `POST /chat`,`/confirm` are protected only by an `X-Hades` CSRF header (don't strip it
  client-side). Keep the agent on a trusted network.
- An out-of-range or garbage `port` keeps the 8080 default.

---

## 10. `Telegram` block

`src/apps/telegram/telegram.cpp` (`on_start`). Requires `Module = telegram`.

| Key | What it does | Default |
|---|---|---|
| `allow_users` | Whitespace-separated **numeric** Telegram user ids allowed to drive the bot. **REQUIRED.** | — (missing/empty → `MalConfig`) |
| `token_env` | Env var name holding the bot token. | `TELEGRAM_BOT_TOKEN` |
| `poll_timeout_s` | `getUpdates` long-poll timeout. | `50` |

**Gotchas.**
- `allow_users` is mandatory and strictly numeric — an open bot lets anyone who finds it drive your
  agent. A non-numeric id → `MalConfig`. Non-allowed senders are silently dropped.
- **Private chat only (v1):** group messages are dropped (replies would be group-readable).
- Token via env only, never in the manifest; redacted in `session.log`. Keep it in a gitignored
  `.env` you `source`.
- Startup backlog is drained and **discarded** — a command queued while the agent was down never
  replays. The poll offset is in-memory (a crash mid-turn loses the update id).
- The dtor's join can wait up to one `poll_timeout_s` cycle, so `/quit` can feel slow — shorten
  `poll_timeout_s` if that matters.

---

## 11. `Bridge` block + `Peer` blocks

`src/apps/bridge/bridge.cpp` (`on_start`) + `app/agent_wiring.cpp`. The `Bridge` block is the
agent's **identity** and is read even without `Module = bridge` (the `ask_agent` tool needs
`Bridge.name`); the `bridge` **module** adds the inbound HTTP listener.

### `Bridge`
| Key | What it does | Default |
|---|---|---|
| `name` | This agent's peer name (embedded in `PEER.<name>.` keys and the `peer:<name>` turn origin). **REQUIRED** when the module is rostered; must match `[A-Za-z0-9_-]{1,64}`. | — (invalid/missing → `MalConfig`) |
| `host` | Listener bind address. | `127.0.0.1` |
| `port` | Listener port; **`0` = ephemeral** (bind any free port; tests use this). Out-of-range → `9090`. | `9090` |
| `secret_env` | Env var name for the shared bridge secret. **Must be set** in the env when the module is rostered (unset/empty → `MalConfig`). | `HADES_BRIDGE_SECRET` |
| `share_out` | Whitespace-separated bus keys to push to all peers on change (`/share`). | empty |
| `max_hops` | Inbound hop limit; a peer request at/above this is rejected. | `1` |
| `ask_timeout_s` | Timeout for outbound `ask_agent` peer calls (also drives the tool's `timeout_s`). | `180` (`kDefaultAskTimeoutS`) |

### `Peer = <name> { url = … }`
One block per peer the agent can call.

| Key | What it does | Default |
|---|---|---|
| `url` | Base URL of the peer's bridge listener. **Required**, whitespace-free. | — (missing/empty → `MalConfig`) |

Rules (`wire_agent`): peer `<name>` must match `[A-Za-z0-9_-]{1,64}`; duplicate peer names →
`MalConfig`. The `ask_agent` tool requires ≥1 `Peer` **and** a valid `Bridge.name`.

**Gotchas.**
- Inbound `/ask` and `/share` are auth-gated (shared secret header) **and** peer-allowlisted (`from`
  must be a known `Peer`) → **403** on bad secret / unknown peer / malformed body.
- A peer `/ask` drives a **normal turn** through *this* agent's own objectives; a confirm-gated
  action inside a peer turn is **auto-denied** (peers can't grant human confirmation).
- Shares arrive as bus key `PEER.<from>.<key>` (rename-on-arrival — a peer can't inject a local key).
- `PeerLoopGuard` hard-vetoes `ask_agent` on peer-origin turns (v1 `max_hops = 1`; no forwarding).
- The secret is the same on every fleet member (v1); via env only, redacted in `session.log`.
- The listener thread is started by `hades_main` after wiring (not in `on_attach`).

---

## 12. `Arbiter` block

`Module = arbiter` builds the Arbiter, but the **`Arbiter { … }` block is currently unread**. The
`policy = v1` key in dev.hades is decorative (no code queries `m.of("Arbiter")`). It reserves the
block for future config; today it does nothing.

---

## Appendix A — CLI flags

`app/hades_main.cpp`.

| Flag | Effect |
|---|---|
| `hades <manifest>` | Build the agent and run the stdin REPL (or block on a poll/listener-only roster). |
| `--serve [port]` | Run the HTTP/web front-end instead of the REPL. Optional port overrides `Serve.port`. |
| `--resume [id]` | Reload a prior session. With `id`: that session (missing → `MalConfig`). Without: the newest `*.jsonl` in `sessions_dir`; none found → starts fresh with a note. Composes with `--serve`. |

Missing manifest arg → exit 2. Any `MalConfig` (bad manifest, unset key env, missing resume id) →
exit 1 with a message.

## Appendix B — Environment variables & the `.env` convention

The manifest never holds secrets — it names the env var. Each is redacted in `session.log`.

| Env var (default name) | Set by manifest key | Used for |
|---|---|---|
| `HADES_API_KEY` | `Session.api_key_env` | LLM API key (**required**). |
| `HADES_EMBED_KEY` | `Embedding.api_key_env` | Embedding key (http provider; dev.hades points it at `HADES_API_KEY`). |
| `TELEGRAM_BOT_TOKEN` | `Telegram.token_env` | Telegram bot token (required with `Module = telegram`). |
| `HADES_BRIDGE_SECRET` | `Bridge.secret_env` | Shared bridge secret (required with `Module = bridge`). |

Convention: keep these in a **gitignored `.env`** and `source` it before launch. Runtime stores
(`.hades/…`) are gitignored; `memory/facts.md` and `skills/` are git-tracked and mutated by the
agent at runtime (expect working-tree churn to review or gitignore).

## Appendix C — Keys that exist but are unread (decorative)

Found by tracing every consumer:

- `Session.provider` — the LLM transport is fixed (always OpenAI-compatible).
- `Objective = avoid_destructive { veto = … }` — the objective takes no config.
- `Arbiter { … }` (whole block, e.g. `policy = v1`) — no code reads it.

These are harmless but do nothing. Every other key above is read by the file cited in its section.
