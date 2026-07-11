# SimpleX Chat front-end (design)

**Date:** 2026-07-11 · **Status:** approved (Vaios) · **Branch:** `feat/simplex`

## Problem / goal

A fourth chat surface — private, self-host-native, no phone number, no central identity — matching
Vaios's P2P/self-hosting direction. The agent becomes reachable over SimpleX Chat exactly the way it
is over Telegram: allowlisted humans drive normal gated turns from their phone.

Reference: Hermes-agent's SimpleX integration (local daemon + WS, allowlist, auto-accept flag,
groups-off default). hades v1 mirrors our own Telegram v1 scope, not Hermes's full surface.

## Decisions (brainstorm outcomes)

1. **v1 scope: text DMs only.** Voice notes (Stt/Tts seams are ready), groups, media, message
   batching — all deferred. Telegram-v1 parity.
2. **Allowlist: contact IDs AND display names; `auto_accept = false` by default.** SimpleX has no
   global user ids. Numeric tokens match the local contact id; other tokens match the display name
   exactly. Display names are chosen by the contact, so a name-only allowlist is spoofable ONLY in
   combination with auto-accepted contact requests — with the default manual acceptance (operator
   accepts new contacts in the simplex-chat CLI once) names are trustworthy. `auto_accept = true`
   is an explicit opt-in; the spoof risk is documented next to it. Allowlist REQUIRED — `MalConfig`
   without it (an open bot lets anyone drive the agent).
3. **Transport: in-house minimal WebSocket client** (option A). The daemon's bot API is JSON text
   frames over a localhost WS (`{"corrId","cmd"}` → `{"corrId","resp"}`). We implement the RFC6455
   client subset we need — handshake, client-masked text frames, ping/pong, close, tolerant of
   server fragmentation — over a plain POSIX TCP socket. No TLS, no compression, no proxy (peer is
   the local daemon). Zero new dependencies (lean dep tree; aarch64 static cross unaffected).
   Rejected: third-party WS lib (dependency for a localhost socket), stdio-scraping the terminal UI
   (human-oriented output, not a protocol; WS is the official bot surface).

## Architecture

`SimplexModule` (`type() == "simplex"`, `src/apps/simplex/`) on the TelegramModule pattern:

- **Own thread**, started by `hades_main` (`start()`, AFTER wiring — never in `on_attach`, tests
  spawn no thread), stop+joined in the dtor. `Agent::simplex` is declared LAST (after `telegram`)
  → destroyed FIRST, while the Executor + modules it touches are still alive. A simplex-only roster
  (no chat/serve/telegram) makes `hades_main` block on `wait()`.
- **Shared TurnGate** (injected in `wire_agent` before `on_attach`, like chat/serve/telegram):
  the thread locks the gate, posts `TURN_ORIGIN = human` + `USER_MESSAGE`, `run_until`
  (reply|confirm, the configured idle timeout), sends the reply. Turn-owner guard: captures
  `ASSISTANT_MESSAGE`/`CONFIRM_REQUEST` only while `my_turn_` (telegram precedent).
- **One thread owns the socket entirely.** Events are read between turns; while a turn runs, the
  thread is inside `run_until` and inbound frames queue in the kernel buffer. Server pings are
  answered when the thread returns to the read loop (a turn can hold the socket silent up to the
  idle ceiling; the localhost daemon does not enforce pong deadlines — documented, revisit if a
  live daemon ever drops us mid-turn).
- **corrId request/response:** commands sent with a sequential corrId; the reader consumes frames
  until the matching `resp`, queuing any message events encountered for the main loop.
  Single-threaded → no locking.
- **Reconnect loop:** a failed/killed connection (daemon restart) → retry with backoff
  (log each attempt; no crash). Messages that arrived while disconnected are NOT replayed by the
  daemon — documented v1 behavior (process what arrives post-connect).
- **`SimplexApi` seam** (TelegramApi precedent): the module talks to an interface
  (send text / accept request / next event); the real impl drives the WS client; tests fake it.

## Inbound flow

Direct-message text event → allowlist check (id or exact display name) → gated turn → reply via
the bot-API send command, **split at 4000 chars**. Non-allowlisted senders: silently dropped.
Group messages: dropped (v1 private-chat-only; replies would be group-readable). Contact request
event: `auto_accept=false` → log and ignore (operator accepts in the CLI); `=true` → send the
accept command (messages still allowlist-gated).

**Confirm = text y/N** (SimpleX has no inline keyboards): the confirm prompt is sent as text
("reply y to approve"); while a confirm is pending, the next message from the SAME contact is the
answer — `y`/`yes` (case-insensitive) approves, anything else denies (ChatModule REPL semantics).

**NOTIFY_USER sink:** optional `notify_contact` (one id-or-name, the Hermes "home channel"
analogue) — when set, the module subscribes `NOTIFY_USER` and delivers heartbeat notifications to
that contact. Absent → no delivery. If both telegram and simplex are rostered with notify sinks,
the user gets the notification on both (documented).

## Config (`Simplex` block; `Module = simplex` rosters it)

| key | default | meaning |
|---|---|---|
| `host` | `127.0.0.1` | simplex-chat daemon WS host |
| `port` | `5225` | daemon WS port |
| `allow_contacts` | — **REQUIRED** | **COMMA-separated** (display names contain spaces — exec_allow precedent): numeric local contact ids and/or exact display names |
| `auto_accept` | `false` | auto-accept incoming contact requests (spoof-risk documented) |
| `notify_contact` | unset | NOTIFY_USER delivery target (id or name) |
| `connect_timeout_s` | `10` | TCP+handshake timeout; also the reconnect backoff base |

## Security

- The daemon's WS API is **unauthenticated by design** — anyone who can reach the port fully
  controls the chat identity. `host` defaults to loopback; the daemon itself binds localhost-only.
  Pointing `host` at a remote daemon is allowed but documented as plaintext + unauthenticated
  (don't — run the daemon next to hades).
- No tokens/secrets — nothing to redact in session.log.
- Allowlist REQUIRED; name-spoof risk exists only with `auto_accept = true` (documented there).
- A SimpleX turn is a normal gated turn: objectives, capability policy, confirm-band all apply.

## Exact daemon protocol shapes

The WS bot API command strings and event JSON paths (send command form, `newChatItems` /
contact-request / connected event tags, contact id/name field paths) are pinned at PLAN time from
`bots/api/COMMANDS.md` in the simplex-chat repo, encoded in the `SimplexApi` impl only, and
covered by canned-JSON tests. The spec deliberately does not freeze strings the upstream docs own.

## Deploy

Desktop + Pi: install the official `simplex-chat` CLI binary (aarch64 builds exist:
`simplex-chat-ubuntu-2x_04-aarch64` runs on Pi OS bookworm), run `simplex-chat -p 5225` as a
service, create the bot profile + address (`/address`) once in the CLI. External runtime dep —
himalaya/whisper pattern; nothing bundled in the static build. Pi Zero 2 W (512 MB) running the
Haskell daemon = live validation by Vaios.

## Non-goals (v1)

Voice notes (Stt/Tts later) · groups · media/files in or out · message batching windows ·
bot-address management from hades · missed-message replay · WS TLS/proxy/compression ·
multi-daemon.

## Testing

- `ws_frame` codec: pure byte-level tests (handshake key/accept, mask, frame build/parse,
  fragmented + interleaved-ping streams, oversize guard).
- WS client vs a **fake localhost server** on an ephemeral port (bridge/serve test pattern):
  handshake, echo, corrId matching, event-while-waiting queuing, server-close → reconnect.
- Module vs **fake `SimplexApi`**: allowlist (id, name, deny), confirm y/N round-trip, turn-owner
  guard, group/dropped events, auto_accept on/off, notify_contact delivery, 4000-char split.
- Wiring: roster factory, `Simplex` block resolution, `MalConfig` without `allow_contacts`,
  TurnGate injection, simplex-only `wait()` roster.
- Full suite + TSan (new thread) gate every task; live smoke vs a real daemon = Vaios.
