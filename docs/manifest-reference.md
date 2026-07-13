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
| `status` | Turn-stats aggregator: posts `AGENT_STATUS` (`{ctx_tokens, spent_usd, turn, model, line}`) from the traffic each turn already produces; the REPL prints the `line` dim under each reply (`[ctx 12.4k tok · $0.0372 · turn 9 · gpt-5.5]`). No config block. `ctx_tokens` = prompt+completion of the last LLM call; `/new` resets ctx/turn, spend stays process-cumulative. | No stats line; REPL output unchanged. |
| `chat` | stdin REPL front-end. | No REPL. |
| `serve` | HTTP/web front-end (`--serve`). | `--serve` errors "no serve module". |
| `telegram` | Telegram long-poll bot (reads `Telegram` block). | No bot. |
| `simplex` | SimpleX front-end via a local `simplex-chat` daemon (reads `Simplex` block). | No SimpleX bot. |
| `bridge` | Agent↔agent HTTP listener (reads `Bridge` block). | No inbound peer surface. |
| `heartbeat` | Timer that fires the agent's own turns — on a cron schedule or a reactive `when` condition (reads `Heartbeat` blocks). | No self-triggered turns, no watches; the agent is purely event-driven. |

**Front-end requirement.** After building, the binary needs *some* front-end or it errors:
`--serve` uses `serve`; otherwise it runs `chat`'s REPL; if there is no `chat`, it blocks on
`telegram` (poll), `simplex` (event thread), `bridge` (listener), or `heartbeat` (timer); with none
of those it errors "no chat module".

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
| `memory_file` | Core "always-on" memory file; the Arbiter re-reads it each turn; `core_memory` edits it. | `""` | **Required if the `core_memory` tool is rostered** (else `MalConfig`). Path must be whitespace-free. |
| `memory_char_limit` | Char cap on the core-memory file (it is in EVERY turn's prompt). An over-cap `core_memory` write fails with the entry list so the agent consolidates. | `2400` | Bad/`<=0` value → default. |
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
| `mcp` | Local MCP server command (stdio transport, spawned per exchange). | — | Tools DISCOVERED at boot and announced as `<block>__<tool>` (see below). |
| `mcp_url` | Remote MCP server URL (Streamable HTTP transport). | — | Same discovery/announce; Bearer auth via `api_key_env`. |
| `api_key_env` | Env var holding the Bearer token for `mcp_url`. | — | Env-only (never in the manifest); absent → no auth header. Ignored for `native`/`mcp`. |
| `timeout_s` | Per-tool subprocess cap. | `0` → the runner default **30s** (`ToolRunner::timeout_s_`) | Overrides the 30s default for this tool only. |

A `Tool` block with none of `native`/`mcp`/`mcp_url` is silently ignored.

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
| `core_memory` | core memory file + char cap | `Session.memory_file`, `Session.memory_char_limit` | **requires `Session.memory_file`** (else `MalConfig`) |
| `use_skill` / `save_skill` | skills dir | `Skills.dir` (default `skills`) | dir must be whitespace-free |
| `ask_agent` | `<own_name> <secret_env> <ask_timeout_s> <peer=url>…` | `Bridge` + `Peer` blocks | see rules below |
| `schedule_task` | `<cron_store> <max_tasks> <min_interval_s>` | unnamed `Heartbeat { }` block (§15) | **requires `Module = heartbeat`** (else `MalConfig`); store path whitespace-free |
| `list_tasks` / `cancel_task` | `<cron_store>` | unnamed `Heartbeat { }` block (§15) | store path whitespace-free |

**`ask_agent` wiring rules** (`wire_agent`): the tool requires **at least one `Peer` block** *and*
a valid `Bridge.name` — else `MalConfig` ("nobody to call" / "requires Bridge { name }"). Its
`timeout_s` is auto-set to `Bridge.ask_timeout_s + 10` so the tool reports its own timeout instead
of being killed mid-write.

**Dev tools (`grep`, `glob`, `edit_file`, `git_read`, `run_command`).** The five coding tools take
**no** argv append — their scoping comes from `capability_policy` (see §5), not a wired path, so
they need no extra block. `run_command` carries `timeout_s = 600` in dev.hades (builds/tests run
long); the other four use the 30s runner default. All five are plain `native` blocks.

**Staleness guard (`fs_read` / `edit_file` / `write_file` — always on, no configuration).** The two
mutating tools are protected against lost updates: `fs_read` (and each successful edit/write) reports
a content-hash `version`, the Arbiter remembers it per file, and injects an `expect_version` into
`edit_file`/`write_file` requests. If the file changed on disk since the agent last observed it, the
tool refuses — file untouched — with `"file changed on disk since you last read it — fs_read it again
and retry"`, and the agent recovers by re-reading (no confirmation prompt; works on heartbeat/peer
turns too). Operators see the injected `expect_version` inside `TOOL_REQUEST` in the Eventlog. A file
the agent never read is not gated (staleness only); writes made via `shell` are invisible to the
guard until the next `fs_read`. `write_file` writes atomically (tmp + rename, mode preserved).

### MCP servers — discovery, naming, gating

One `Tool` block = one MCP **server** (not one tool). At boot (registry warm) hades performs a
`tools/list` exchange per server and announces every discovered tool to the LLM as
**`<block>__<toolname>`** — e.g. block `weather` exposing `get_alerts` announces
`weather__get_alerts`. The prefix guarantees a server can never shadow a native tool name
(`fs_read`, `shell`, …) and inherit its capability verdict. At most ONE of
`native | mcp | mcp_url` per block (a block with none is silently ignored, as above), and an
mcp block name must be `[A-Za-z0-9_-]{1,64}`
without `__` — both enforced at launch (`MalConfig`). Discovered tool names are themselves
charset-gated the same way (`[A-Za-z0-9_-]{1,64}`): a name with provider-illegal characters
(`/`, space, …) is skipped rather than 400 the whole tools array at the LLM API. Duplicate
discovered names dedup first-wins — a buggy server listing a name twice announces it once.

    # local (stdio) server — needs its runtime (node/python) on the box
    Tool = weather { mcp = npx -y @h1deya/mcp-server-weather }

    # remote (Streamable HTTP) server — token via env, never in the manifest
    Tool = linear
    {
      mcp_url     = https://mcp.linear.example/mcp
      api_key_env = LINEAR_MCP_KEY
      timeout_s   = 60
    }

**Transport.** `mcp` spawns the command per exchange (one-shot: initialize + request over
newline-delimited JSON-RPC on stdio). `mcp_url` speaks MCP Streamable HTTP: JSON-RPC over
POST, `Mcp-Session-Id` honored, responses accepted as plain JSON **or** SSE-framed, redirects
disabled, best-effort session `DELETE`. Auth is Bearer-only — a server requiring OAuth login
flows should be bridged through the stdio path instead: `Tool = x { mcp = npx -y mcp-remote
https://server.example/mcp }`.

**Gating.** Every `<block>__<tool>` maps to the `McpTool` capability: **confirm by default**
(each call needs human approval; heartbeat/peer turns auto-deny), unless listed in the
`capability_policy` scope `mcp_allow` (exact prefixed names, whitespace-separated; the single
literal `*` allows every discovered MCP tool — that trusts every rostered server, prefer
naming tools). `timeout_s` covers both the discovery exchange and each call.

**Failure + trust notes.** A server that is down (or lists nothing) at boot degrades
fail-soft: nothing announced, one stderr log line, and the legacy call-by-block-name path
remains. `mcp_url` is operator-set (not LLM-chosen), so the private-net/SSRF gate does NOT
apply to it — a loopback self-hosted server is legitimate; the gate stays on `http_fetch`.
Discovered tool descriptions/schemas are server-controlled text entering the prompt — roster
only servers you trust.

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
`dd if=`, fork-bomb, `> /dev/`, `shutdown`, `reboot`, `chmod -R 000`; 9 entries — 8 idioms,
`rm -rf`/`rm -r` both listed) and
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
| `mcp_allow` | Discovered MCP tools (`<block>__<tool>`, whitespace-separated; literal `*` = all) allowed without confirm. | empty (every MCP call confirms) |

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
| `run_command` | ExecScoped | confirm if command empty/non-string; confirm if it has shell metacharacters (`;\|&$\`()<>` or a newline); **allow** if it matches an `exec_allow` prefix at a token boundary; else confirm. Scope-tunable. |
| `git_read` | GitRead | **always allow** — read-only by construction (fixed argv per op, no shell, leading-dash paths rejected, `--` before pathspecs). |
| `shell` | Exec | **always confirm**. |
| unknown tool | Unknown | **always confirm**. |
| `save_memory`, `core_memory` | MemoryAppend | **always allow** (the agent's own memory files; `core_memory` also edits/removes — curation must be frictionless). |
| `use_skill` | SkillRead | **always allow**. |
| `save_skill` | SkillWrite | **always allow** (enum kept distinct so a future policy can confirm-gate it). |
| `ask_agent` | PeerAsk | **always allow** (the receiving agent's own gates are the real protection). |
| `<block>__<tool>` (any MCP-discovered tool) | McpTool | in `mcp_allow` (or `*`) → allow; else **confirm** (heartbeat/peer turns auto-deny). |

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

**`save_skill` has two modes** (selected by which optional arg is non-empty; an empty string
counts as absent): a full **save** (`name` + `description` + `body` — creates or overwrites) and
a **patch** (`name` + `old_string`/`new_string` — edits part of an EXISTING skill without
resending the body; `old_string` must match exactly once, no `replace_all`). A patch that would
break the frontmatter (the scanner could no longer parse a description) is refused with the file
untouched, so the agent cannot brick a skill out of its own roster. Sending `body` and
`old_string` together, or a `description` alongside a patch, is an error.

**Shipped skill: `email`** (`skills/email/SKILL.md`). Read/send the user's mail via the
[himalaya](https://github.com/pimalaya/himalaya) CLI — no email module or credentials in hades
(accounts live in himalaya's own config). Requires the `run_command` + `shell` + `write_file` tools
and himalaya installed with a configured account (the skill walks the agent through install +
points the user at himalaya's account wizard). **Reads** (`himalaya envelope list`/`message read`)
run unattended only if the operator adds them to `capability_policy.exec_allow` (else confirm);
**sends** go through `shell` so they are always human-confirmed (auto-denied on heartbeat/peer
turns → the agent never mails anyone unattended). **Caveat on a bridged agent:** `exec_allow`
read-prefixes are allow-band for *peer*-driven turns too — a peer could read the user's mail; keep
himalaya out of `exec_allow` on peer-exposed workers (reads then confirm → auto-denied for peers).

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
- **Notify sink.** TelegramModule subscribes **`NOTIFY_USER`** and pushes each such message to every
  `allow_users` id — this is the delivery path for a `notify = true` `Heartbeat` (see §15). A
  notifying heartbeat with no `telegram` rostered posts `NOTIFY_USER` but nothing delivers it.

---

## 11. `Stt` block

`resolve_stt` (`app/agent_wiring.cpp`); providers in `src/stt/stt_providers.cpp`. **Opt-in: with no
`Stt` block the agent is text-only** (`Agent.stt == nullptr`). When present, one `SttProvider` is built
and injected into the **user-facing** front-end (Telegram v1) so a voice message is transcribed to text
and drives a normal turn. The **Bridge is never given one** — agent↔agent traffic stays text (a peer
cannot send audio). No `Module =` line is needed; the block's presence is the switch.

| Key | What it does | Default | Notes |
|---|---|---|---|
| `provider` | `http` (OpenAI-compat) or `command` (local wrapper). | `http` | Unknown value → `MalConfig`. |
| `endpoint` | **Base** URL of the transcription API (http provider). | — | **Required for `http`** (empty → `MalConfig`). The provider appends `/audio/transcriptions`. |
| `model` | Transcription model id (http provider). | `nova-3` | Sent as the multipart `model` field. `nova-3` is PPQ's Deepgram-backed STT model; use `whisper-1` only against an OpenAI-proper `/audio/transcriptions`. |
| `api_key_env` | **Name of the env var** holding the STT key (http provider). | `HADES_API_KEY` | Resolved from the env; empty → sent with no bearer. Redacted in `session.log`. |
| `language` | Spoken-language hint. English-only v1. | `en` | http: sent as the multipart `language` field. command: ignored by hades — bake the language flag into your wrapper script (`-l en`). |
| `timeout_s` | Per-transcription timeout (HTTP call or subprocess). | `60` | Bad/0/garbage → default. |
| `command` | Subprocess wrapper (command provider). | — | **Required for `command`** (empty → `MalConfig`). Whitespace-split into argv; the audio path is appended as the **last** arg. See `tools/whisper_reference.sh`. |

**Gotchas.**
- **`endpoint` must be the BASE url, NOT `.../audio/transcriptions`** — the http provider appends
  `/audio/transcriptions` (the same base-url gotcha as embedding's `/embeddings`). PPQ:
  `endpoint = https://api.ppq.ai/v1`.
- **PPQ `model = nova-3`** (Deepgram Nova-3, PPQ's default STT model) — NOT `whisper-1`. PPQ params:
  `file`, `model`, `language` (`en` or `multi`), optional `response_format`/`prompt`; returns `{"text":…}`.
- **Audio format:** Telegram voice notes are **OGG/Opus** (`.oga`); PPQ's documented format list
  (mp3/mp4/mpeg/mpga/m4a/wav/webm, max 25 MB) omits ogg. Deepgram accepts Ogg-Opus in practice, but if
  PPQ rejects a clip, transcode the temp file to wav/mp3 before upload (a v2 seam — not built).
- **Fail-soft everywhere.** A non-2xx status, unparseable response, missing `text`, subprocess timeout,
  non-zero exit, or empty transcript degrades to a "didn't catch that" reply — it never crashes a turn.
- **`command` provider:** the wrapper must print ONLY the plain transcript to stdout; run with
  `run_subprocess` (fork/exec, **no shell**). One-shot per clip (no warm child — voice notes are
  human-paced). Language is the wrapper's job (English v1); adjust the reference script's binary/model.
- **User-facing only / Bridge-excluded:** the seam is injected into Telegram (and future local-mic
  front-ends), never the Bridge — a peer agent cannot transcribe audio through this agent.

---

## 12. `Tts` block

`resolve_tts` (`app/agent_wiring.cpp`); providers in `src/tts/tts_providers.cpp`. **Opt-in: with no
`Tts` block the agent never speaks** (`Agent.tts == nullptr`). When present, one `TtsProvider` is built
and injected into the **user-facing** front-end (Telegram v1) so that on a **voice-origin** turn the
agent's reply is synthesized to a voice note and sent alongside the text (mirror modality — a typed
turn stays text-only). The **Bridge is never given one** — agent↔agent traffic stays text (a peer
cannot receive audio). No `Module =` line is needed; the block's presence is the switch.

| Key | What it does | Default | Notes |
|---|---|---|---|
| `provider` | `http` (OpenAI-compat) or `command` (local wrapper). | `http` | Unknown value → `MalConfig`. |
| `endpoint` | **Base** URL of the speech API (http provider). | — | **Required for `http`** (empty → `MalConfig`). The provider appends `/audio/speech`. |
| `model` | TTS model id (http provider). | `deepgram_aura_2` | Sent as the JSON `model` field. PPQ: `deepgram_aura_2` (Deepgram Aura) or an ElevenLabs model (`eleven_multilingual_v2`, `eleven_flash_v2_5`). For OpenAI-proper use `tts-1`/`gpt-4o-mini-tts`. |
| `voice` | Voice id (http provider). | `aura-2-arcas-en` | Sent as the JSON `voice` field. PPQ Aura voices: `aura-2-{arcas,thalia,andromeda,helena,apollo,aries}-en`; ElevenLabs = a voice id. OpenAI = `alloy` etc. |
| `api_key_env` | **Name of the env var** holding the TTS key (http provider). | `HADES_API_KEY` | Resolved from the env; empty → sent with no bearer. Redacted in `session.log`. |
| `max_chars` | Replies longer than this are NOT spoken (the text reply is still sent). | `4000` | Guards against a multi-minute synth. **Set ≤ the provider's input limit — PPQ: 2000 (Deepgram) / 5000 (ElevenLabs); over the limit the API 422s → fail-soft skip.** Bad/0/garbage → default. |
| `timeout_s` | Per-synthesis timeout (HTTP call or subprocess). | `60` | Bad/0/garbage → default. |
| `command` | Subprocess wrapper (command provider). | — | **Required for `command`** (empty → `MalConfig`). Whitespace-split into argv; reply TEXT on stdin, ogg-opus bytes on stdout. See `tools/piper_reference.sh`. |

**Gotchas.**
- **`endpoint` must be the BASE url, NOT `.../audio/speech`** — the http provider appends
  `/audio/speech` (the same base-url gotcha as STT's `/audio/transcriptions` and embedding's
  `/embeddings`). PPQ: `endpoint = https://api.ppq.ai/v1`; OpenAI: `https://api.openai.com/v1`.
- **OGG/Opus is required + the `response_format` risk.** Telegram `sendVoice` needs Ogg-Opus, so the
  provider MUST yield it: the http provider requests `response_format = opus`; the `command` wrapper must
  emit ogg-opus on stdout (the reference `tools/piper_reference.sh` pipes piper → `ffmpeg -c:a libopus -f
  ogg`). **PPQ's `/audio/speech` docs do NOT list `response_format`** — if PPQ ignores it and returns
  mp3/default, `sendVoice` rejects it → fail-soft skip (text stands). If a live PPQ test yields no voice,
  this is why; the v2 fix is a module-side mp3→ogg-opus transcode (not built).
- **Mirror modality — only voice-origin turns speak.** A typed message gets a text reply only; a voice
  message gets BOTH the text reply and a spoken voice note. Text is the anchor: it is sent first, then
  the voice note is best-effort.
- **Fail-soft everywhere.** A non-2xx status, empty audio, subprocess timeout/non-zero exit, or a
  `sendVoice` failure logs and leaves the already-delivered text reply as the answer — it never crashes a turn.
- **`command` provider:** the wrapper reads reply TEXT on stdin and must write ONLY ogg-opus bytes to
  stdout; run with `run_subprocess` (fork/exec, **no shell**). One-shot per reply.
- **User-facing only / Bridge-excluded:** the seam is injected into Telegram (and future local front-ends),
  never the Bridge — a peer agent never receives a voice reply from this agent.

---

## 13. `Bridge` block + `Peer` blocks

`src/apps/bridge/bridge.cpp` (`on_start`) + `app/agent_wiring.cpp`. The `Bridge` block is the
agent's **identity** and is read even without `Module = bridge` (the `ask_agent` tool needs
`Bridge.name`); the `bridge` **module** adds the inbound HTTP listener.

### `Bridge`
| Key | What it does | Default |
|---|---|---|
| `name` | This agent's peer name (embedded in `PEER.<name>.` keys and the `peer:<name>` turn origin). **REQUIRED** when the module is rostered; must match `[A-Za-z0-9_-]{1,64}`. | — (invalid/missing → `MalConfig`) |
| `description` | One-line persona shown in this agent's `/card` (its `description` field). | the bridge `name` |
| `host` | Listener bind address. **Cross-machine peers need `0.0.0.0`** (or the LAN IP) — the loopback default refuses LAN connections before any firewall matters. And the port must be reachable inbound on each machine: NixOS → `networking.firewall.allowedTCPPorts`; Raspberry Pi OS Lite ships no firewall. Traffic flows both ways (each peer pulls `/card` and pushes `/ask`/`/share`), so **both** ends need this. LAN-only — don't port-forward it; every endpoint is secret-gated. | `127.0.0.1` |
| `port` | Listener port; **`0` = ephemeral** (bind any free port; tests use this). Out-of-range → `9090`. | `9090` |
| `secret_env` | Env var name for the shared bridge secret. **Must be set** in the env when the module is rostered (unset/empty → `MalConfig`). | `HADES_BRIDGE_SECRET` |
| `share_out` | Whitespace-separated bus keys to push to all peers on change (`/share`). | empty |
| `max_hops` | Inbound hop limit; a peer request at/above this is rejected. | `1` |
| `ask_timeout_s` | Timeout for outbound `ask_agent` peer calls (also drives the tool's `timeout_s`). | `180` (`kDefaultAskTimeoutS`) |
| `discover_interval_s` | Seconds between re-pulling each `Peer`'s `/card`. Literal **`0` = boot-pull only** (no periodic discovery thread). | `300` |

### `Peer = <name> { url = … }`
One block per peer the agent can call.

| Key | What it does | Default |
|---|---|---|
| `url` | Base URL of the peer's bridge listener. **Required**, whitespace-free. | — (missing/empty → `MalConfig`) |
| `trust` | `trusted` \| `untrusted`. Labels this peer's shared facts on consumption (trusted → "`<peer>` reports:", untrusted → "unverified claim from `<peer>`:"). The untrusted tier is the seam for future dynamic joiners; all manifest peers are allowlisted, so v1 leaves it `trusted`. | `trusted` |

**A `Peer` with `trust` MUST be a multi-line block** — the parser is one-kv-per-line and **fails
loud** (`MalConfig`) on two `key = value` pairs packed on one physical line, so never write
`Peer = watcher { url = …  trust = untrusted }`. Write:
```
Peer = watcher
{
  url   = http://10.0.0.9:9090
  trust = untrusted
}
```

Rules (`wire_agent`): peer `<name>` must match `[A-Za-z0-9_-]{1,64}`; duplicate peer names →
`MalConfig`. The `ask_agent` tool requires ≥1 `Peer` **and** a valid `Bridge.name`.

### Bridge protocol — card discovery + typed `/share`
On top of `/ask` (a turn) and a raw `/share`, bridged agents exchange **structured** state over two
channels. Both are backward-compatible (nothing new required; an agent with no protocol-aware peers
behaves exactly as before).

**Channel 1 — `GET /card` (pull, capability discovery).** Each bridged agent serves a **secret-gated**
`GET /card` (same shared-secret header as `/ask`/`/share`; **not public** — a public card at
`/.well-known/agent.json` for real cross-harness A2A interop is deferred v2). The card is built
on demand, A2A-shaped:
```
{ "name":…, "description":…, "url":…, "version":…,
  "capabilities": { "streaming": false },
  "skills": [ { "id":…, "description":… } ],   // reverse-parsed from SKILLS_ANNOUNCE
  "tools":  [ { "name":… } ],                   // from the module roster
  "caps":   { "fs_read":…, "fs_write":…, "exec":…, "net":… } }  // capability_policy SUMMARY
```
`caps` reports **categories only** (`"scoped"` / `"none"` / `"public"` / `"private-blocked"`) — a peer
**never** learns your literal `fs_*_allow`/`exec_allow` paths or command strings, nor `fs_deny` entries.
A discovery timer re-pulls each `Peer`'s `/card` every `discover_interval_s` (default 300; **`0` = boot
pull only**) and posts **`PEER.<peer>.card`** on the bus.

**Channel 2 — typed `/share` (push).** The `/share` envelope gained a **`type`** field (absent → `"raw"`,
so legacy shares are unchanged):
| `type` | Receiver bus var | Payload |
|---|---|---|
| `card` | `PEER.<from>.card` | the sender's agent-card. Also the **boot self-announce**: an agent pushes its own card to every peer on boot and whenever its skills change → discovery is **boot-order-independent** (no dependence on who started first). |
| `fact` | `PEER.<from>.fact.<key>` | `{ from, trust, text }` — a shared fact, trust-tiered per the `Peer.trust` label. |
| `raw`  | `PEER.<from>.<key>` | opaque value (legacy `/share`). |

Rename-on-arrival holds for **every** type — a peer can only ever write a `PEER.*` bus key, never a
local one.

**Consumption (Arbiter).** The Arbiter subscribes `PEER.*` and folds two blocks into the leading system
message at turn start (both empty → nothing, backward-compatible):
- **"Peers you can delegate to (use `ask_agent` by advertised capability):"** — from `PEER.*.card`
  (each peer's name + skills + `caps`), so the agent routes `ask_agent` by a peer's *advertised*
  capability instead of blind.
- **"Reported by peers (treat as claims, re-verify before acting):"** — from `PEER.*.fact.*`,
  trust-labeled (`"<peer> reports:"` vs `"unverified claim from <peer>:"`).

**Security.** `/card` is secret-gated (not public); `caps` is a summary so a peer never learns your
literal allowlist; a peer can never write a non-`PEER.*` local bus key (rename-on-arrival, all types).

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

## 14. `Arbiter` block

`Module = arbiter` builds the Arbiter, but the **`Arbiter { … }` block is currently unread**. The
`policy = v1` key in dev.hades is decorative (no code queries `m.of("Arbiter")`). It reserves the
block for future config; today it does nothing.

---

## 15. `Heartbeat` blocks — `Heartbeat = <name> { … }`

`src/apps/heartbeat/heartbeat.cpp` (`HeartbeatModule`), `src/apps/heartbeat/cron.cpp` (the matcher),
wiring in `app/agent_wiring.cpp`. Requires **`Module = heartbeat`** (opt-in; omit → `agent.heartbeat
== nullptr`, no self-turns — the agent stays purely event-driven, a turn fires only on a human/peer
message).

This is the **autonomy leg**: the agent acts on its own. The module owns a timer thread that wakes
~every 30 s and, for each `Heartbeat` entry whose 5-field cron matches the current machine-local
minute (deduped so an entry fires at most once per minute), fires a **self-turn**. One
`Heartbeat = <name>` block = one scheduled task; roster as many as you like.

| Key | What it does | Default | Notes |
|---|---|---|---|
| `schedule` | 5-field cron `min hour dom month dow`. | — | **Exactly one of `schedule`/`when` is REQUIRED** (both or neither → `MalConfig`). An invalid expression → `MalConfig` at launch. See the cron subset below. |
| `when` | A reactive condition — fire when a Blackboard variable changes or crosses a threshold. | — | The alternative to `schedule`. Malformed → `MalConfig`. See the `when` conditions section below. |
| `cooldown_s` | Min seconds between fires of a `when` entry (flap absorber). | `60` | `when` entries only. Bounded `0..1e9`; negative/garbage/out-of-range → default. |
| `prompt` | The task text sent as the turn's user message (inline). | — | One of `prompt`/`prompt_file` is **REQUIRED** (neither → `MalConfig`). **See the `=` footgun below** — an inline prompt containing `=` will refuse to boot. |
| `prompt_file` | Path to a file whose contents are the task text. | — | Alternative to `prompt`. Unreadable path → `MalConfig`; the resolved text being empty → `MalConfig`. cwd-relative. Use this for any prompt with an `=` or multiple lines. |
| `notify` | Whether the tick's reply is delivered to the user. | `false` | `false` → the reply is dropped (a silent background task — the tool actions *are* the output). `true` → the reply is forwarded (see the notify flow below). |

If both `prompt` and `prompt_file` are given, `prompt` wins.

### `when` conditions — reactive triggers

Instead of a schedule, an entry may fire on a **Blackboard condition**, evaluated against the latest
value on each ~30 s scan (so reaction latency is up to ~30 s — a polling design, the MOOS `Iterate()`
shape; a bus subscription cannot drive a turn). Five forms, keyword operators (an `=`/`==` inside an
inline value would trip the one-kv-per-line fail-loud parser):

```
when = PEER.pi0.card changes        # any change of the value
when = MISSION_STATE is returning    # string equals (edge)
when = MISSION_STATE not idle        # string differs (edge)
when = BUDGET_SPENT_USD above 0.8    # numeric > (edge)
when = GPS_QUALITY below 4           # numeric < (edge)
```

Any Blackboard key is watchable (including `PEER.*` vars a peer shared). **Edge-triggered, not
level:** `changes` arms on its first observation (no fire) and fires once per subsequent change;
the other four fire once when the condition *becomes* true and re-arm only after it goes false. A
condition already true at boot fires once on the first scan. An entry skipped because a turn holds
the TurnGate keeps its edge and retries next scan. **`cooldown_s`** (default 60) suppresses re-fires
after one runs; edges arriving inside the cooldown are **absorbed, not queued** — and note the
corollary: an alarm that flapped true *during* a cooldown and stays true is edge-latched and will
not re-fire when the cooldown expires (only a fresh false→true edge fires). Absent key → condition
false, never an error.

The same conditions are available to the **agent at runtime** via `schedule_task` (`when` as the
timing kind + optional `cooldown_s` — see the self-scheduling subsection): "watch X and tell me when
it changes" becomes a dynamic watch, listed by `list_tasks` and removable by `cancel_task`.

### The cron subset

`schedule` is a **standard 5-field cron** expression: `minute hour day-of-month month day-of-week`,
separated by whitespace. Each field supports:

- `*` — every value;
- `N` — a literal (`30`, `6`);
- `A-B` — an inclusive range (`9-17`);
- `A-B/N` and `*/N` — a stepped range / every-Nth (`*/10` = every 10th, `0-30/5`);
- comma lists — `1,15,45` (any of the above, joined).

Fields are **ANDed** (all five must match; the Vixie dom-OR-dow quirk is intentionally *not*
implemented). Resolution is **one minute** — the finest schedule is a specific minute. Times are
**machine-local** (evaluated against `localtime_r`), so a `0 6 * * *` daily task fires at 06:00 in
the host's timezone — set the box's TZ deliberately. A malformed expression (wrong field count, a
range out of order, garbage) is rejected loud at launch (`MalConfig`), never silently.

### What a tick is — a normal gated turn

A tick is an **ordinary Arbiter turn**, not a privileged one. It posts `TURN_ORIGIN =
heartbeat:<name>` then a `USER_MESSAGE` carrying the entry's prompt, and runs the turn through the
same machinery every front-end uses:

- **Full context.** The agent gets the whole system prompt (soul + core memory + skills roster +
  `PEER.*` folds) and may call **any tool**, including `ask_agent`. `PeerLoopGuard` only blocks
  `peer:*` origins, so a heartbeat turn **can delegate** to a peer.
- **Objectives apply.** `capability_policy`, `avoid_destructive`, and `stay_on_budget` gate a
  self-turn exactly as they gate a human turn.
- **Confirm-band actions are AUTO-DENIED.** There is no human to approve, so any action that would
  raise a confirmation prompt is denied (with the same auto-deny behavior as a peer-driven bridge
  turn). To let a heartbeat do something confirm-band, give it an *allowed* path in
  `capability_policy` (e.g. an `exec_allow` prefix), not a prompt that hopes for approval.
- **Skip-if-busy, never queued.** The tick `try_lock`s the shared **TurnGate**. If a human/peer turn
  already holds it, the tick is **skipped** for that minute (a `HEARTBEAT_SKIPPED` bus post) rather
  than queued — heartbeats never pile up behind a live conversation.
- **Everything is logged.** Every tick (and skip/error) lands in the Eventlog → replay with
  `hades-scope` to see what the agent did unattended.

### The notify flow — `NOTIFY_USER` → Telegram, with a SILENT sentinel

- **`notify = false`** (the default): the reply is **dropped**. The turn still ran and its tool calls
  (a `save_memory`, an `edit_file`, an `http_fetch`) took effect — those side effects are the point.
  Use this for maintenance tasks the user shouldn't be pinged about.
- **`notify = true`**: the trimmed reply is posted to **`NOTIFY_USER`** and forwarded — **unless** it
  is empty or exactly **`SILENT`**. The convention (bake it into the prompt) is: *reply `SILENT` when
  there is nothing worth reporting*. So a monitor that checks a condition every 10 minutes stays quiet
  on a normal check and only messages you when something is actually up.
- **The sink is Telegram (v1).** `TelegramModule` subscribes `NOTIFY_USER` and sends the text to every
  `allow_users` id (§10). **A `notify = true` heartbeat with no `telegram` module rostered posts
  `NOTIFY_USER` but nothing delivers it** — if you want notifications, roster `telegram`.

### The inline-prompt `=` footgun (read this)

The manifest is **one-kv-per-line and fails loud** on ` word = word ` packed on a physical line (§1).
An inline `prompt` whose text contains an `=` (e.g. `prompt = set the counter x = 5`) trips that
detector → `MalConfig`, **the binary refuses to start**. Two ways out, and the second is the rule of
thumb:

- Cron values never contain `=`, so `schedule` is always safe inline.
- **For any prompt that contains an `=`, spans multiple lines, or is more than a short sentence, use
  `prompt_file`** and put the text in a file (see `prompts/daily_summary.txt` for a worked example).
  A file has no parser constraints.

### Bus keys & the turn origin

New signals this module introduces (visible in the Eventlog / `hades-scope`):

| Bus key | Value | When |
|---|---|---|
| `TURN_ORIGIN` | `heartbeat:<name>` | Posted at the start of each self-turn (a new origin value alongside `human` and `peer:<name>`). `PeerLoopGuard` treats it as non-peer, so the turn may call `ask_agent`. |
| `NOTIFY_USER` | `{ text, from }` (`from = heartbeat:<name>`) | A `notify = true` tick whose reply is non-empty and not `SILENT`. The Telegram module (§10) is the sink. |
| `HEARTBEAT_SKIPPED` | the entry name | The tick's minute matched but the TurnGate was held by a live human/peer turn (skip-if-busy). |
| `HEARTBEAT_ERROR` | `<name> …` (the entry name + a reason) | The tick threw, or the self-turn hit its idle timeout. |

### Worked example

A monitor that pings you at most every 10 minutes (only when it finds something), plus a silent daily
summary loaded from a file:

```
Module = heartbeat

Heartbeat = disk_watch
{
  schedule = */10 * * * *
  prompt   = Check free disk on this host with run_command. If any filesystem is over 90 percent full, report which one and the usage. Otherwise reply exactly SILENT.
  notify   = true
}

Heartbeat = daily_summary
{
  schedule    = 0 6 * * *
  prompt_file = prompts/daily_summary.txt
  notify      = false
}
```

The `disk_watch` prompt is a single sentence with no `=`, so inline is fine; it needs `Module =
telegram` rostered for its notifications to arrive. `daily_summary` runs a background task (save a
recap to memory) and, being `notify = false`, never pings the user regardless of what it replies.

### Self-scheduling — the agent creates its own tasks at runtime

The `Heartbeat = <name>` blocks above are **operator-static** (parsed once at launch). Self-scheduling
lets the **agent** create, list, and cancel its own scheduled turns *at runtime* via three tools, so a
goal you set in conversation ("keep an eye on X, ping me if it drifts") can spawn its own monitors and
reminders. Dynamic tasks are persisted in a store the `HeartbeatModule` re-reads on every ~30 s scan and
**coexist** with the static `Heartbeat = <name>` entries.

A scheduled task is a **prompt** (never a raw command): when it fires it drives the same gated self-turn
a static heartbeat does (`TURN_ORIGIN = heartbeat:<name>`, all objectives, confirm auto-denied). To run a
command, the prompt instructs the agent and it calls `run_command` — which passes `capability_policy` —
so scheduled work stays inside the same gate as everything else.

**Opt-in — roster the three tools** (absent → the agent cannot self-schedule):

```
Tool = schedule_task { native = ./build/hades-schedule-task }
Tool = list_tasks    { native = ./build/hades-list-tasks }
Tool = cancel_task   { native = ./build/hades-cancel-task }
```

**Config — the UNNAMED `Heartbeat { }` block** (distinct from the `Heartbeat = <name>` *entry* blocks):

| Key | What it does | Default | Notes |
|---|---|---|---|
| `cron_store` | Path to the dynamic-task store (append-only jsonl). | `.hades/cron.jsonl` | Whitespace in the path → `MalConfig` (it is appended to the tools' whitespace-split argv). The module compacts it on boot. |
| `allow_self_schedule` | May a **heartbeat tick** create new tasks? | `false` | `false` → a heartbeat-origin turn is hard-vetoed from `schedule_task` (recursion contained); a **human**-origin turn may always schedule. `true` → a tick may schedule follow-ups too (escalation). |
| `max_tasks` | Cap on active dynamic tasks. | `20` | `schedule_task` refuses (`ok:false`) when the active count is at the cap. |
| `min_interval_s` | Floor for a one-shot's delay, seconds. | `60` | A one-shot `in_minutes` below this floor is refused. (Recurring cron is minute-resolution + deduped → an inherent 60 s/task floor; `when` watches are exempt — their rate is bounded by `cooldown_s` instead.) |

Rostering `schedule_task` **without** `Module = heartbeat` is a `MalConfig` (nothing would ever run the
tasks). The gate is a `SelfScheduleGuard` objective, auto-registered when heartbeat + `schedule_task` are
both present; it guards only the create path — `list_tasks`/`cancel_task` are always allowed. A
**`peer:`-origin turn is hard-vetoed from `schedule_task` unconditionally** (even with
`allow_self_schedule = true`): a peer must never plant standing work on this agent.

**The tools:**

- **`schedule_task`** — `{ name, prompt, notify?, and exactly ONE of: schedule | in_minutes | at | when }`.
  `schedule` = a 5-field cron (recurring, same subset as above). `in_minutes` = run once, N minutes from
  now. `at` = run once at an absolute machine-local time, `YYYY-MM-DDTHH:MM` (or `HH:MM` = the next
  occurrence). `when` = a reactive watch (the 5 condition forms above; optional `cooldown_s`, default 60).
  Returns the new task's `id`.
- **`list_tasks`** — the agent's own active dynamic tasks (id, name, kind, timing, prompt, notify). Static
  `Heartbeat = <name>` entries are operator-owned and **not** listed.
- **`cancel_task`** — `{ id }` (from `list_tasks`) → removes it.

**Store schema** (`.hades/cron.jsonl`, append-only, folded by id — an operator can read it):

```
{"op":"add","id":"t<epoch>-<hex>","name":"nightly","kind":"cron","schedule":"*/15 * * * *","fire_epoch":null,"prompt":"…","notify":true,"created":1751900000}
{"op":"add","id":"t<epoch>-<hex>","name":"remind","kind":"once","schedule":null,"fire_epoch":1751986800,"prompt":"…","notify":true,"created":1751900050}
{"op":"add","id":"t<epoch>-<hex>","name":"watch","kind":"when","schedule":null,"fire_epoch":null,"when":"PEER.pi0.card changes","cooldown_s":60,"prompt":"…","notify":true,"created":1751900100}
{"op":"cancel","id":"t<epoch>-<hex>"}    # cancel_task
{"op":"done","id":"t<epoch>-<hex>"}      # the module, after a one-shot fires
```

**Gotchas:** the self-schedule config lives in the **unnamed** `Heartbeat { }` block — a `Heartbeat =
<name> { }` block is always a static *entry* (and needs a `schedule`). `at`/`in_minutes` are **machine-local**
(like cron; no timezone key in v1). The store **persists across restarts** — a one-shot whose time passed
while the process was down fires once on the next boot scan (catch-up), then completes. `notify` behaves
exactly as for a static entry (`NOTIFY_USER` → Telegram, `SILENT` sentinel), so a `notify = true` dynamic
task still needs `Module = telegram` to deliver. Delivery is **at-least-once**: the `done` record is written
*after* a one-shot's turn runs, so a crash in that window (or a same-minute process restart for a recurring
task, whose per-minute dedup is in-memory) can re-fire it once — write tasks whose action is idempotent, or
tolerant of a rare repeat. The store is compacted only on boot; between boots it grows append-only (a v2
concern for a very high-frequency self-scheduling agent).

---

## 16. `Simplex` block

`src/apps/simplex/simplex.cpp` (`SimplexModule::on_start`). Requires `Module = simplex`. A fourth
front-end (after `chat`, `serve`, `telegram`): it reads events from a **local `simplex-chat` daemon**
over its unauthenticated loopback WebSocket API and drives whole turns through the shared TurnGate,
exactly like Telegram — lock → post `USER_MESSAGE` → `run_until(reply|confirm)` → send the reply
(split at 4000 chars). There is **no bot token**: the daemon's WS API is unauthenticated by design, so
run it bound to loopback only.

| Key | What it does | Default | Notes |
|---|---|---|---|
| `allow_contacts` | **Comma-separated** list of exact display names (what `/contacts` shows) and/or numeric contact ids allowed to drive the agent. **REQUIRED.** | — (missing/empty → `MalConfig`) | Non-allowed senders are silently dropped. A name is the practical choice and safe with `auto_accept = false`; use ids only if you enable auto-accept (see below). |
| `host` | Host of the local `simplex-chat` daemon's WS API. | `127.0.0.1` | Keep it loopback — the API is unauthenticated. |
| `port` | Port of the daemon's WS API. | `5225` | Match the `simplex-chat -p <port>` you launched. Bad/garbage → `5225`. |
| `auto_accept` | Auto-accept incoming contact requests. | `false` | `false` → accept requests manually in the `simplex-chat` CLI. `true` is an explicit opt-in (name-spoof risk below). |
| `notify_contact` | Contact id-or-name that receives `NOTIFY_USER` messages (heartbeat notifications). | `""` (no delivery) | A numeric id resolves directly; a display name resolves once that contact has been seen in an event (else the notify is skipped with a log line). |
| `connect_timeout_s` | WS connect timeout; also the reconnect backoff base. | `10` | Positive-double; bad/0/garbage → keeps the default. |

**Setup walkthrough.**
1. Install the official **`simplex-chat`** CLI — an **external runtime dependency**, not built by hades.
   Prebuilt release binaries exist for x86_64 and aarch64; the `simplex-chat-ubuntu-2x_04-aarch64` build
   runs on the Pi (Debian) directly. **On NixOS** the CLI is **not in nixpkgs** (only the desktop app), so
   this repo's `flake.nix` ships `packages.x86_64-linux.simplex-chat-cli` (the official release binary,
   autoPatchelf'd) and puts `simplex-chat` on the `nix develop` PATH.
2. First run (`simplex-chat`, then `/q`) creates a chat profile — pick the bot's display name.
3. In the CLI, run `/address` **once** to create the bot's long-term contact address; share it (or the
   generated link) with the humans who will message the bot.
4. Launch the daemon with the WS API on the port you configured: `simplex-chat -p 5225`.
5. Connect from the SimpleX phone/desktop app to the bot's address; **accept the contact** in the CLI
   (`/ac <name>`, or set `auto_accept = true`). Then run **`/contacts`** — it shows the contact's
   **display name** (the terminal CLI does not print the numeric id). Use that exact name in
   `allow_contacts` (a name is the practical path — a numeric id is only obtainable via the WS API
   `/_contacts 1`, and is unnecessary since manual acceptance already makes the name trustworthy).
6. Uncomment/activate the `Simplex` block in your manifest with that name (or a numeric id), then run
   hades. Message the bot → a gated turn replies. **LIVE-VALIDATED 2026-07-11** (phone → hades reply,
   allowlist by display name).

**Gotchas.**
- **No token, loopback only.** The WS API is unauthenticated by design — bind the daemon to `127.0.0.1`.
  Anyone who can reach the port can drive the daemon, so do not expose it on a LAN/public interface.
- **`allow_contacts` is mandatory** (else `MalConfig`). A **display name is the practical choice** — it is
  what `/contacts` shows, and with `auto_accept = false` (default) it is trustworthy because you accepted
  that contact by hand. A name is only spoofable when combined with `auto_accept = true` on an open address
  (a stranger names themselves like an allowlisted contact and is auto-admitted); if you enable auto-accept,
  allowlist by a numeric id instead (obtainable via the WS API `/_contacts 1` — ids are daemon-assigned and
  cannot be spoofed).
- **`auto_accept = false` (default) is the safe choice.** With `auto_accept = true` **and** an open
  address, the name-spoof path above lets a stranger both connect and (if you allowlist by name) drive the
  agent. Keep manual acceptance, or allowlist strictly by numeric id.
- **Confirms are plain text (no inline keyboards).** A confirm-gated action prompts the contact with a
  `y/N` question; the **next message from that same contact** answers it (`y`/`yes` approves, anything
  else denies).
- **Notify sink.** SimplexModule subscribes **`NOTIFY_USER`** and, when `notify_contact` is set, delivers
  each such message to that contact — the delivery path for a `notify = true` `Heartbeat` (§15). Delivery
  is queued and sent by the module's own event thread (one thread owns the daemon socket), so it can lag
  the notification by up to ~25 s (one internal read cycle). With **both** `telegram` and `simplex`
  rostered and each configured with a notify target, a heartbeat notification is delivered on **both**
  surfaces.
- **One outstanding confirm at a time (v1).** The pending-confirm slot is single: if a second allowlisted
  contact messages while a confirm is outstanding, the first contact's confirm can be displaced (the
  agent-side pending confirm survives; re-ask to re-prompt). Single-operator deployments — the intended
  v1 shape — never notice.
- **Reconnect loop.** The event thread reconnects with backoff (base `connect_timeout_s`) if the daemon
  is down or drops the connection, so the bot survives a daemon restart. The dtor stop+joins the thread
  (it can wait up to one internal read deadline for `next_event` to return).
- **Simplex-only roster** (no `chat`/`serve`) → `hades_main` blocks on the event thread (`Ctrl-C` to exit).

---

## 17. `status` module — the REPL stats line

`src/apps/status/status.cpp`. **No config block** — `Module = status` in the roster is the whole
setup (an unknown-key `Status` block would be ignored). A zero-config, threadless aggregator: it
subscribes the traffic every turn already produces and posts one latest-value bus key,
**`AGENT_STATUS`**:

```json
{"ctx_tokens": 12437, "spent_usd": 0.0372, "turn": 9, "model": "gpt-5.5",
 "line": "[ctx 12.4k tok · $0.0372 · turn 9 · gpt-5.5]"}
```

- **`ctx_tokens`** = `prompt_tokens + completion_tokens` of the **last** LLM call — the real size of
  the conversation the model is carrying (not a local estimate). `0` until the first call returns
  usage numbers (e.g. after a failed call).
- **`spent_usd`** mirrors `BUDGET_SPENT_USD` (cumulative for the process, the budget objective's
  view). **`turn`** counts `USER_MESSAGE`s. **`model`** is captured from `LLM_REQUEST`.
- **`/new`** resets `ctx_tokens` and `turn` (they describe the conversation); spend and model carry
  over.
- **Rendering:** the chat REPL prints `line` dim under each assistant reply. Without `Module =
  status` the key is never posted and REPL output is byte-identical to before the module existed.
  The raw fields are the seam for other consumers (a web/telegram `/status` — not built yet); the
  module never touches the terminal itself — the REPL stays the terminal's only writer.

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
