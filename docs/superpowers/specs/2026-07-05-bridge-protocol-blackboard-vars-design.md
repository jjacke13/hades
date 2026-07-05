# Bridge-as-protocol: standardized blackboard variables + capability discovery — design

**Date:** 2026-07-05
**Status:** Approved (Vaios — brainstorm 2026-07-05, sections 1–5 approved)
**Branch:** `feat/bridge-protocol` (off `main`)
**Scope:** A+B of the "bridge → protocol" direction (canonical vars + discovery + typed `/share` +
turn-start peer-aware folding). Cross-harness interop (channel C) is **deferred**.

## Problem

Today the Bridge (spec 2026-07-03) shares only **session text**: `/ask` drives a turn and returns a
reply; `/share` stores an **opaque** `PEER.<from>.<key>` value with no type. An agent has no way to
learn *what another agent can do* — its skills, tools, or capability shape. Vaios wants agents to
**advertise and share structured capability state**, so a delegating agent can route by advertised
capability, so the peer set can refresh live, and so an agent can react to peer-shared data —
"standardize the variables and values published on the blackboard." A secondary goal: shape the wire
so cross-harness interop (a Claude Code / A2A agent talking to hades) stays cheap **if** pursued later.

## Decision summary (from the brainstorm)

- **First slice = A+B together:** canonical vars + discovery **and** typed `/share` + turn-start
  peer-aware folding. Channel C (full A2A interop) deferred.
- **Payoffs (all three):** smarter delegation, reactive ingest, dynamic roster.
- **Interop anchor = A2A-shaped card content** (A2A agent-card field names), but served at a **plain
  bridge path** (`GET /card`), **not** `/.well-known/agent.json`. Peers are **manifest-seeded** with a
  full base URL, so hades controls the path; the well-known alias is a one-line later add for a
  non-hades A2A client. The **manifest allowlist stays the trust boundary** — discovery enriches the
  *capabilities* of already-allowlisted peers; it does **not** admit strangers.
- **Registry formality = constants + builders + tolerant parse** (Option 1). Standard key **names** as
  C++ constants + small pure builder helpers; value shapes **documented**, parsed tolerantly (ignore
  unknown fields — the existing additive bridge-protocol style). **No** strict schema validator.
- **Provenance = split by type, with a per-peer trust tier.** Capability metadata is trusted for
  routing; factual content is trust-tiered — a **trusted** peer (all manifest peers today) is
  usable-with-provenance, an **untrusted** peer (`trust = untrusted`, the seam for future dynamic
  joiners) is a tagged report only. In v1 all peers default trusted, so the untrusted path is wired
  but unused.

## Architecture — two channels, one canonical vocabulary

### Channel 1 — the Card (PULL): capability discovery

Each agent's bridge serves a secret-gated **`GET /card`** returning an **A2A-shaped agent-card** built
**on demand** in a socket-free `card_json()` seam (mirrors `health_json()`):

```json
{
  "name": "hades2",
  "description": "…persona one-liner…",
  "url": "http://host:9090",
  "version": 1,
  "capabilities": { "streaming": false },
  "skills": [ { "id": "deploy", "description": "…" } ],
  "tools":  [ { "name": "shell" }, { "name": "http_fetch" } ],
  "caps":   { "fs_read": "scoped", "fs_write": "scoped", "net": "public", "exec": "scoped" }
}
```

- `name`, `url`, `version` — the bridge already holds these.
- `description` — injected at wiring (`set_description`); default = `name`.
- `skills[]` — from `bb_->get("SKILLS_ANNOUNCE")`, **reverse-parsed** from its fixed `- <id>: <desc>`
  line format (a ~10-line tolerant parser; no new bus var, no `SkillsModule` change).
- `tools[]` — the tool roster, injected at wiring (`set_tools`) — the same list `ask_agent` gets.
- `caps{}` — a **SUMMARY** of the `capability_policy` scopes (categories/booleans), injected at wiring
  (`set_caps`). **Never the literal fs paths / exec prefixes** — routing needs the shape, not the
  allowlist contents (leak avoidance, even to a trusted peer).

`skills`/`tools`/`caps` are A2A-extension fields (A2A ignores unknown fields; a hades peer reads them).
`name`/`description`/`url`/`capabilities`/`skills` keep A2A agent-card names so a later A2A client reads
the card with zero rework.

**Discovery (pull).** `BridgeHttp` gains `get_json(url, secret, timeout)` (cpr::Get in `CprBridgeHttp`).
A small **discovery timer thread** (the proven embedding-reindex lifecycle: sleep/notify/join in the
dtor) re-pulls every manifest peer's `/card` every `discover_interval_s` (default **300s**, `0`=off);
the first tick is the boot pull. Each successful pull posts `PEER.<peer>.card` locally. Best-effort: a
down/unreachable peer is skipped and logged to `BRIDGE_ERROR`, never fatal (same posture as
`run_share_push`).

### Channel 2 — Typed `/share` (PUSH): reactive updates + content

The share envelope gains a **`type`** field (tolerant; **absent → `"raw"`** = today's exact behavior,
so all existing `/share` traffic is unchanged). v1 types:

- **`card`** — the value is an agent-card → post `PEER.<from>.card`. This is the **reactive push /
  boot self-announce**: a fresh agent posting its card for the first time is a change → it pushes
  `type=card` to all its manifest peers, so a late booter instantly announces itself to every peer
  already up, and a capability change re-pushes. **Capability metadata → trusted for routing**
  regardless of the peer's trust tier.
- **`fact`** — a content claim → post `PEER.<from>.fact.<key>`, **trust-labeled** (below).
- **`raw`** (default) — → `PEER.<from>.<key>`, unchanged legacy opaque share.

The rename-on-arrival guarantee is unchanged: a peer can never write a local (non-`PEER.`) bus key.
`kMaxShareBytes` still caps the body.

### Discovery is boot-order-independent

Agents run on different machines with **no shared start time**. The two directions together give
eventual convergence for any boot order:

- **Push self-announce (immediacy):** a new/changed agent pushes its card to all its manifest peers
  the moment it boots or changes — so whoever is already up learns it in real time.
- **Periodic pull (safety net):** the discovery timer re-pulls every peer's card every
  `discover_interval_s`, catching the cases push misses — a peer that was **down** during the push, a
  restart, or capability drift on a peer that does not push. First tick = boot pull.

Neither assumes simultaneous boot. Presence tracking (marking a peer down on a failed pull) is **not**
v1 — a down peer's `ask_agent` fails at call time anyway; v1 keeps the last-known card.

### Trust tiers

`Peer` block gains optional **`trust = trusted | untrusted`** (default `trusted`). The bridge tracks
`name → {url, trust}`. On an inbound `fact`:

- **trusted** peer (all manifest peers today) → labeled `"peer <name> reports:"` — usable.
- **untrusted** peer (`trust = untrusted`; the seam for future dynamic joiners) → labeled
  `"unverified claim from <name>:"` — re-verify, never authoritative.

Both land as `PEER.*` bus vars carrying the provenance in the label. In v1 all peers default trusted,
so the untrusted path is wired but has no members until dynamic-join lands.

### Consumption — Arbiter turn-start fold (NOT a live subscription)

The Arbiter `get`-scans `PEER.*` at turn start (exactly the `SKILLS_ANNOUNCE` fold pattern) and folds
two blocks into the leading system message:

- `PEER.*.card` → a **"Peers you can delegate to:"** block — each line a peer + its skills/caps
  summary. The LLM routes `ask_agent` by advertised capability instead of blind. `ask_agent`'s own
  `describe` stays names-only (unchanged — a subprocess tool can't see the bus).
- `PEER.*.fact` → a **"Reported by peers (treat as claims, re-verify):"** block, trust-labeled.

Both-empty → no block (backward-compatible). This needs one small Blackboard **read** helper —
`entries_with_prefix("PEER.")` — a const snapshot of matching latest-value entries. **No
`subscribe_prefix()`** in v1: a turn-start scan covers both folds. True between-turns reactivity (act
the moment a peer pushes) is the **heartbeat direction's** consumer and gets the live subscription then.

## Config (manifest)

```
Bridge
{
  name          = hades2                 # existing — identity (bus keys + peer:<name> origin)
  host          = 127.0.0.1              # existing
  port          = 9090                   # existing (0 = ephemeral)
  secret_env    = HADES_BRIDGE_SECRET    # existing — env only, redacted
  max_hops      = 1                      # existing
  ask_timeout_s = 180                    # existing
  description         = "a deploy/ops helper"   # NEW — card persona one-liner (default = name)
  discover_interval_s = 300                     # NEW — peer-card re-pull; 0 = off (boot pull only)
}
Peer = hades1 { url = http://host1:9090 }                 # existing; trust defaults to trusted
Peer = watcher { url = http://host2:9090  trust = untrusted }   # NEW — trust tier
```

Injected at wiring (never in the manifest): card `tools[]` (roster `ask_agent` already receives),
card `caps{}` (from the `capability_policy` block, summarized), `description` source. Bridge gains
`set_description` / `set_tools` / `set_caps`. There is **no `announce_card` flag** — a bridged agent
always self-announces to its allowlisted peers.

## Security

- **`/card` is secret-gated** (like `/health`): only an allowlisted-secret holder reads it. The card is
  **not public** — capability info never leaks to the open network. (A public card + `/.well-known/`
  alias is a deliberate later opt-in for real A2A interop, not v1.)
- **`caps{}` is a summary, never the literal allow-lists** — categories/booleans only, so a trusted
  peer learns the *shape* of what the agent may do, not its exact fs paths / exec prefixes.
- **No regression of the existing Bridge SECURITY note:** a peer `/ask` can still read out whatever
  reaches a plain answer (injected memory, folded skills, files already read). The card adds
  *capability metadata*, not memory access — the surface is unchanged. Do not put peer-secret content
  in a bridged agent's memory/reachable files (unchanged advice).
- **Untrusted-peer facts are never authoritative** (label-enforced; the LLM sees "unverified claim").
- Secret stays **env-only, redacted** in `session.log`. `kMaxShareBytes` caps typed shares.

## Components (file boundaries)

| File | Responsibility |
|---|---|
| `include/hades/bridge/registry.h` | canonical key constants + `SkillCard`/etc. structs; pure builder decls |
| `src/bridge/registry.cpp` (or fold into `bridge.cpp`) | `build_card`, `build_skills_from_announce` (reverse-parse), `caps_summary` |
| `include/hades/bridge/protocol.h`, `src/apps/bridge/bridge.cpp` | `type` field on `build_share`/`parse_share`/`BridgeMsg` (tolerant default `raw`) |
| `include/hades/blackboard.h`, `src/core/blackboard.cpp` | `entries_with_prefix(prefix)` const read helper |
| `include/hades/module/bridge_module.h`, `src/apps/bridge/bridge.cpp` | `card_json()`, `set_description/set_tools/set_caps`, `GET /card` route, `get_json`, discovery timer thread, typed `handle_share` routing + trust map, boot self-announce |
| `include/hades/bridge/http.h`, `src/apps/bridge/bridge.cpp` | `BridgeHttp::get_json` + `CprBridgeHttp` impl |
| `src/apps/arbiter/arbiter.cpp` | fold `PEER.*.card` (delegation) + `PEER.*.fact` (reports) into the leading system message |
| `app/agent_wiring.cpp`, `app/hades_main.cpp` | feed description/tools/caps, parse `Peer` trust + `discover_interval_s`, start the discovery timer after wiring (like the listener) |
| `manifests/dev.hades`, `docs/manifest-reference.md`, `prompts/soul.md`, `CLAUDE.md` | ship: commented keys + trust example, new-key reference + card schema + canonical-vars table, delegation guidance, notes |

## Testing

TDD, ~30 new tests (baseline **426**). Per-unit:

- **Builders:** card shape/field names; `build_skills_from_announce` reverse-parse (incl. empty →
  `[]`); `caps_summary` contains **no literal paths**; typed-envelope default `raw`.
- **Protocol:** typed share build/parse round-trip; unknown-field tolerance; absent-type → `raw`.
- **Blackboard:** `entries_with_prefix` returns matching latest-value entries; empty prefix / no match.
- **Bridge card:** `card_json()` assembles injected + bus inputs; `GET /card` **403 without secret,
  200 with**; card omits literal allow-lists.
- **Discovery:** injected `BridgeHttp::get_json` returns a canned card → `PEER.<peer>.card` posted; a
  failing pull logs `BRIDGE_ERROR`, posts nothing, never throws.
- **Inbound routing:** `handle_share` `type=card|fact|raw` × trusted|untrusted → correct `PEER.*` keys
  and labels; a peer cannot write a non-`PEER.` key.
- **Arbiter folds:** `PEER.x.card` → delegation block; `PEER.y.fact` (trusted/untrusted) → reports
  block with the right label; both-empty → no block.
- **Wiring:** manifest with `Bridge` + `Peer(trust)` → bridge carries description/tools/caps/trust;
  `GET /card` returns them; `discover_interval_s` parsed.
- **TSan at feature end** — the module now runs two threads (listener + discovery timer).

## Deferred / v2

- **Peer-knowledge persist:** trusted peer facts accumulate in the archival memory store,
  provenance-tagged (Vaios: "add the knowledge from peers later") — the immediate next step after ship.
- `subscribe_prefix()` true between-turns reactivity → arrives with the **heartbeat** direction.
- Presence tracking (peer down on failed pull).
- **Full A2A interop (channel C):** public/unauthenticated card + `/.well-known/agent.json` alias +
  JSON-RPC `tasks/send` + SSE streaming.
- Transitive/gossip discovery (peers-of-peers) + open-network join (untrusted tier gets real members)
  + DID identity (ANP) for P2P / hyperdht.
- Async `/ask` task-id polling (existing bridge v2 seam).

## Non-goals

`/.well-known/agent.json` in v1 · a public card · a strict schema validator · persist-to-memory ·
between-turns reactive subscription · presence/health polling · JSON-RPC/SSE transport · open-network
peer join.
