# hades Skills System — Design

**Date:** 2026-07-02
**Status:** Approved (design review with Vaios, 2026-07-02)

## Goal

Give the hades agent **skills**: loadable capability/instruction packs it can discover, invoke,
and — crucially — **author itself**. A skill is a directory holding a `SKILL.md` (instructions)
and optional scripts. The agent sees a one-line announcement of every available skill in its
system prompt, pulls a skill's full body into context on demand with a `use_skill` tool, and
distills learned procedures into new skills with a `save_skill` tool.

**MOOS framing:** a skill is the software analog of a loadable behavior file (`.bhv`) — but
loaded by the helm at runtime instead of fixed at mission start. The `SkillsModule` is the
behavior-library app: it publishes the available roster; the Arbiter (helm) consults it.

## Decisions (from design review)

| Question | Decision |
|---|---|
| What a skill carries | Full pack: `SKILL.md` instructions + optional `scripts/` |
| Who authors | **Mostly the agent** (via `save_skill`); user can hand-drop `SKILL.md` dirs too |
| Discovery/invocation | Name+description list in system prompt; explicit `use_skill(name)` tool loads the body |
| Skill executables | Run via the **existing `shell` tool** — existing capability gating + confirm applies unchanged; no dynamic tool registration |
| Architecture | Dedicated **`SkillsModule`** (announce lives on the bus), file structure per below |
| Announce refresh | Event-driven: rescan only when a `save_skill` call succeeds — **no per-turn scan**. (The list rides the system prompt, which is sent with every LLM request regardless; cost ≈ one line per skill.) |
| Coupling | **None.** Tools without module, module without tools, either alone, neither — all legal. `skills_dir` defaults to `skills`. |

## File structure

```
skills/
  <name>/
    SKILL.md        # frontmatter + instruction body
    scripts/...     # optional; agent runs these via the shell tool per SKILL.md instructions
```

`SKILL.md` format (Claude Code convention):

```markdown
---
name: deploy-webapp
description: How to build and deploy the webapp to the staging box
---
<instruction body — free-form markdown>
```

- The **directory name is the canonical skill id** (what `use_skill` resolves). The frontmatter
  `description` feeds the announcement; frontmatter `name` is display-only.
- A dir without a parseable `SKILL.md` (missing file, no frontmatter, empty description) is
  **skipped** at scan time — never crashes the scan.

## Components

### 1. `SkillsModule` (`type() == "skills"`, roster `Module = skills`)

- **on_attach:** scan `skills_dir`, parse each `SKILL.md` frontmatter, post **`SKILLS_ANNOUNCE`**
  (a preformatted string) to the Blackboard:

  ```
  Available skills (call use_skill with a name to load its full instructions):
  - deploy-webapp: How to build and deploy the webapp to the staging box
  - nixos-module: How to write a NixOS module for this workspace
  ```

  Empty/missing dir → post `""` (Arbiter skips empty — backward compat, zero cost).
- **Refresh (event-driven):** subscribe `TOOL_REQUEST`; when `tool == "save_skill"`, remember the
  request `id`. Subscribe `TOOL_RESULT`; when a remembered `id` arrives with `ok == true`, rescan
  and repost `SKILLS_ANNOUNCE`, then forget the id. Single-threaded pump → no races; scan is a
  tiny synchronous fs walk on the pump thread (tools already run inline there today).
- Module absent → key never posted → feature invisible.

### 2. Arbiter injection

`start_turn()` reads `bb_->get("SKILLS_ANNOUNCE")` (latest-value map — **no subscription, no new
handler**) and, when non-empty, folds the string into the **leading** `{role:system}` message,
next to the live core-memory layer. Key absent or empty → no block.

Note on freshness/ordering: `post()` updates the latest-value map immediately (pump is only for
handlers), so the announce posted in `on_attach` is visible to the first `start_turn` regardless
of pump order.

### 3. Two subprocess tools (pin_fact/save_memory pattern)

Self-describing binaries; the wiring appends the resolved skills dir to each tool's argv
(single source of truth — the LLM never chooses the directory).

- **`use_skill`** — args `{name}`. Reads `skills/<name>/SKILL.md`, returns its full content in
  the result. The content arrives as a normal `TOOL_RESULT` → enters `history_` → persists in
  context naturally (and is windowed by `history_budget_chars` like everything else).
  Missing skill → `{"ok":false, "error":"no such skill: <name>"}` listing nothing sensitive.
- **`save_skill`** — args `{name, description, body}`. Composes the canonical frontmatter +
  body, creates `skills/<name>/` (parents as needed), writes `SKILL.md` **atomically enough**
  (write temp + rename, matching the wizard atomic-write habit). Existing skill → overwrite
  (that *is* the update path). Success → `{"ok":true, "result":{"name":..., "path":...}}`.

**`name` sanitization (both tools, security-critical):** accept only `[A-Za-z0-9_-]+` (reject
empty, `.`, `/`, `\`, whitespace). Otherwise `save_skill(name="../../home/user/.bashrc")` is an
arbitrary-file-write escape and `use_skill(name="../..../etc/passwd")` an arbitrary read. Fail
closed with `{"ok":false}` — never write/read outside `skills_dir`.

### 4. Capability model

Extend the built-in `capability_of` table (the authority — tools cannot self-grant):

| tool | capability | CapabilityPolicy verdict |
|---|---|---|
| `use_skill` | `SkillRead` (new) | allow |
| `save_skill` | `SkillWrite` (new) | allow |

Rationale for allow-without-confirm: `pin_fact` already performs unconfirmed writes that inject
into **every** turn's prompt; a saved skill is strictly weaker (its body only enters context on
an explicit `use_skill`). Distinct capabilities (not `MemoryAppend`) keep the table honest so a
future policy can confirm-gate `SkillWrite` without touching code. Scripts inside skill packs
run through the existing `shell` tool — its confirm/veto gating is untouched.

Without table entries the policy's `confirm_unscoped` would nag on every skill call — the new
entries are required, not cosmetic.

### 5. Config & wiring

- **`Skills` block** (optional): `Skills { dir = skills }`. One `resolve_skills_dir(manifest)`
  helper in wiring returns the block's `dir` or the default `skills`; the same value is handed
  to the module (setter before `on_attach`) **and** appended to both skill tools' argv.
- Path must contain **no whitespace** (argv is whitespace-split — same MalConfig rule as the
  memory store paths).
- **No coupling MalConfig:** every combination of {skill tools present/absent} ×
  {`Module = skills` present/absent} is legal. Tools without module: functional, no announce
  (agent can `list_dir` the skills dir). Module without tools: announce-only; skills readable
  via `fs_read` if the user scopes `fs_read_allow` accordingly.
- **dev.hades** ships the full feature: `Module = skills`, `Skills { dir = skills }`,
  `Tool = use_skill`, `Tool = save_skill`.
- `skills/` is git-tracked (like `memory/facts.md`) — agent-authored skills show as working-tree
  churn for the user to review/commit as curated capabilities.

### 6. soul.md

Add a skills paragraph: you have a skills library; the list in your prompt is your available
skills; call `use_skill` before doing a task a skill covers; when you work out a reusable
procedure (or the user teaches you one), **distill it into `save_skill`** — name it well, write
the description so your future self picks it correctly from one line.

## Error handling

- Scan: unreadable dir / file / bad frontmatter → skip entry (module never throws on the pump
  thread; whole scan wrapped try/catch, consistent with embedding module's handler guard).
- Tools: all failures → single-line `{"ok":false, ...}` JSON on stdout (native tool protocol).
- Arbiter: missing/empty/non-string `SKILLS_ANNOUNCE` value → no block (is_string guard, same
  as the memory keys).

## Testing

- **Frontmatter parser** (pure fn): happy path, missing frontmatter, empty description, body
  extraction.
- **SkillsModule:** scan+announce format, empty dir → `""`, skip-bad-dir, refresh on
  save_skill TOOL_RESULT (and no refresh on other tools / failed saves).
- **Tools:** describe, roundtrip save→use, overwrite, name sanitization (traversal attempts
  fail closed), missing skill.
- **Arbiter:** announce folded into leading system message; absent/empty → no block.
- **Wiring:** default dir, `Skills { dir }` override, argv append, whitespace MalConfig,
  no-coupling combinations build.
- **Manifest lock test:** shipped dev.hades parses with the new blocks.

## Deferred (v2)

- `delete_skill` / rename tooling (today: user deletes the dir by hand).
- Relevance hints ("skill X may apply") via the embedding module.
- Per-skill capability scopes; confirm-gating `SkillWrite` by policy.
- Skill-declared first-class tools (dynamic tool registration).
- Announce pagination if the library grows past dozens of skills.
