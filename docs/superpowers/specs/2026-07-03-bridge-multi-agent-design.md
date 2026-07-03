# Bridge: multi-agent operation + agent↔agent communication — design

**Date:** 2026-07-03
**Status:** Approved (Vaios, brainstorm session 2026-07-03)
**Branch:** `feat/bridge`

## Problem

hades runs one agent per process (1 agent = 1 MOOS community: Blackboard + Arbiter + modules).
We want **multiple agents that cooperate**: distributed across machines (home server + laptop +
RPi), with **different capability scopes** (a sandboxed worker with shell/write; a front agent
with none) and **specialist personas**. Primary use cases (Vaios): isolation/safety split +
distributed deployment, with specialists among them.

The MOOS answer, recorded in CLAUDE.md since the architecture pass: replicate the community and
bridge them **pShare/pMOOSBridge-style**. This spec un-parks that Bridge.

## Decisions (from the brainstorm)

| Question | Decision |
|---|---|
| Topology | **N processes, one manifest each, network bridge.** In-process Community×N rejected — cannot span machines. |
| What crosses | **Both from day one:** natural-language delegation (ask/reply) AND pShare-style key forwarding. |
| Transport | **Dedicated Bridge listener** — own httplib port, separate from `--serve` (human-facing API with CSRF stays distinct from agent-facing API). |
| Peer confirm gate | **Auto-deny.** A confirm-band action in a peer-driven turn is denied with an explanatory reply. A worker's risky powers come from its OWN manifest scopes (allow-band), set at deploy. Propagation to the asker's human = v2. |
| Architecture | **Approach B — split:** inbound `BridgeModule` (front-end pattern) + outbound `ask_agent` native tool (rides the existing tool/capability/Eventlog infra). One fat module rejected: outbound-ask-as-module bypasses the "everything the LLM does is a tool through the gate" invariant. |

## MOOS-IvP mapping

| MOOS-IvP | hades |
|---|---|
| community | agent process (manifest = mission file) |
| pShare listener | `BridgeModule` (inbound `/ask` + `/share`) |
| pShare forward config | `share_out = KEY…` + outbound push on change |
| variable rename on arrival | inbound share stored as `PEER.<name>.<KEY>` (fixed rename rule v1; per-key rename = v2 seam) |
| new comms app | delegation = `ask_agent` native tool |

Core invariants preserved: modules talk ONLY via the Blackboard; everything the LLM does is a
tool through the gate; the front-end pattern (lock TurnGate → post → run_until → reply) is
reused verbatim for peer turns.

## Architecture

```
Machine 1 (front)                     Machine 2 (worker1, headless RPi)
┌─────────────────────────┐           ┌─────────────────────────┐
│ agent "front"           │           │ agent "worker1"         │
│  Blackboard + Arbiter   │           │  Blackboard + Arbiter   │
│  REPL/Telegram/serve    │           │  (no human front-end)   │
│  ask_agent TOOL ────────┼──HTTP────▶│  BridgeModule /ask      │
│  BridgeModule /ask ◀────┼───────────┼─ ask_agent TOOL         │
│  BridgeModule /share ◀──┼──HTTP────▶│  BridgeModule /share    │
└─────────────────────────┘           └─────────────────────────┘
```

Components:

1. **`BridgeModule`** (`type()=="bridge"`) — inbound listener + outbound share push.
2. **`ask_agent`** — native subprocess tool binary (`hades-ask-agent`), outbound delegation.
3. **`PeerLoopGuard`** — built-in objective auto-registered by wiring whenever the bridge
   module is present (not manifest-optional; the bridge brings its own safety behavior).
4. Manifest: `Bridge { … }` block + `Peer = <name> { url = … }` blocks + the ask_agent Tool line.

## Wire protocol (versioned, additive-extensible)

Auth on EVERY request: header `X-Hades-Bridge: <secret>` where the secret comes from the
`secret_env` environment variable (default `HADES_BRIDGE_SECRET`) — fleet-wide shared secret in
v1; per-peer secrets are a v2 Peer-block key. The secret is NEVER in the manifest and is
redacted in the Eventlog (telegram-token pattern). Bad secret OR `from` not in the Peer
allowlist → **403, silent drop** (no info leak).

```
POST /ask    {"v":1, "from":"front", "hops":0, "message":"..."}
             → 200 {"ok":true, "reply":"..."} | {"ok":false, "error":"..."}

POST /share  {"v":1, "from":"front", "key":"STATUS", "value":<json>}
             → 200 {"ok":true}

GET  /health → 200 {"name":"worker1", "v":1}       (liveness/debug; auth required too)
```

Protocol rules:
- Unknown JSON fields are ignored (forward compatibility). `v` major mismatch → reject.
- `hops >= max_hops` (default 1) → reject at the HTTP layer (belt-and-braces beside the
  PeerLoopGuard; the field is the v2 multi-hop seam).
- Share payload size cap **64 KB** → reject oversized.
- Malformed JSON → `{ok:false}`; the listener never crashes on input.

## Inbound: BridgeModule

Telegram-shaped module. Own httplib listener thread on `Bridge { host, port }` (host default
`127.0.0.1`; set `0.0.0.0` for LAN). Thread started explicitly by `hades_main` after wiring
(never in `on_attach` — tests spawn no thread), stop+joined in the dtor.

**`/ask` flow:** validate (secret, allowlist, version, hops, size) → lock the shared
**TurnGate** (serializes vs REPL/serve/telegram — proven model) → post
`TURN_ORIGIN = "peer:front"` then `USER_MESSAGE = "(from peer agent \"front\") <message>"` →
`run_until(reply)` → the HTTP response carries the `ASSISTANT_MESSAGE`. `my_turn_` turn-owner
guard + RAII reset, verbatim Telegram pattern. The message prefix means zero Arbiter change;
the LLM knows who is asking (plus a soul.md paragraph explaining peers).

**Confirm auto-deny:** during a bridge-owned turn, `CONFIRM_REQUEST` → the module immediately
posts `CONFIRM_RESPONSE{denied, reason:"peer-driven turn — needs human confirmation"}`. The LLM
sees the denial and explains it in the reply. No headless deadlock.

**`TURN_ORIGIN` convention:** every front-end posts `TURN_ORIGIN` immediately before its
`USER_MESSAGE` — chat/serve/telegram post `"human"`, the bridge posts `"peer:<name>"` (one line
each). Latest-value key; consumed by PeerLoopGuard.

**Loop guard (the A↔B deadlock):** `PeerLoopGuard : Objective` hard-vetoes an `ask_agent` tool
call when `TURN_ORIGIN` starts with `peer:` — a peer-driven turn can never ask onward, so
A→B→A mutual-wait cannot form. Auto-registered by `wire_agent` whenever the bridge module is in
the roster (registered BEFORE manifest objectives; first hard-veto wins). Wire `hops` stays as
defense-in-depth.

**`/share` flow:** no turn, no gate — a thread-safe
`bb.post("PEER.<from>.<key>", value, "bridge")`. The prefix is collision-proof: a peer can
never inject `USER_MESSAGE`, `LLM_RESPONSE`, or any local key.

## Outbound

**`ask_agent` native tool** (`hades-ask-agent`, links cpr like `http_fetch`):
- LLM args: `{peer, message}` (both required strings; name-checked against the roster).
- Wiring appends to argv: the peer roster as `name=url` pairs, the secret **env-var NAME**
  (the tool getenvs it — the secret is never in argv or the manifest), own agent name, and the
  ask timeout. `reject_ws` on URLs (tool argv is whitespace-split).
- Does `POST <url>/ask {v:1, from:<own name>, hops:0, message}` → returns `reply` as the tool
  result; any failure (peer down, refused, timeout, 403, malformed) → `ok:false` + reason —
  the LLM sees "worker1 unreachable" and tells the user.
- **Capability `PeerAsk` → allow** (distinct enum, SkillWrite precedent — a future policy can
  confirm-gate it with zero code). The receiver's own gates are the real protection.
- **Blocking accepted v1:** the tool runs inline on A's pump thread → A is frozen while B
  thinks. Timeout has ONE source: `Bridge { ask_timeout_s }` (default 180). Wiring applies it
  twice — as the tool's inner HTTP timeout (argv) and as a per-tool `ToolRunner` cap of
  `ask_timeout_s + 10` (cap > inner timeout, so the tool reports its own timeout error instead
  of being killed mid-write; all other tools keep the 30 s default). Tool-offload later fixes
  the freeze properly (and then extends the epoch/abandonment pattern to `TOOL_RESULT`, as
  already noted).

**Outbound share push:** the BridgeModule subscribes to the `share_out` keys; on change it
pushes to ALL peers **via an Executor worker** (LLMModule pattern — `set_executor`; without an
executor, tests run inline and stay deterministic). Fire-and-forget best-effort; a failure
posts `BRIDGE_ERROR` (visible in hades-scope + tests) and never crashes. Note: subscriber
handlers fire when the bus pumps, so an idle agent's queued changes flow on the next turn
activity. Accepted v1.

## Manifest

```
Module = bridge
Bridge
{
  name = worker1
  port = 9090
  host = 0.0.0.0                       # default 127.0.0.1; set for LAN
  secret_env = HADES_BRIDGE_SECRET
  share_out = STATUS BUDGET_SPENT_USD
  max_hops = 1
  ask_timeout_s = 180
}
Peer = front { url = http://192.168.1.10:9090 }
Tool = ask_agent { native = ./build/hades-ask-agent }
```

Wiring rules (fail-fast, `MalConfig` at launch):
- `Bridge { name }` required when the bridge module is in the roster.
- The secret env var must be SET and non-empty when the bridge module is present (no open
  listener on a missing secret).
- Peer URLs must contain no whitespace.
- `ask_agent` tool present but no Peer blocks → MalConfig (a delegation tool with nobody to
  call is a mis-wiring).
- Zero coupling otherwise: bridge module without the ask_agent tool is legal (receive-only
  agent); ask_agent without the bridge module is legal (ask-only agent that cannot be asked).

Teardown (load-bearing, extends the existing chain): Agent member order
`…, executor, bridge, telegram` — telegram destroyed first, **bridge second** (its listener
thread is stop+joined while the Executor and every module it touches are still alive),
executor third. `hades_main` best-effort resolves the bridge secret env and adds it to the
Eventlog redactions.

## Failure modes

- Peer down / connection refused / timeout → tool `ok:false` + reason; the asker's turn
  continues normally.
- B busy (TurnGate held by a long local turn) → A's tool blocks up to `ask_timeout_s` →
  timeout error.
- Bad secret / unknown `from` → 403, silent drop.
- B's peer-driven turn hits the idle timeout → `/ask` returns `{ok:false, error:"turn timed
  out"}`; the existing `TURN_ABANDONED` hardening applies unchanged (bridge posts it like the
  other front-ends).
- Listener exceptions → caught, 500, keeps serving. Malformed JSON → `{ok:false}`.

## Testing

- **Pure:** protocol parse/build helpers, `PEER.<name>.<KEY>` naming, hops/size/version
  rejection, PeerLoopGuard veto on `TURN_ORIGIN = peer:*` (and non-veto on `human`).
- **Module (real loopback HTTP):** `/ask` end-to-end with a scripted LLM — reply path, confirm
  **auto-deny** path, allowlist 403, bad-secret 403; `/share` ingest + prefix; `my_turn_`
  isolation vs a concurrent REPL turn.
- **Tool:** `hades-ask-agent` against a stub httplib server (success, 403, down, timeout,
  non-string args fail closed).
- **Two-agent e2e:** two full agents (two Blackboards, two ports) in ONE test process — A asks
  B through the real tool + real bridge; loop-guard e2e (B's peer-driven turn tries ask_agent
  → vetoed); share e2e (A's key change lands as `PEER.A.KEY` on B).
- **TSan** full suite (new listener thread × TurnGate × Executor).

## v2 seams (recorded, not built)

Per-peer secrets · per-peer share lists · per-peer confirm policy (propagate to the asker's
human) · per-key rename (full pShare) · `max_hops > 1` (wire field already present) ·
transport behind a small interface (queue/webhook) · discovery (static roster now,
registry/mDNS later) · inbound share whitelist · peer presence via `/health` polling ·
ask offload (with tool-offload, un-freezing the asker during a peer turn).
