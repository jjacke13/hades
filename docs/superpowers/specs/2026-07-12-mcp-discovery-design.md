# MCP tool discovery + remote (HTTP) transport — design

**Date:** 2026-07-12
**Status:** approved (Vaios, 2026-07-12 — "stdio + http")
**Origin:** backlog item "MCP tool discovery (MCP servers can be called but aren't announced to
the LLM)". Extended during brainstorm to include a native remote transport so the agent (and
the Pi, which has no Node) can use hosted MCP servers without the `mcp-remote` bridge.

## Current state (verified in code)

- `Tool = <name> { mcp = <command> }` → `ToolEntry{kind:"mcp"}`. `ToolRegistry::ensure_warm`
  SKIPS mcp entries when building specs (`// MCP tool discovery is deferred (MVP)`), routing
  only the block name → entry. So MCP tools never reach the LLM's tool list; a TOOL_REQUEST
  naming the block works only if the server's tool happens to share the block's name.
- `mcp_call()` (in `src/apps/tool_runner/tool_runner.cpp`) is a ONE-SHOT stdio client: spawns
  the command, sends `initialize` + `notifications/initialized` + one `tools/call` as
  newline-delimited JSON-RPC, scans stdout for the id-2 reply. No persistent child.
- Specs reach the LLM via `wire_agent` (`app/agent_wiring.cpp:350`):
  `a.arbiter->set_tools(a.tools->registry().specs())`. Anything added to `specs_` at warm time
  is announced automatically — zero Arbiter/LLM changes needed.
- There are NO existing tests covering `mcp_call` (grep: no `mcp` in tests/). This feature adds
  transport tests for both transports.

## Goal

1. **Discovery:** at registry warm time, ask each configured MCP server for its tools
   (`tools/list`) and announce them to the LLM like native tools.
2. **Remote transport:** a second, native transport — MCP Streamable HTTP with Bearer auth —
   so remote servers work without Node/`mcp-remote` (Pi story). OAuth-only servers remain on
   the stdio path via the documented `npx -y mcp-remote <url>` bridge.

## Decisions (from brainstorm)

- **One `Tool` block = one MCP SERVER** (not one tool). Discovered tools are announced as
  **`<block>__<toolname>`** (double underscore — the Claude Code `mcp__server__tool`
  convention; `.` is illegal in OpenAI-compat function names). Prefixing GUARANTEES a server
  can never shadow a native tool name (`fs_read`, `shell`, …) and capture its capability
  verdict. Rejected: raw MCP names + collision checks (spoof surface, more code).
- **Capability:** default **confirm**, with an operator allowlist shipped NOW (Vaios):
  `capability_policy` gains **`mcp_allow`** (see below). Rejected: blanket allow (one
  compromised npx package = unattended arbitrary actions on heartbeat/peer turns).
- **Per-call one-shot spawn/exchange KEPT** (v1): discovery is one extra exchange per server at
  boot. v2 seam: warm persistent child (the embedding `persistent_child` precedent) if per-call
  startup (e.g. npx) hurts.
- **Transports v1 = stdio + Streamable HTTP (Bearer only).** OAuth = bridge escape hatch.

## Manifest surface

```
# local (stdio) server — command spawned per exchange
Tool = weather { mcp = npx -y @h1deya/mcp-server-weather }

# remote (Streamable HTTP) server — Bearer token via env, never in the manifest
Tool = linear
{
  mcp_url     = https://mcp.linear.example/mcp
  api_key_env = LINEAR_MCP_KEY
  timeout_s   = 60
}
```

- Exactly ONE of `native | mcp | mcp_url` per Tool block → otherwise `MalConfig` at launch
  (today a block with `native`+`mcp` silently prefers `native` — this tightens it).
- `api_key_env` is only meaningful with `mcp_url` (ignored otherwise); resolved from the
  environment at exchange time; absent/empty env → no Authorization header (public or
  self-hosted servers). Redacted like every other secret (never logged).
- **Block-name gate (prefix integrity):** an `mcp`/`mcp_url` block name must match
  `[A-Za-z0-9_-]{1,64}` and must NOT contain `__` → else `MalConfig` at launch (validated in
  wiring, where Tool blocks pass through). Native block names unaffected.
- `timeout_s` covers BOTH the discovery exchange and each call (existing per-block override
  semantics; 0 → ToolRunner default).

## Architecture

### Transport seam

`ToolEntry.kind` gains `"mcp_http"` (set when `mcp_url` present; `command` holds the URL).
`ToolEntry` gains `std::string api_key_env`. The MCP section of
`src/apps/tool_runner/tool_runner.cpp` (or a sibling TU if it outgrows the file — plan's call)
exposes two operations over both transports:

- `mcp_list(entry, timeout_s)` → `{tools:[{name,description,inputSchema}]}` or `{error}`
- `mcp_call(entry, real_tool_name, args, timeout_s)` → result or `{error}` (existing function,
  re-signatured to take the entry so it can pick the transport)

**stdio exchange** (existing shape): spawn `command`, write `initialize` +
`notifications/initialized` + the request (`tools/list` id 2 or `tools/call` id 2) as
newline-delimited JSON-RPC, scan stdout for the id-2 reply. Never throws; timeout →
`{error}`.

**HTTP exchange** (new, libcpr): per exchange —
1. POST `initialize` to `mcp_url`; headers `Content-Type: application/json`,
   `Accept: application/json, text/event-stream`, plus `Authorization: Bearer <env>` when
   `api_key_env` resolves non-empty. Capture the `Mcp-Session-Id` response header if present.
2. POST `notifications/initialized` (echo session header). Any 2xx accepted (spec says 202).
3. POST the request (`tools/list` / `tools/call`, id 2; echo session header).
4. Best-effort `DELETE mcp_url` with the session header (spec-recommended session teardown);
   failures ignored.
- **Response parsing (both steps 1 and 3):** if `Content-Type` is `text/event-stream`, scan
  `data:` lines for the JSON-RPC object whose `id` matches (servers MAY answer any POST as a
  one-off SSE stream); else parse the body as JSON directly. Malformed/absent → `{error}`,
  never throws.
- Redirects OFF (http_fetch precedent). TLS via libcpr/curl. HTTP status ≥ 400 → `{error}`
  with the status (401/403 surface as-is so the operator sees an auth problem).
- **No SSRF/private-net gate on `mcp_url`:** it is OPERATOR-set in the manifest, not
  LLM-chosen — a loopback self-hosted server is a legitimate target. The private-net gate
  stays on `http_fetch`, where the LLM picks URLs. (Documented in manifest-reference.)

### Discovery (registry warm)

In `ToolRegistry::ensure_warm`, for `kind == "mcp"` or `"mcp_http"`:

1. `mcp_list(entry, timeout)` (timeout = the entry's `timeout_s` or the warm default).
2. On success, for each tool `t` in `result.tools`: push
   `ToolSpec{ block + "__" + t.name, t.description, t.inputSchema }` (MCP `inputSchema` IS
   JSON Schema — 1:1 into the existing spec shape) and map
   `by_tool_name_[prefixed] = &entry` plus `mcp_real_names_[prefixed] = t.name`.
   Tools with empty/invalid names are skipped. Descriptions are used verbatim.
3. **Fail-soft:** timeout / `{error}` / zero tools → keep TODAY'S behavior for that entry
   (`by_tool_name_[block] = &entry`, no specs) — a down server degrades to the current
   call-by-block-name path with a bounded boot delay (stdio: one `timeout_s`; HTTP: worst
   case ~3× — the initialize/request/teardown POSTs are each bounded). A stderr log line
   notes the failed discovery.

`specs()` flows into `wire_agent:350` unchanged → the LLM sees `weather__get_alerts` etc. in
its tool list, schema included. `find_by_tool_name` resolves prefixed names via the map (the
existing mechanism — no string-splitting anywhere).

### Call path (ToolRunner)

`kind != "native"` → `real = reg_.mcp_real_name(name)` (falls back to `name` itself — the
legacy block-name path) → `mcp_call(entry, real, args, timeout)`. `ok` = result object without
`error` key (unchanged).

### Capability: `Capability::McpTool` + `mcp_allow`

- `capability_of(tool)`: a name **containing `__`** → `Capability::McpTool`. Sound because
  native tool names use single underscores only and the block-name gate forbids `__` inside
  block names, so `__` appears exactly at our prefix boundary and only for MCP tools.
- `CapabilityPolicy::veto` for `McpTool`: the prefixed name is in the **`mcp_allow`** scope →
  **allow**; else → **confirm** ("mcp tool out of allow scope — requires confirmation").
- `mcp_allow` (capability_policy block key): **whitespace-separated** list of prefixed names
  (`weather__get_forecast linear__search_issues`) — names are `[A-Za-z0-9_-]`+`__`, never
  contain spaces. The single literal **`*`** allows ALL discovered MCP tools (operator opts
  into trusting every rostered server; documented sharp edge).
- Confirm default means: human approves each MCP call in chat; **heartbeat/peer turns
  AUTO-DENY** (existing behavior for the confirm band) — no unattended foreign-server
  execution unless explicitly `mcp_allow`ed.
- No `capability_policy` objective rostered → ungated (identical to every other tool today).

## What does NOT change

- Arbiter/LLMModule/front-ends: zero changes (specs flow through the existing `set_tools`).
- Native tool path, staleness guard, SkillsModule: untouched.
- Legacy behavior for an mcp block whose discovery fails = exactly today's behavior.

## Files (expected)

| file | change |
|---|---|
| `include/hades/tool/registry.h` | `ToolEntry.api_key_env`; kind `"mcp_http"`; `mcp_real_name()` accessor; `mcp_real_names_` map |
| `include/hades/tool/mcp_adapter.h` | re-signature: `mcp_list`/`mcp_call` over `ToolEntry` (transport-agnostic) |
| `src/apps/tool_runner/tool_runner.cpp` | discovery in `ensure_warm`; call-path real-name resolve; stdio `mcp_list`; HTTP exchange (possibly split to `src/apps/tool_runner/mcp_http.cpp` if >~400 lines — plan decides) |
| `src/behaviors/capability_policy.cpp` (+ header) | `Capability::McpTool`; `__`-detect in `capability_of`; `mcp_allow` scope parse + verdict |
| `app/agent_wiring.cpp` | Tool-block validation: exactly-one-of native/mcp/mcp_url, block-name gate for mcp kinds; pass `api_key_env` through |
| `tests/` | new `test_mcp_adapter.cpp` (stdio via scripted fake-server shell script; HTTP via in-test httplib server incl. SSE-framed reply, session header echo, Bearer header assert, 401 surface) · registry discovery tests (specs content, prefix routing, real-name map, fail-soft) · capability tests (McpTool detect, mcp_allow exact/`*`/absent, heartbeat auto-deny falls out) · wiring MalConfig tests |
| `manifests/dev.hades` | commented example blocks (stdio + http) |
| `docs/manifest-reference.md` | §4 rewrite of the mcp row + new transport table + `mcp_allow` in the capability_policy section + bridge note for OAuth servers |
| `CLAUDE.md` | feature section; mark backlog item done |

## Security summary

- Prefixing removes the name-shadow/spoof surface entirely; `capability_of` stays
  name-table-driven with one structural rule (`__` ⇒ McpTool).
- Default confirm + heartbeat/peer auto-deny: a rostered server's tools cannot run unattended
  until the operator lists them in `mcp_allow` (or `*`).
- `api_key_env`: env-only (house rule), Bearer header, never logged/echoed.
- `mcp_url` is operator trust, deliberately exempt from the private-net gate (self-hosted
  loopback servers are the norm for this user); `http_fetch`'s SSRF gate is unaffected.
- Tool descriptions/schemas from a server are attacker-controlled text entering the system
  prompt/tool list (prompt-injection surface). v1 accepts this (same trust as running the
  server at all) — noted for the record; v2 could length-cap/sanitize descriptions.

## Testing

Baseline **626/626** green before work. New tests as in the Files table — both transports
covered without real network/Node: stdio fake server = a small shell script emitting canned
JSON-RPC lines; HTTP fake = in-test `httplib::Server` (already a dep, precedent in bridge
tests). TSan: not expected to be needed (all new code runs on existing threads: warm happens
in ToolRunner::on_start / lazily, calls in the TOOL_REQUEST handler) — run it once at the end
anyway since the HTTP exchange adds libcpr calls from the pump thread (same as today's
mcp/native path).

Live smoke (Vaios, later): uncomment a stdio example (any npx MCP server) → boot → tools
appear in the LLM's list (ask "what tools do you have?") → call one (confirm prompt fires) →
add to `mcp_allow` → call runs without confirm. HTTP: same against a hosted/self-hosted
Streamable HTTP server with a Bearer key.
