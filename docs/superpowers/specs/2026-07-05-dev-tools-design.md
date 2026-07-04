# Dev tools: Claude-Code-style coding toolkit — design

**Date:** 2026-07-05
**Status:** Approved (Vaios — brainstorm 2026-07-04/05, "good!")
**Branch:** `feat/dev-tools` (off `main` @ `fc5f291`)

## Problem

hades agents have 10 tools but weak coding ergonomics: no search-in-files, no recursive file
find, no surgical edit (only whole-file `write_file`, always confirm-gated), no cheap git
introspection (only `shell`, always confirm), no build/test runner that doesn't interrupt the
human per invocation. Vaios: "add more tools related to coding and software development —
like Claude Code."

The bridge live-smoke lesson, systematized: **a peer's (and the agent's own fluid) power is
exactly the allow band** — dedicated NARROW tools with scope-tunable allow verdicts are how an
agent gets useful power safely. `shell`/Exec stays always-confirm; the new tools are the
disciplined subsets that escape the confirm tax inside operator-set scopes.

## Decisions (Vaios)

- v1 toolset: **grep + glob + edit_file + git_read trio + run_command** (all four groups).
- Write gating: **new `fs_write_allow` scope** — writes/edits inside it allow, outside confirm,
  `fs_deny` still hard-vetoes; `write_file` rides the same scope. (Rejected: keep
  always-confirm — confirm-heavy coding; allow-everywhere+denylist — too loose.)

## The five tools (10 → 15)

All follow the native tool protocol: isolated subprocess binary (`tools/<name>_main.cpp`), one
JSON line stdin → one JSON line stdout (`{"ok":bool,"result":{...}}`), self-describing
(`describe`), fail-closed on ANY input (typed `find()+is_*()` guards, never throws,
replace-handler dump). CMake target `hades-<name>` per binary.

### 1. `grep` (Claude Code: Grep) — `hades-grep`
Args: `pattern` (required, ECMAScript `std::regex`; invalid regex → ok:false), `path`
(default `"."` — file or directory), `ignore_case` (bool, default false), `context` (int lines
of context per match, default 0, max 5), `max_results` (default 100, max 500).
Behavior: recursive walk (skip `.git/`, `.hades/`, `build*/` dir names; skip binary files —
NUL byte in the first 8 KB; skip files > 4 MB), match line-by-line, output entries
`{file, line, text}` (+ context lines prefixed in `text` when requested). Result
`{matches:[...], truncated:bool}`; total output capped at 64 KB. Symlinked dirs NOT followed
(`recursive_directory_iterator` without `follow_directory_symlink`).
Capability: **FsRead** — the `path` arg goes through the EXISTING FsRead case verbatim
(canon_path, `fs_deny` hard-veto, `..` escape → confirm, `fs_read_allow` boundary match →
allow, else confirm/deny per `confirm_unscoped`).

### 2. `glob` (Claude Code: Glob) — `hades-glob`
Args: `pattern` (required — glob over the path RELATIVE to root: `*` = within a path segment,
`**` = across segments, `?` = one char; e.g. `**/*.cpp`, `src/**/test_*.py`), `path` (root,
default `"."`), `max_results` (default 200, max 1000).
Behavior: recursive walk (same skip list as grep, no symlink follow), match the relative path
against the pattern (glob→regex translation, anchored), sorted output, `{files:[...],
truncated:bool}`.
Capability: **FsRead** (same as grep — `path` arg).

### 3. `edit_file` (Claude Code: Edit) — `hades-edit-file`
Args: `path`, `old_string`, `new_string` (all required; `old_string` non-empty and ≠
`new_string`), `replace_all` (bool, default false).
Behavior: read the file (missing → ok:false); count occurrences of `old_string`; 0 →
ok:false "old_string not found"; >1 without `replace_all` → ok:false "matches N times — give
more surrounding context"; else replace (one or all) and write ATOMICALLY (temp file + rename,
save_skill precedent). Result `{path, replacements:int}`.
Capability: **FsWrite** — newly scope-tunable (below).

### 4. `git_read` — `hades-git-read`
One binary, read-only git introspection. Args: `op` (required: `"status"` | `"diff"` |
`"log"`), `path` (optional pathspec filter), `staged` (bool, diff only), `max_lines`
(status/diff: output line cap, default 200, max 1000; log: the `-n` entry count, default 30,
max 200).
Behavior: runs the `git` binary with a **FIXED argv per op** — never a shell:
- `status` → `git status --porcelain=v1 --branch`
- `diff`   → `git diff` (+`--staged` when set) (+`--`, path when given)
- `log`    → `git log --oneline --decorate -n <max 30..1000>` (+`--`, path when given)
**Flag-injection guard:** a `path` beginning with `-` is rejected (ok:false) AND every path is
preceded by a literal `--` separator. Unknown `op` → ok:false. Output (stdout+stderr) capped
64 KB, `{output, truncated, exit_code}`. Uses `run_subprocess` (core) — the tool binary links
`hades_core` (shell/ask_agent precedent).
Capability: **new `GitRead` → allow** (read-only by construction: fixed argv, no shell, no
write ops exposed).

### 5. `run_command` — `hades-run-command`
Args: `command` (required — a full command line, e.g. `ctest --test-dir build`).
Behavior: whitespace-split into argv and `execvp` DIRECTLY — **no shell, ever**: no pipes, no
redirection, no `$()`/globs/`cd` (need those → use `shell`, which stays confirm-gated).
Captures stdout+stderr (merged), caps 64 KB, result `{exit_code, output, truncated}`. Runs in
the agent's cwd. Subprocess wall-clock capped by the Tool block's per-tool `timeout_s`
(dev.hades ships `timeout_s = 600` for it — builds are slow; ToolRunner default stays 30 s).
Capability: **new `ExecScoped`** (below).

## Capability model extension

`CapabilityScope` (`include/hades/objective/capability_policy.h`) gains:

- **`fs_write_allow`** — path-prefix list, same canon_path + boundary-aware `allow_match` as
  `fs_read_allow`. FsWrite verdict becomes:
  1. `fs_deny` prefix → **hard-veto** (unchanged, still first);
  2. lexical `..` escape → **confirm** (new, mirrors FsRead);
  3. inside `fs_write_allow` → **allow**;
  4. else → **confirm** (unchanged default).
  Applies to `write_file` AND `edit_file` (both map to FsWrite). Empty/absent key → today's
  behavior byte-for-byte (backward compatible; the allow branch simply never fires).

- **`exec_allow`** — **COMMA-separated** list of command PREFIXES (comma because prefixes
  contain spaces — the one non-whitespace list in the manifest, documented loudly; entries
  trimmed). Example: `exec_allow = cmake --build build, ctest --test-dir build`.
  New `Capability::ExecScoped` (run_command) verdict:
  1. command contains a shell metacharacter (any of `` ; | & $ ` ( ) < > `` or newline) →
     **confirm** (fail-closed: prefix matching is only sound because neither the check nor the
     tool ever involves a shell — and the metachar check keeps the operator's mental model
     "this exact program runs" honest even against argv smuggling);
  2. command starts with an `exec_allow` prefix at a TOKEN BOUNDARY (prefix is the whole
     command, or is followed by a space — `ctest` matches `ctest --x`, never `ctest-evil`) →
     **allow**;
  3. else → **confirm**.
  `avoid_destructive` still backstops all args (a `run_command` of `rm -rf /` confirm-gates
  there too, and the no-shell execvp makes shell-syntax attacks inert).

New `capability_of` rows: `grep`→FsRead, `glob`→FsRead, `edit_file`→FsWrite,
`git_read`→GitRead, `run_command`→ExecScoped. Enum order: insert `GitRead, ExecScoped` before
`Unknown`.

## What this buys (incl. peers)

Inside operator scopes an agent codes fluidly: grep → fs_read → edit_file → run_command
(build/test) → git_read diff — zero confirms. Outside scope everything still confirms;
secrets still hard-veto. And because a peer's powers are exactly the receiver's unconfirmed
powers, a bridged worker with `fs_write_allow = ./workspace` + `exec_allow = ctest ...` can
now grep/edit/build/test FOR A PEER — the locked-down-worker deploy story becomes real.

## Ship (dev.hades + docs)

- 5 `Tool =` lines (run_command as a multi-line block carrying `timeout_s = 600`).
- `capability_policy` block gains `fs_write_allow = ./workspace` and an `exec_allow` example
  (commented or minimal real entries — decided at ship task with the lock tests).
- `prompts/soul.md`: a short "Coding tools" paragraph (grep/glob to explore, edit_file for
  surgical changes, run_command for allowlisted builds/tests, git_read for repo state; shell
  remains for everything else but asks the human).
- `docs/manifest-reference.md` (live operator doc): new tool rows, the two new scope keys, the
  updated capability verdict table (FsWrite scope-tunable now; GitRead/ExecScoped rows), the
  comma-list callout.
- CLAUDE.md: current-state (15 tools), targets line, capability section update, gotcha for the
  comma list.

## Testing

Per tool binary: describe schema; happy path; fail-closed paths (missing/typed-wrong args,
missing file, invalid regex, ambiguous old_string, flag-injection path, metachar command,
unknown op); output caps. Policy: fs_write_allow allow/confirm/deny matrix (+ `..` escape,
boundary no-sibling-match), exec_allow prefix boundary + metachar confirm + empty-scope
defaults, GitRead allow, new capability_of rows. Wiring: dev.hades lock tests (roster parses,
zero fatal warnings), per-tool timeout_s reaches run_command. Full suite + TSan lane green.

## Non-goals / v2 seams

No git write ops (commit/push stay via `shell`+confirm; a `git_commit` tool is a v2 decision) ·
no pipes/redirection in run_command (use `shell`) · no fs_read offset/limit paging · no
per-peer tool scoping (bridge v2) · grep/glob symlink-follow stays the documented lexical v1
gap (matches fs_read) · no PCRE (std::regex ECMAScript only).
