# hades web UI — design spec

**Date:** 2026-06-29
**Status:** approved (brainstorm via Q&A) → ready for plan

## Goal

Add a browser front-end for the agent, reusing the existing HTTP server (`--serve`). Ship it as
**static files on disk** (editable without recompiling), **LAN-reachable** (configurable bind host, no
auth — the user's own private networking secures it), and **structured so auth and a future settings UI
(for editing `manifests/`) drop in cleanly**.

The HTTP JSON API already exists: `POST /chat {message}` → `{reply}` or `{needs_confirm,id,prompt}`,
`POST /confirm {id,approved}`, `GET /health`. The web UI is a page that speaks that API; the C++ change
is mounting a static directory + a couple of config/seam additions. No Arbiter/core change.

## Components

### 1. `ServeConfig` resolution — `include/hades/serve_config.h`, `src/config/serve_config.cpp`

```cpp
struct ServeConfig { std::string host; int port; std::string webroot; };
ServeConfig resolve_serve_config(const Manifest& m, int cli_port);  // cli_port<=0 => not given
```

Reads an optional `Serve { host, port, webroot }` block. Resolution + defaults:
- `host`    = `Serve.host`    or `"127.0.0.1"` (loopback). Set `0.0.0.0` for LAN.
- `port`    = `cli_port` (if > 0, the `--serve <port>` override) else `Serve.port` (if a valid positive int) else `8080`.
- `webroot` = `Serve.webroot` or `"web"` (directory served as `/`).

Pure, never throws (bad values fall back to defaults); fully unit-testable. This struct is the thing a
future settings UI would read/write.

### 2. Static web assets — `web/index.html`, `web/style.css`, `web/app.js`

Self-contained (no external CDN — works offline on loopback/LAN).

- **`index.html`** — title bar (`hades ● live`), a scrollable message list, an input row (text field +
  Send). Links `style.css` + `app.js`.
- **`style.css`** — dark, monospace (terminal vibe). `user>` label bold cyan, `assistant>` bold green,
  `confirm` amber. Replies rendered with `white-space: pre-wrap` (code/newlines readable; no markdown
  lib needed → stays offline).
- **`app.js`** — chat logic:
  - Send (button or Enter) → `POST /chat {message}` → append a `user>` line + render the result.
  - If result is `{reply}` → append an `assistant>` block.
  - If result is `{needs_confirm,id,prompt}` → append a `confirm` block with **Approve** / **Deny**
    buttons → `POST /confirm {id,approved}` → render the resulting `{reply}` (or another confirm).
  - Auto-scroll to the latest message. A **Clear** button resets the local view (server history is
    untouched). Network/JSON errors append a visible error line.

### 3. HTTP server: static mount + auth seam — `src/module/http_server_module.{h,cpp}`

- `listen(const std::string& host, int port, const std::string& webroot)` — **adds the `webroot`
  parameter**. Mounts the dir as the site root: `srv.set_mount_point("/", webroot)` (so `/` serves
  `index.html`, plus `style.css`/`app.js`/any future page). Keeps `POST /chat`, `POST /confirm`,
  `GET /health` exactly as they are (the JSON API the page calls).
- **Auth seam (groundwork, no-op today):** a `set_pre_routing_handler` calls a file-local
  `authorize(const httplib::Request&)` that currently `return true;` (everything allowed). Adding auth
  later = implement `authorize` (check a token/header) in that one place; the 401 short-circuit is
  already wired. `httplib` stays confined to the `.cpp` (not pulled into the header).

### 4. Binary wiring — `app/hades_main.cpp`, `manifests/dev.hades`

- `--serve` no longer requires a port: presence enables serving; an optional following integer is the
  port override. Resolve `ServeConfig` via `resolve_serve_config(manifest, cli_port)` and call
  `agent.serve->listen(cfg.host, cfg.port, cfg.webroot)`. Update the usage string.
- `manifests/dev.hades` gains an optional block:
  ```
  Serve { host = 127.0.0.1  port = 8080  webroot = web }
  ```
  (loopback by default; the user flips `host = 0.0.0.0` for LAN.)

## Deferred (groundwork only — structure ready, not built)

- **Auth implementation** — the `authorize()` seam exists; filling it in is a later feature.
- **Settings UI** — a future `web/settings.html` + `GET/POST /manifest` endpoints to view/edit the
  manifest. The static-dir + JSON-API pattern is the template; writing/validating/reloading a manifest
  is its own feature with real risk (kept out of scope).
- **SSE/WebSocket streaming** — replies still arrive whole when `/chat` returns. Live token streaming is
  a separate change to the provider + Arbiter + an SSE endpoint.
- Markdown rendering, server-side history reload, timestamps, multi-session.

## Security / safety

- Default bind is **loopback** (`127.0.0.1`); LAN requires an explicit `host = 0.0.0.0`. **No auth by
  design** for now (user-supplied private networking) — but the bind default is the safe one, and the
  `authorize()` seam is the single chokepoint for adding it. `webroot` only exposes the configured
  static dir (httplib serves files under the mount point; no traversal above it).
- The existing single-mutex serialization of `/chat`/`/confirm` is unchanged (single-threaded bus).

## Testing (TDD, GoogleTest)

- `test_serve_config` — `resolve_serve_config`: defaults when no `Serve` block; `Serve` values honored;
  `cli_port > 0` overrides the block; invalid `port` falls back to `8080`; `host`/`webroot` defaults.
- `test_webui` (new) — read the on-disk `web/index.html` (path via a CMake compile-def) and assert it
  wires the API: contains `/chat`, `/confirm`, `needs_confirm`, and the Approve/Deny controls; assert
  `web/style.css` and `web/app.js` exist and are non-empty.
- Existing `test_serve` (socket-free `handle_message`/`handle_confirm`) stays green — the JSON API is
  unchanged.

## MOOS-IvP mapping

The web UI is not a new module — it rides the existing **HttpServerModule** (a front-end Module on the
Blackboard, the `--serve` mode). Same pattern as ChatModule (stdin) and a future TelegramModule: a
front-end that posts `USER_MESSAGE` and renders `ASSISTANT_MESSAGE`. Static-file serving + the config
block is the seam that makes the browser surface (and later a settings surface) cheap to grow.
