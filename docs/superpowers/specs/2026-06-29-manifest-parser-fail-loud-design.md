# Manifest parser — fail LOUD on multi-kv-on-one-line

**Date:** 2026-06-29
**Status:** approved (debt (2) from the architecture-honesty pass) → ready for plan

## Open question for the user (non-blocking — recommended default chosen)

- **Free-text values with embedded spaces + `=`.** The chosen detector flags a *second*
  ` <ident> =` inside a value as a packed kv pair. A genuine free-text value such as
  `note = use x = y here` would therefore be a **false positive**. The manifest grammar
  **does not support** free-text/quoted values today — every shipped value is an atomic,
  whitespace-free token (path, model id, URL, number, env-var name, bool). **Recommended
  default: accept this as a documented limitation** and keep the simple detector; if quoted
  free-text values are ever needed, add quoting to the grammar and make the detector
  quote-aware (out of scope here). Flagging this so the user can veto if a space-bearing
  value is already planned.

## Goal

Make the manifest parser **fail LOUD** when a single physical line packs **two or more
`key = value` pairs**, instead of silently corrupting the config. The fix must:

1. **Catch** the multi-kv-on-one-line mis-parse (`Memory { store=x top_n=3 }`,
   `Serve { host=1 port=2 webroot=w }`).
2. **Not break** the legitimate single-kv inline form (`Tool = fs { native = ./x }`),
   which is used throughout the shipped `dev.hades` and is valid.
3. Surface the failure where bad config already fails: a **specific parser warning** plus a
   hard **`MalConfig`** at the build/launch boundary, so the binary **refuses to start** on a
   corrupt manifest rather than running the wrong mission.

## The bug (concrete)

The parser (`src/config/manifest.cpp`) is **one-key-value-per-line**. `split_kv()` splits each
body line on the **first** `=`:

```cpp
auto eq = line.find('=');
b.kv[lower(trim(line.substr(0, eq)))] = trim(line.substr(eq + 1));   // first '=' wins
```

For the inline-brace path, the body between `{` and `}` is handed to `split_kv` as **one
string**. So a single line carrying two pairs mis-parses, silently:

| Manifest line | Intended | Actually parsed (silent) |
|---|---|---|
| `Memory { store=x top_n=3 }` | `store=x`, `top_n=3` | `store = "x top_n=3"` (`top_n` **lost**) |
| `Serve { host=1 port=2 webroot=w }` | three keys | `host = "1 port=2 webroot=w"` (`port`,`webroot` **lost**) |

No warning, no error — the downstream consumer (`resolve_serve_config`, `MemoryModule`) gets
garbage and runs anyway. This has bitten the project **three times** (`Memory`, then `Serve`,
caught only by a final whole-branch review each time). The current "workaround" — *only ever
use multi-line blocks* — is an unenforced footgun, not a fix. The exact same corruption also
occurs in a **multi-line** body if a single body line packs two pairs
(`store = x top_n = 3`); both routes funnel through `split_kv`, so fixing `split_kv` covers
both.

## Detection approach (chosen)

**In `split_kv`, after extracting the value (everything after the first `=`), scan that value
for a second `key=value` signature:** whitespace, then an identifier, then optional
whitespace, then `=`. Concretely the pattern is

```
[ \t]+  [A-Za-z_][A-Za-z0-9_]*  [ \t]*  =
```

Implemented as a small hand-rolled scanner `packs_second_kv(value)` (no `<regex>` — the file
already hand-rolls all parsing; keep it dependency-free and O(n)):

```cpp
// True if `value` (the substring AFTER a line's first '=') contains a second
// "<whitespace><ident><ws?>=" — the signature of a second key=value pair packed onto
// one physical line. The REQUIRED leading whitespace is the discriminator: it makes a
// '=' that lives *inside* a single token (URL query "?a=b", base64 padding "x==") never
// match, because those have no whitespace before the identifier-then-'='.
static bool packs_second_kv(const std::string& value);
```

The scan is applied to the **value only** (the first `key =` is the legitimate separator; we
hunt for a *second* one hiding in the value).

### Why this heuristic — and rejected alternatives

- **Rejected: count `=` signs in the line.** False-positives on every URL with a query string
  (`endpoint = https://api.ppq.ai/v1?model=x&k=y` → two extra `=`) and on base64 padding
  (`token = aGVsbG8=`). Counting cannot tell a packed kv pair from an `=` inside one value.
- **Rejected: split the value on whitespace and look for any token containing `=`.** Same
  base64/URL problem (`a=b&c=d` is one whitespace-delimited token containing `=`).
- **Chosen: require `whitespace + identifier + =`.** Real kv pairs in this grammar are
  **whitespace-separated and key-named**; an `=` *inside* a single value token (URL query,
  base64 pad, an `=`-bearing path) is **never** preceded by `whitespace + identifier`. This is
  the clean discriminator. It is also exactly the structure the parser itself would have
  needed to see a second key, so it matches intent.

### False-positive analysis (explicit)

| Value (after first `=`) | `packs_second_kv`? | Why |
|---|---|---|
| `./build/hades-fs-read` | **no** | no whitespace at all |
| `5` / `1.0` / `claude-haiku-4.5` | **no** | atomic token |
| `https://api.ppq.ai/v1?model=x&k=y` | **no** | `=` inside a contiguous token; no leading-ws+ident |
| `aGVsbG8=` (base64, incl. `==` padding) | **no** | trailing `=`, no leading-ws+ident |
| `/tmp/a=b` | **no** | no whitespace |
| `x top_n=3` (the bug) | **yes** | ` top_n=` = ws+ident+`=` |
| `1 port=2 webroot=w` (the bug) | **yes** | ` port=` matches |
| `use x = y here` (hypothetical free text) | **yes (false positive)** | ` x =` matches — see Open question |

**Known, accepted limitations (documented, not blocking):**

- **Free-text value with internal spaces + `=`** → false positive. Out-of-grammar today (see
  Open question). The mitigation (quoting) is out of scope.
- **Empty first value immediately followed by a second key**, e.g. `host= port=2`. After
  `trim`, the value is `port=2` with the leading whitespace stripped, so the scanner does **not**
  fire. This is a deliberate tradeoff favoring **zero false positives** over catching a
  pathological edge: every realistic packed line (and all three real incidents) has a
  **non-empty** first value, which guarantees whitespace before the second key. The edge is no
  worse than today (silent) and the common form **is** caught. Documented, not fixed.
- **Header lines** (`Section = name`) are not scanned — they carry no kv body (kv only lives
  inside `{ }`). A malformed header is a separate class; out of scope.

## Severity & where the loud failure surfaces (decision)

**Two-layer, by design — the parser stays pure; the boundary fails loud.**

1. **Parser layer (non-throwing, records a SPECIFIC warning).** `parse_manifest` keeps its
   documented contract: *pure; never throws; collects warnings* (`include/hades/config.h`).
   `split_kv` pushes a warning that **starts with a stable, exported prefix constant** so the
   class is machine-classifiable without brittle full-string matching:

   ```cpp
   // include/hades/config.h
   inline constexpr std::string_view kMultiKvWarning =
       "multiple key=value pairs on one line";

   // pushed by split_kv:
   //   "multiple key=value pairs on one line (only one per line; use a multi-line
   //    { } block): Memory { store=x top_n=3 }"
   ```

   The best-effort first kv is **still stored** (so the Block stays structurally non-empty for
   any non-enforcing reader/test); the warning — not a missing key — is what stops the launch.

   A pure classifier (also in the config layer, **no `MalConfig` dependency** so `config.h`
   stays decoupled from `launcher.h`):

   ```cpp
   std::vector<std::string> fatal_warnings(const Manifest& m);   // warnings starting with kMultiKvWarning
   ```

2. **Boundary layer (fails LOUD with `MalConfig`).** A new
   `enforce_manifest(const Manifest&)` (in `app/agent_wiring.{h,cpp}`, where `MalConfig` and the
   other config-validation throws already live) promotes any fatal warnings to a hard error:

   ```cpp
   void enforce_manifest(const Manifest& m) {
     auto fatal = fatal_warnings(m);
     if (fatal.empty()) return;
     std::string msg = "corrupt manifest — multiple key=value pairs packed on one physical "
                       "line (split each onto its own line in a { } block):";
     for (const auto& w : fatal) msg += "\n  - " + w;
     throw MalConfig(msg);
   }
   ```

   It is called **at the top of `build_agent(bb, const Manifest&)`** — the single chokepoint
   every real run (and `hades-scope`, future front-ends, the test Manifest path) funnels through
   — so the library invariant "an agent never builds from a corrupt manifest" holds for **all**
   callers (validate-at-the-boundary). `app/hades_main.cpp` additionally **prints all
   `manifest.warnings` to stderr** right after parse and calls `enforce_manifest` **before**
   `resolve_api_key`, so the *corrupt-manifest* error is reported first (not a downstream
   api-key symptom). `MalConfig` is already caught by `main`'s handler → prints
   `hades: configuration error: …` and exits **1**.

**Why warning-then-promote rather than throw in the parser:** the parser is consumed by pure,
non-failing contexts (tests, `hades-scope` replay, the soft-warning path for genuinely
recoverable typos). Keeping it non-throwing preserves that contract and keeps all
construction-time failures funneled through the one boundary that already owns `MalConfig`.
Defense-in-depth (main enforces early **and** `build_agent` enforces as a library invariant) is
intentional and matches the project's "validate at system boundaries / fail fast" rule; the two
calls share one `enforce_manifest`, so there is no logic duplication.

## Error message wording (final)

- **Parser warning (per corrupt line):**
  `multiple key=value pairs on one line (only one per line; use a multi-line { } block): <original line>`
- **Boundary `MalConfig` (aggregate):**
  ```
  corrupt manifest — multiple key=value pairs packed on one physical line (split each onto its own line in a { } block):
    - multiple key=value pairs on one line (...): Memory { store=x top_n=3 }
  ```

## Testing strategy (TDD, GoogleTest)

**Parser + classifier — `tests/test_manifest.cpp` (includes only `config.h`):**

- **Inline multi-kv (Memory) → fatal warning.** `Memory { store=x top_n=3 }` →
  `m.warnings` non-empty **and** `fatal_warnings(m)` non-empty; the fatal entry **starts with**
  `kMultiKvWarning`.
- **Inline multi-kv (Serve) → fatal warning.** `Serve { host=1 port=2 webroot=w }`.
- **Multi-line body line that packs two pairs → fatal warning.** `Memory {\n store = x top_n = 3\n}`
  (proves the non-inline `split_kv` route is covered too).
- **Legit single-kv inline → NO warning (regression).** `Tool = fs { native = ./tools/hades-fs-read }`
  → `m.warnings` empty, `fatal_warnings(m)` empty, `kv.at("native") == "./tools/hades-fs-read"`.
- **False-positive guards.** A `Session` block whose value is a URL with a query string
  (`endpoint = https://api.ppq.ai/v1?model=x&k=y`) and one with base64 padding
  (`token = aGVsbG8=`) → `fatal_warnings(m)` **empty**.
- **Lock test (dev.hades stays clean).** `parse_manifest(<DEV_MANIFEST file>)` →
  `m.warnings` empty **and** `fatal_warnings(m)` empty. (`DEV_MANIFEST` is already a compile
  definition on `hades_tests`, used by `test_serve_config`.)

**Boundary — `tests/test_pantler_wiring.cpp` (already links `build_agent` + `MalConfig`):**

- **Corrupt single-line manifest → `MalConfig`.** A full-roster manifest with
  `Memory { store=x top_n=5 }` (single line, two pairs) →
  `EXPECT_THROW(build_agent(bb, parse_manifest(m)), MalConfig)`.
- **Positive control.** The clean full roster (multi-line `Memory`) does **not** throw for this
  reason (the existing `FullRosterBuildsAllModules` already covers the clean path).

All **119** existing tests stay green: the legit inline `Tool`/`Arbiter`/`Objective` blocks in
`dev.hades` are single-kv, and the shipped manifest is fully multi-line for `Session`/`Memory`/
`Serve`, so `fatal_warnings(dev)` is empty.

## Out of scope (follow-ups)

- **Quoted / free-text values** (and a quote-aware detector) — would remove the one documented
  false positive; needs a grammar extension.
- **Typed warning severities** on `Manifest` (a `std::vector<Severity>` parallel to `warnings`)
  — we use a stable prefix + classifier instead, to avoid restructuring the public struct.
- **Promoting other malformed-line classes** ("bad config line", "unclosed `{` block") to fatal
  — they stay soft warnings (now printed by `main`); could be promoted later behind the same
  `fatal_warnings` seam.
- **The empty-first-value edge** (`host= port=2`) — deliberately uncaught (see analysis).
- **Header-line multi-kv** (`Tool = fs native = x`, no braces) — separate class, not addressed.

## MOOS-IvP framing

The manifest **is** the `.moos` mission file; its `{ }` blocks are **ProcessConfig**. In
MOOS-IvP, an app that cannot cleanly parse its `ProcessConfig` calls `reportConfigWarning` and a
mis-configured helm/app **refuses to launch (MALCONFIG)** rather than running with garbage
parameters — you do not want a vehicle to *silently fly the wrong mission* because one config
line was mangled. hades today does exactly that: a mis-packed line silently changes the mission
(wrong memory store, wrong bind host) and the agent runs anyway. This change brings the harness
in line with the MOOS contract: **a corrupt mission file fails at launch**, loudly, naming the
offending line.
