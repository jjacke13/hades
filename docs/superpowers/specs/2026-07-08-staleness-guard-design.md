# hades edit/write staleness guard — design (lost-update protection for file mutations)

**Date:** 2026-07-08
**Status:** approved (Vaios — "A ok")
**Branch:** `feat/staleness-guard` off `main` @ `cfc5f89`
**Origin:** the Claude Code tool gap-analysis backlog (CLAUDE.md "edit/write staleness guard").

## The gap

`write_file` blind-truncates and `edit_file` replaces against the file's **current** bytes with no check
that the LLM's view of the file is current. A change between the LLM's `fs_read` and its `edit_file`/
`write_file` — another front-end's turn, a heartbeat self-turn, the human editing in an editor, an
external process — is **silently clobbered** (classic lost update). This matters more since heartbeat/
self-scheduling landed: an unattended self-turn can now mutate a file a human just changed.

Scope is **staleness only**: `edit_file` already enforces the unique-match contract (`count==0`→error,
`count>1 && !replace_all`→error — do NOT re-add), and no new "must read before edit" rule is introduced.
A file hades has no recorded view of behaves exactly as today.

## The design (approach A — Arbiter-threaded version token)

A hybrid of the two backlog options, smaller than either: tools **report** content versions, the
**Arbiter** (the one stateful component that sees every request/result) threads them, and the **tool**
enforces the check atomically next to its write. The LLM never sees or copies tokens.

### 1. Tools report a `version` (content hash)

- **`fs_read`** result gains `"version": "<hash>"` alongside `content` — the hash of the exact bytes
  returned.
- **`edit_file`** result gains `"version": "<hash>"` — the hash of the **new** content it just wrote.
- **`write_file`** result gains `"version": "<hash>"` — the hash of the content it just wrote.

Hash = **FNV-1a 64-bit** over the raw bytes, rendered as 16 lowercase hex chars. Implemented once as a
header-only inline (`include/hades/tool/file_version.h`, the `valid_skill_name` precedent) so the three
tool binaries share it without linking `hades_core`, and tests can call it directly. (Not a security
boundary — a collision-resistance requirement would be overkill; this detects accidental concurrent
modification, not an adversary.)

### 2. Arbiter tracks `path → version`

Two small private members:

- `file_versions_` : `std::map<std::string, std::string>` — canonical path → last-observed version.
  Updated in `on_tool_result` whenever a tracked file op (`fs_read`/`edit_file`/`write_file`) returns
  `ok:true` with a `version` string. A refused/failed op updates nothing.
- `pending_file_ops_` : `std::map<std::string, std::pair<std::string,std::string>>` — tool-call id →
  (tool, canonical path), recorded at dispatch so the result can be correlated (the TOOL_RESULT payload
  carries only `{id, ok, content}`).

**Canonical key:** the same lexical normalization family as `capability_policy`'s `canon_path`
(collapse `./`, duplicate separators), so `./x.txt` and `x.txt` map to one entry. Lexical only —
symlinks/hardlinks are not resolved, the same documented v1 gap as the capability model.

**Lifetime:** in-memory, process-lifetime. A restart clears it (a resumed LLM should re-read before
editing — its own memory-injection framing already says "re-verify current state"). `/new` does NOT
clear it: the map describes disk state, not conversation state, and a stale entry self-heals (see §4).
Updating on every read keeps entries current; consecutive `edit_file` calls without a re-read keep
working because each successful write updates the map with the new version.

### 3. Arbiter injects `expect_version` at dispatch

In `dispatch_or_gate`, when the action is an `edit_file` or `write_file` ToolCall, the Arbiter first
**erases any LLM-supplied `expect_version`** from the args (the field is Arbiter-owned plumbing — a
hallucinated value must never reach the tool), then, if the `path` has an entry in `file_versions_`,
sets `args["expect_version"] = <version>` before posting `TOOL_REQUEST`. No entry → no injection →
the tool behaves exactly as today (fail-soft). The injected
value appears in the Eventlog's `TOOL_REQUEST` → the guard is observable in `hades-scope` for free.
(The conversation history keeps the LLM's original args; only the dispatched request carries the
injection — an Eventlog-visible, LLM-invisible mechanism.)

### 4. Tools enforce the check (inside the writing subprocess)

Both mutating tools accept an **optional** `expect_version` arg (string). It is deliberately **absent
from the `describe` schema** — it is Arbiter-injected plumbing, not LLM API surface; documenting it to
the model would invite the model to guess values.

- **`edit_file`**: it already reads the file once. Hash that same content; if `expect_version` is
  present and differs → `ok:false`, error
  `"file changed on disk since you last read it — fs_read it again and retry"`, **file untouched**.
  The check precedes match/replace, and the content checked is the content edited (no second read →
  no gap between check and edit; the atomic tmp+rename already published either old or new bytes).
- **`write_file`**: if `expect_version` is present, read the current file first: unreadable/missing
  (deleted since read) → refuse with the same error; hash mismatch → refuse. No `expect_version` →
  today's behavior (blind write). **`write_file` also becomes atomic** (tmp + rename with mode
  preservation — the `edit_file` pattern): the guard promises "file untouched on refusal", and the
  plain-`trunc` write it has today can leave a torn file on crash — the exact lost-update class this
  feature targets. Small, in-scope hardening.

**Recovery is self-healing:** the refusal error instructs the LLM to `fs_read` again; the fresh read
updates `file_versions_`; the retried edit gets the new `expect_version` injected and succeeds. No
confirm prompt, no human wakeup — this is a correctness gate, not a permission gate, so it works
identically on human, peer, and heartbeat-origin turns (a confirm would auto-deny on the latter two).

## What is deliberately NOT covered (v1)

- **`grep`/`glob`/`git_read`/`shell` are not tracked** — only the three file-content ops carry
  versions. A write performed via `shell` is invisible to the map (next `fs_read` re-syncs it).
- **No cross-restart persistence** (map is memory-only; restart = re-read).
- **No mtime** (content hash only — exact, no filesystem-granularity issues).
- **No LLM-visible tokens** (schemas unchanged; `expect_version` is injected, not documented).
- **No read-before-edit requirement** (unread file → today's behavior; staleness only).
- **No config switch** — always-on, zero-key. Fail-soft by construction (no record → no check).
- Lexical path keys (symlink aliasing unhandled — capability-model v1 parity).

## Testing

- **`file_version.h`** (pure): stable hash for fixed bytes; differs on 1-byte change; empty input.
- **`fs_read` tool**: result carries `version`; equals the header hash of the returned content.
- **`edit_file` tool**: matching `expect_version` → edit succeeds + result `version` = hash of new
  content; mismatched → `ok:false` + file byte-identical; absent → today's behavior (edit succeeds);
  the error text mentions re-reading.
- **`write_file` tool**: same trio; plus deleted-file-with-expect_version → refuse; plus
  atomicity/mode-preservation (exec bit survives an overwrite — the edit_file precedent test).
- **Arbiter**: fs_read TOOL_RESULT populates the map (via a scripted bus rig, no real LLM);
  edit_file dispatch carries the injected `expect_version` in TOOL_REQUEST; unknown path → no
  injection; a failed (ok:false) result does not update the map; a successful edit_file result
  updates the map (edit→edit chain works); `./x` and `x` share one entry.
- **E2E (wiring)**: real binaries through the ToolRunner — read, mutate externally, edit → refused;
  re-read, edit → succeeds.

## Pieces (anticipated)

- `include/hades/tool/file_version.h` — inline FNV-1a 64 → 16-hex (header-only, shared).
- `tools/fs_read_main.cpp`, `tools/edit_file_main.cpp`, `tools/write_file_main.cpp` — version in
  results; `expect_version` check; write_file atomic rename.
- `src/apps/arbiter/arbiter.cpp` (+ `include/hades/arbiter.h`) — `file_versions_`,
  `pending_file_ops_`, dispatch injection, result harvesting, path canonicalization.
- Tests: `tests/test_file_version.cpp`, extensions to `tests/test_tools.cpp` (or the tools' test
  files), `tests/test_arbiter.cpp`, an e2e case in the wiring tests.
- Docs: CLAUDE.md (backlog item → shipped), `docs/manifest-reference.md` (a note under the tool
  section that mutating tools are staleness-guarded), no soul.md change (LLM-invisible by design —
  the error message itself is the guidance).
