# web_search native tool — design

**Date:** 2026-07-18
**Status:** approved (Vaios)
**Motivation:** item 2 of the 2026-07-18 CC tool-gap analysis (CLAUDE.md): the agent cannot
discover anything without already knowing a URL. `web_search` finds pages; the just-shipped
`http_fetch` HTML extraction reads them — the two compose. Native static tool (no MCP
server) keeps the zero-dep Pi deploy story.

## Decision summary

One **generic HTTP search engine** in a single tool binary; **named presets** fill its
knobs. v1 presets: **`searxng`** (default emphasis — Vaios will self-host an instance on
loopback/LAN) and **`brave`**; a raw **`http`** provider exposes every knob so Brave-like
hosted APIs (Kagi, Serper, Exa, …) work with zero future C++. Config lives in a `Search`
manifest block, resolved by wiring into flat `k=v` argv pairs (operator-pinned — the LLM
can never redirect the endpoint). New `Capability::WebSearch` → **allow**.

## Tool contract

`web_search { query, max_results? }`:

- `query` — required string; empty/non-string → `ok:false` (fail closed).
- `max_results` — optional number, default **5**, clamped **1..10** (snippets burn tokens).
  Non-number / non-positive → treated as absent (house rule).
- Result: `{ ok, result: { results: [ { title, url, snippet }, … ], provider } }` —
  `provider` echoes the resolved provider name so the agent can say where hits came from.
  Zero hits → `ok:true` with an empty `results` array (a search that found nothing is not
  an error).
- Describe text (the LLM's API surface): "Search the web and return ranked results
  (title, url, snippet). Follow up promising results with http_fetch to read the page."
  Schema: `query` (string, required), `max_results` (number).
- Truncation to `max_results` is **client-side** in the tool (providers disagree on count
  params; SearXNG's JSON API has none). Each snippet is byte-capped at 500 with the
  UTF-8 boundary respected via the reply's replace-dump (house pattern).

## Provider model — one engine, presets fill knobs

The engine (in the tool binary) executes: build URL/body from config + query → cpr
GET/POST with optional auth header → parse JSON → walk `results_path` (nlohmann JSON
pointer) → map each entry through `title_key`/`url_key`/`snippet_key` → truncate.

**Knobs** (= the flat resolved config):

| knob | meaning | default |
|---|---|---|
| `provider` | `searxng` \| `brave` \| `http` | REQUIRED |
| `endpoint` | base URL (house gotcha family: BASE, path appended per preset) | preset or REQUIRED |
| `api_key_env` | env var NAME holding the key; empty = no auth | preset |
| `method` | `get` \| `post` | `get` |
| `query_param` | URL param (get) or JSON body key (post) for the query | `q` |
| `auth_header` | header carrying the key | `Authorization` |
| `auth_scheme` | `bearer` (prefix `Bearer `) \| `none` (raw key) | `bearer` |
| `results_path` | JSON pointer to the results array | `/results` |
| `title_key` / `url_key` / `snippet_key` | per-result field names | `title` / `url` / `snippet` |
| `extra_query` | one literal `k=v` appended to a GET query string | empty |
| `max_results` | default result count | `5` |
| `timeout_s` | per-request HTTP timeout | `15` |

**Presets** (a table, not code paths; an explicit manifest key beats the preset value):

- `searxng`: path `/search`, `query_param = q`, `extra_query = format=json`,
  `results_path = /results`, `snippet_key = content`, no auth. `endpoint` REQUIRED
  (instance-specific, e.g. `http://127.0.0.1:8888`).
- `brave`: `endpoint = https://api.search.brave.com/res/v1/web/search` (path already in
  the endpoint; no extra path appended), `auth_header = X-Subscription-Token`,
  `auth_scheme = none`, `results_path = /web/results`, `snippet_key = description`,
  `api_key_env = HADES_SEARCH_KEY`.
- `http`: raw knobs, nothing pre-filled beyond the table defaults; `endpoint` REQUIRED.

Preset path handling: `searxng` appends `/search` to the endpoint; `brave` and `http` use
the endpoint as the full request URL. POST body is `{ <query_param>: <query> }`.

## Manifest + wiring

```
Tool = web_search { native = ./build/hades-web-search }

Search
{
  provider = searxng
  endpoint = http://127.0.0.1:8888
}
```

Wiring (`wire_agent`) resolves the `Search` block + preset into the flat config and
appends it to the tool argv as **`k=v` pairs** (each value `reject_ws` — argv is
whitespace-split; `auth_scheme` is an enum so no value ever needs a space). The tool
parses argv `k=v`; unknown keys are ignored (forward-compat). **The API key is NEVER in
argv** (argv is world-readable via /proc): argv carries only the env-var NAME; the tool
`getenv`s it. Boot-time `MalConfig` (fail loud):

- `web_search` rostered with no `Search` block;
- resolved config missing `endpoint` (searxng/http);
- `api_key_env` non-empty but the env var unset/empty at launch (resolve_api_key
  precedent) — a keyless custom endpoint sets `api_key_env =` empty explicitly;
- any config value containing whitespace.

No `Tool = web_search` line → block ignored, feature absent (opt-in; dev.hades ships the
Tool line + `Search` block COMMENTED with the loopback SearXNG example).

## Capability / security

- New **`Capability::WebSearch`** in `capability_of("web_search")` → **allow**. Rationale:
  the endpoint is operator-pinned via argv (the `mcp_url` precedent — operator-set
  endpoints are exempt from the private-net gate, which is what makes a loopback/LAN
  SearXNG instance work); the LLM supplies only query text, so there is no SSRF surface
  in the tool's arguments.
- **Peer/heartbeat caveat (documented, session_search pattern):** a `peer:`-origin or
  heartbeat turn can trigger searches unattended. Queries flow only to the
  operator-chosen backend; the exposure is query-text exfiltration to that backend, which
  the operator already trusts. Per-origin tool scopes (capability v2) is the real fix.
- The tool follows cpr defaults for redirects (search APIs legitimately 3xx); the
  endpoint is operator-trusted, unlike http_fetch's LLM-chosen URLs — so redirect-SSRF
  reasoning does not apply here. State this in the code comment.
- Secrets: key via env only; never logged; the existing `session.log` redaction covers
  values of known secret envs — `HADES_SEARCH_KEY` is added to `hades_main`'s redaction
  list.

## Error handling (fail soft in-tool)

- HTTP status != 200 → `ok:false`, `{error, status}` (a 403 from SearXNG usually means
  its JSON API is disabled — the error text says so).
- Unparseable JSON / `results_path` missing or not an array → `ok:false` with a short
  error naming the path.
- Per-result entries missing `title_key`/`url_key` → skipped; missing snippet → empty
  string.
- Network failure/timeout → `ok:false` (cpr status 0).
- Reply serialized with the UTF-8-replace dump (snippet byte-caps can split codepoints).

## Files

- `include/hades/search/search_config.h` — header-only: `SearchConfig` struct, preset
  table, `resolve_search_config(Block)` (returns config or throws `MalConfig`),
  `to_argv_kv(config)` / `parse_argv_kv(argv)` — shared by wiring, tool, tests without a
  core link.
- `tools/web_search_main.cpp` — the engine (cpr + nlohmann, links like http_fetch).
- `src/behaviors/capability_policy.cpp` + header — `WebSearch` enum + row + allow.
- `app/agent_wiring.cpp` — `Search` block extraction, resolve, reject_ws, env-presence
  check, argv append; `app/hades_main.cpp` — redaction list + `HADES_SEARCH_KEY`.
- Tests: `tests/test_search_config.cpp` (preset resolution, overrides, MalConfig cases,
  argv round-trip); `tests/test_web_search_tool.cpp` (loopback `httplib::Server` faking a
  SearXNG shape and a Brave shape: happy path, max_results clamp, zero hits, 403, garbage
  JSON, missing path, no-auth vs auth-header assertion); `tests/test_capability_policy.cpp`
  (+row); wiring test for argv assembly + the MalConfig gates.
- Docs: `docs/manifest-reference.md` — new `Search` block section, §2/§4 rows (argv-append
  table), §5 capability row; `manifests/dev.hades` commented block; `.env.example`
  `HADES_SEARCH_KEY`; CLAUDE.md gap-list item 2 → shipped.

## Non-goals (recorded, not built)

DuckDuckGo scraping (fragile, ToS) · result pagination · bundled fetch-of-top-hit ·
query caching/rate limiting · per-request provider selection (one provider per manifest) ·
POST-body auth (Tavily-style key-in-body; use their header mode or v2).

## Test plan

Unit: preset/override/argv round-trip + MalConfig gates. Tool-level: loopback httplib
servers shaped as SearXNG (`/search?q=…&format=json` → `{results:[{title,url,content}]}`)
and Brave (`/web/results`, asserts the `X-Subscription-Token` header arrived) — no real
network. Live smoke (Vaios): SearXNG instance up → "search the web for X" → hits →
follow-up `http_fetch` reads a result page.
