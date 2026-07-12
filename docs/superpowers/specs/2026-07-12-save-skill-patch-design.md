# save_skill patch mode — design

**Date:** 2026-07-12
**Status:** approved (Vaios, 2026-07-12)
**Origin:** Hermes-agent borrow (`skill_manage patch`, 2026-07-11 research) — CLAUDE.md Skills v2
idea-list item 5b. Token-cheap skill self-improvement: the agent refines a skill incrementally
with an `old_string`/`new_string` replacement instead of resending the whole SKILL.md body.

## Goal

Extend the existing `save_skill` native tool (`tools/save_skill_main.cpp`) with a **patch mode**
so the agent can edit part of an existing skill without retransmitting the full body. No new
tool binary, no module/wiring/capability changes.

## Decision (approach)

**Extend `save_skill`'s args** — rejected alternatives: an explicit `action = save|patch` field
(core_memory precedent; bigger schema, does not remove the weak-LLM fills-every-field problem)
and a separate `patch_skill` binary (new CMake target + manifest lines + capability row +
SkillsModule second watch — heaviest for the same behavior).

## Behavior

`save_skill` gains two OPTIONAL string args: `old_string`, `new_string`. Mode select at
dispatch, **empty string = absent** (the schedule_task exactly-one-of rule, `833b9aa`):

| args present (non-empty) | mode |
|---|---|
| `body` only | **save** — today's path, byte-identical behavior (frontmatter rebuilt from `name`+`description`) |
| `old_string` only | **patch** — see below |
| both `body` and `old_string` | `ok:false` — error names the rule ("provide body OR old_string/new_string, not both") |
| neither | `ok:false` — same self-healing error |

**Patch mode:**
1. `name` gated by `valid_skill_name` (shared header) — unchanged.
2. Read `<skills_dir>/<name>/SKILL.md` (skills dir = argv[1], fixed by wiring). Missing file →
   `ok:false` "no such skill" (patch never creates; creation is save mode's job).
3. `old_string` must match **exactly once** (edit_file contract): 0 matches → "old_string not
   found"; >1 → "matches N times — give more surrounding context". **No `replace_all`** —
   skill files are small; more context is the fix (YAGNI).
4. `new_string` may be empty (= deletion). `old_string == new_string` → `ok:false` (edit_file
   parity).
5. Apply the replacement in memory, then **post-patch validation**: the result must still parse
   as a valid skill — opens with the `---` fence, closing fence present,
   `parse_skill_description` yields non-empty. Fail → refuse, file untouched, error: "patch
   would break the skill's frontmatter — fix old/new_string or resend the full skill with
   body". This stops the agent bricking a skill out of its own announce roster.
6. Atomic write: tmp + rename (existing pattern in this tool).
7. Success result: `{"patched": true, "name": ..., "path": ...}`.

**`description` in patch mode:** non-empty `description` alongside non-empty `old_string` →
`ok:false` ("patch edits the file directly — put the description change in
old_string/new_string"). One mode = one arg set; no half-merge semantics. (Empty `description`
is simply absent, per the empty=absent rule.)

**Describe schema:** `old_string`/`new_string` added as string properties; `required` shrinks to
`["name"]` (mode args are conditionally required — enforced by the dispatch table above, and
`description`+`body` remain required *for save mode* via the existing runtime check). The tool
description text explains both modes in one short paragraph.

## What does NOT change

- **SkillsModule rescan** — keys on `tool == "save_skill"` + `ok:true` only; a successful patch
  fires the re-announce for free. Zero module changes.
- **Capability** — `capability_of("save_skill") == SkillWrite → allow`. Unchanged.
- **Wiring** — skills dir stays argv[1], single source of truth. Unchanged.
- **Staleness guard** — none (v1, documented): the Arbiter injects `expect_version` only into
  `edit_file`/`write_file`; save_skill is untracked. Acceptable because the match-exactly-once
  check runs against the file's LIVE disk content at patch time — a stale `old_string` simply
  fails to match (0 hits) and the error tells the agent to re-read. Self-healing without
  version plumbing.
- **Announce-line injection** — non-issue by construction: `scan.cpp`'s
  `parse_skill_description` getlines the frontmatter, so a description value cannot span lines
  in the announce regardless of what a patch writes.

## Files

| file | change |
|---|---|
| `tools/save_skill_main.cpp` | patch branch + mode dispatch + post-patch validation + describe update |
| `tests/test_save_skill_tool.cpp` | append ~10 cases: patch roundtrip · deletion (empty new_string, edit_file parity) · 0-match · >1-match · both-modes error · neither error · description-with-patch error · brick-refusal (frontmatter broken) · missing-skill · traversal name in patch mode |
| `CMakeLists.txt` | one line: add `src/skills/scan.cpp` to the `hades-save-skill` target sources (see below) |
| `docs/manifest-reference.md` | save_skill tool row: mention patch mode |
| `CLAUDE.md` | mark Skills v2 item 5b shipped; one-line feature note |
| `prompts/soul.md` | skills paragraph: one sentence — refine an existing skill with old_string/new_string instead of resending the body |

No new target; no core link. Post-patch validation reuses `parse_skill_description`, which is
declared in `hades/skills/scan.h` (already included for `valid_skill_name`) but DEFINED in
`src/skills/scan.cpp` — the tool does not link `hades_core`, so it compiles `src/skills/scan.cpp`
into itself (the cron_store precedent from the self-scheduling tools). That one
`target_sources(hades-save-skill PRIVATE src/skills/scan.cpp)` line is the only CMake change.

## Testing

Baseline **614/614** (ASan+UBSan) green before work; suite green after. TSan not needed (no
threads touched). Live smoke (Vaios, later): ask the agent to tweak one line of an existing
skill — expect a `save_skill` call with `old_string`/`new_string`, announce unchanged unless
description edited, `skills/<name>/SKILL.md` diff shows the one-line change.
