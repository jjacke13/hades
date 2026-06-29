# Manifest Parser Fail-Loud Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Make the one-key-value-per-line manifest parser **fail LOUD** when a single physical line packs two or more `key = value` pairs (`Memory { store=x top_n=3 }`), instead of silently corrupting config — while leaving the legitimate single-kv inline form (`Tool = fs { native = ./x }`) untouched.

**Architecture:** Two layers. (1) The parser stays **pure/non-throwing**: `split_kv` detects a packed second kv via `packs_second_kv(value)` (a hand-rolled `whitespace+identifier+'='` scanner) and records a **specific** warning prefixed with the exported constant `kMultiKvWarning`; a pure `fatal_warnings(Manifest)` classifier returns those. (2) A new `enforce_manifest(Manifest)` at the build/launch boundary (`app/agent_wiring.cpp`, where `MalConfig` already lives) promotes any fatal warnings to a hard `MalConfig`, so the binary refuses to start on a corrupt manifest. Detection sits in `split_kv`, the single chokepoint for **both** the inline-brace body and multi-line body lines.

**Tech Stack:** C++20 · CMake + Ninja · Nix dev shell · GoogleTest.

## Global Constraints

- **C++20**, g++ 15.2. **Every build/test command runs inside `nix develop`.**
- **TDD:** RED → GREEN → COMMIT per task. Commit `<type>: <desc>`, **no attribution footer**.
- **Parser contract preserved:** `parse_manifest` stays *pure; never throws; collects warnings*. Detection only **adds** a warning; it still stores the best-effort first kv (Block stays non-empty for non-enforcing readers).
- **No new dependencies:** hand-rolled scanner, no `<regex>`. Match the existing `manifest.cpp` style (static helpers, `trim`/`lower`).
- **Zero false positives on shipped values:** URLs with query strings and base64 padding must NOT be flagged. The shipped `dev.hades` must stay warning-clean (lock test).
- **`config.h` stays decoupled from `launcher.h`:** the throwing `enforce_manifest` lives in `agent_wiring` (which already includes `launcher.h` for `MalConfig`), NOT in the config layer.

---

## File Structure

```
include/hades/config.h          T1 (modify)  + kMultiKvWarning constant, + fatal_warnings() decl
src/config/manifest.cpp         T1 (modify)  + packs_second_kv(), split_kv() warns, + fatal_warnings()
tests/test_manifest.cpp         T1 (extend)  detection + false-positive guards + dev.hades lock test
app/agent_wiring.h              T2 (modify)  + enforce_manifest() decl
app/agent_wiring.cpp            T2 (modify)  enforce_manifest() impl; call at top of build_agent(Manifest)
app/hades_main.cpp              T2 (modify)  print warnings + enforce_manifest() before resolve_api_key
tests/test_pantler_wiring.cpp   T2 (extend)  corrupt manifest -> MalConfig (clean control already present)
```

(`DEV_MANIFEST` is already a compile definition on `hades_tests` — `CMakeLists.txt:110` — so no CMake change is needed.)

---

## Task 1: Parser detects packed multi-kv lines and records a fatal-class warning

**Files:** Modify `include/hades/config.h`, `src/config/manifest.cpp`, `tests/test_manifest.cpp`.

**Interfaces — Produces:** `hades::kMultiKvWarning` (`constexpr std::string_view`), `std::vector<std::string> hades::fatal_warnings(const Manifest&)`. **Consumes:** existing `Manifest`/`Block`, `split_kv`, `trim`.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_manifest.cpp` (it already `#include "hades/config.h"` and `using namespace hades;`):

```cpp
TEST(Manifest, InlineMultiKvMemoryProducesFatalWarning) {
  Manifest m = parse_manifest("Memory { store=x top_n=3 }\n");
  EXPECT_FALSE(m.warnings.empty());
  auto fatal = fatal_warnings(m);
  ASSERT_FALSE(fatal.empty());
  EXPECT_TRUE(fatal.front().starts_with(kMultiKvWarning));   // stable prefix
}
TEST(Manifest, InlineMultiKvServeProducesFatalWarning) {
  Manifest m = parse_manifest("Serve { host=1 port=2 webroot=w }\n");
  EXPECT_FALSE(fatal_warnings(m).empty());
}
TEST(Manifest, MultiLineBodyLinePackingTwoPairsIsCaught) {
  // The corruption also happens inside a multi-line block if a body line packs two pairs.
  Manifest m = parse_manifest("Memory {\n  store = x top_n = 3\n}\n");
  EXPECT_FALSE(fatal_warnings(m).empty());
}
TEST(Manifest, LegitSingleKvInlineHasNoWarning) {
  Manifest m = parse_manifest("Tool = fs { native = ./tools/hades-fs-read }\n");
  EXPECT_TRUE(m.warnings.empty());
  EXPECT_TRUE(fatal_warnings(m).empty());
  auto tools = m.of("Tool");
  ASSERT_EQ(tools.size(), 1u);
  EXPECT_EQ(tools[0].kv.at("native"), "./tools/hades-fs-read");
}
TEST(Manifest, UrlValueWithQueryStringNotFlagged) {
  Manifest m = parse_manifest(
      "Session {\n  endpoint = https://api.ppq.ai/v1?model=x&k=y\n}\n");
  EXPECT_TRUE(fatal_warnings(m).empty());   // '=' inside one token, no ws+ident
  EXPECT_EQ(m.session()->kv.at("endpoint"), "https://api.ppq.ai/v1?model=x&k=y");
}
TEST(Manifest, Base64ValueWithPaddingNotFlagged) {
  Manifest m = parse_manifest("Session {\n  token = aGVsbG8=\n}\n");
  EXPECT_TRUE(fatal_warnings(m).empty());   // trailing '=' padding, no ws+ident
}
TEST(Manifest, ShippedDevManifestHasNoWarningsOrFatals) {
  std::ifstream f(DEV_MANIFEST);
  std::stringstream s; s << f.rdbuf();
  Manifest m = parse_manifest(s.str());
  EXPECT_TRUE(m.warnings.empty());          // dev.hades is fully multi-line / single-kv inline
  EXPECT_TRUE(fatal_warnings(m).empty());
}
```

Add the two includes the lock test needs to the top of `tests/test_manifest.cpp` (after the existing includes):
```cpp
#include <fstream>
#include <sstream>
```

- [ ] **Step 2: Run, expect FAIL** — `nix develop --command cmake --build build` → compile error: `fatal_warnings` and `kMultiKvWarning` are undeclared.

- [ ] **Step 3: Implement.**

`include/hades/config.h` — add `#include <string_view>` to the includes, then add inside `namespace hades` (after the `Manifest` struct, before `parse_manifest`):
```cpp
// Stable prefix for the "two key=value pairs packed on one physical line" warning.
// fatal_warnings() classifies by this prefix; the boundary (enforce_manifest) promotes
// matching warnings to a hard MalConfig so the binary refuses to start on a corrupt manifest.
inline constexpr std::string_view kMultiKvWarning = "multiple key=value pairs on one line";
```
and declare (after `parse_manifest`):
```cpp
// Subset of m.warnings that are the fatal multi-kv class (start with kMultiKvWarning).
// Pure; no MalConfig dependency (keeps config.h decoupled from launcher.h).
std::vector<std::string> fatal_warnings(const Manifest& m);
```

`src/config/manifest.cpp` — add a hand-rolled scanner above `split_kv`:
```cpp
static bool is_ident_start(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
static bool is_ident(char c)       { return std::isalnum((unsigned char)c) || c == '_'; }

// True if `value` (the substring AFTER a line's first '=') packs a second
// "<whitespace><ident><ws?>=" pair. The REQUIRED leading whitespace is the discriminator:
// a '=' inside a single token (URL query "?a=b", base64 padding "x==") is never preceded
// by whitespace+identifier, so those never match — only a genuine second key does.
static bool packs_second_kv(const std::string& value) {
  for (std::size_t i = 0; i + 1 < value.size(); ++i) {
    if (!std::isspace((unsigned char)value[i])) continue;          // need whitespace first
    std::size_t j = i;
    while (j < value.size() && std::isspace((unsigned char)value[j])) ++j;   // skip ws
    if (j >= value.size() || !is_ident_start(value[j])) continue;  // need an identifier
    std::size_t k = j + 1;
    while (k < value.size() && is_ident(value[k])) ++k;            // consume identifier
    while (k < value.size() && std::isspace((unsigned char)value[k])) ++k;   // optional ws
    if (k < value.size() && value[k] == '=') return true;         // identifier then '='
  }
  return false;
}
```
and have `split_kv` record the warning (store the best-effort first kv as before):
```cpp
static void split_kv(const std::string& line, Block& b, Manifest& m) {
  auto eq = line.find('=');
  if (eq == std::string::npos) {
    m.warnings.push_back("bad config line: " + line);
    return;
  }
  std::string value = trim(line.substr(eq + 1));
  if (packs_second_kv(value))
    m.warnings.push_back(std::string(kMultiKvWarning) +
                         " (only one per line; use a multi-line { } block): " + line);
  b.kv[lower(trim(line.substr(0, eq)))] = value;
}
```
and add the classifier near the other `Manifest` members (C++20 `std::string::starts_with(string_view)`):
```cpp
std::vector<std::string> fatal_warnings(const Manifest& m) {
  std::vector<std::string> out;
  for (const auto& w : m.warnings)
    if (w.starts_with(kMultiKvWarning)) out.push_back(w);
  return out;
}
```
(`<string_view>` is pulled in via `config.h`; `<cctype>` is already included.)

- [ ] **Step 4: Build + test** — `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R Manifest` → PASS (prior `Manifest.*` tests + 7 new). The existing `MalformedLineProducesWarning` ("no equals") and `StrayCloseBrace...` stay soft (not in `fatal_warnings`).

- [ ] **Step 5: Commit**
```bash
git add include/hades/config.h src/config/manifest.cpp tests/test_manifest.cpp
git commit -m "feat: manifest parser flags packed multi-kv lines (fatal-class warning)"
```

---

## Task 2: Fail LOUD at the build/launch boundary

**Files:** Modify `app/agent_wiring.h`, `app/agent_wiring.cpp`, `app/hades_main.cpp`, `tests/test_pantler_wiring.cpp`.

**Interfaces:**
- Consumes: `fatal_warnings(Manifest)` (T1), `MalConfig` (`hades/launcher.h`).
- Produces: `void hades::enforce_manifest(const Manifest&)` — throws `MalConfig` listing the fatal warnings; no-op when there are none. Called at the top of `build_agent(bb, const Manifest&)` and (early) by `main`.

- [ ] **Step 1: Write the failing test** — append to `tests/test_pantler_wiring.cpp` (it already has `MalConfig`, `setenv`, and `build_agent`). Use a **self-contained** corrupt manifest (do NOT derive from `kFullRoster`, whose `Memory`/`Arbiter` blocks are already clean — multi-line `Memory` + single-kv inline `Arbiter` — so they need no edits and the existing `FullRosterBuildsAllModules` / `RosterOmittingServeYieldsNullServe` / `MisorderedRosterStillBuilds` tests stay green):

```cpp
TEST(PantlerWiring, CorruptInlineMultiKvManifestThrowsMalConfig) {
  setenv("HADES_TEST_KEY", "x", 1);
  // A full roster, but the Memory block packs two kv pairs on ONE line (the silent-corruption
  // footgun). enforce_manifest() must promote the parser's fatal warning to MalConfig.
  const char* corrupt = R"(
Session
{
  endpoint    = https://example.invalid/v1
  model       = test-model
  api_key_env = HADES_TEST_KEY
}
Module = llm
Module = arbiter
Memory { store=x top_n=5 }
)";
  Blackboard bb;
  EXPECT_THROW(build_agent(bb, parse_manifest(corrupt)), MalConfig);
}
```

> Note: `kFullRoster` and `misordered` in `tests/test_pantler_wiring.cpp` already use a
> **multi-line** `Memory` block and a **single-kv** inline `Arbiter { policy = v1 }`, so the
> T1 detector does not flag them — no edits to the existing constants are required, and the
> existing presence/order tests stay green.

- [ ] **Step 2: Run, expect FAIL** — `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R PantlerWiring`: the new `CorruptInlineMultiKvManifestThrowsMalConfig` test fails — `build_agent` does not yet enforce, so it does **not** throw (it builds the agent from the corrupted Memory block).

- [ ] **Step 3: Implement.**

`app/agent_wiring.h` — declare after the second `build_agent` overload (before the closing `}  // namespace hades`):
```cpp
// Promote the fatal multi-kv parser warnings (see kMultiKvWarning) to a hard MalConfig so
// no agent is ever built from a corrupt manifest. No-op when there are none. Called at the
// top of build_agent(bb, Manifest) as a library invariant, and early by the binary.
void enforce_manifest(const Manifest& m);
```

`app/agent_wiring.cpp` — add the implementation (e.g. just below the anonymous-namespace block, in `namespace hades`), and call it first thing in the Manifest overload:
```cpp
void enforce_manifest(const Manifest& m) {
  auto fatal = fatal_warnings(m);
  if (fatal.empty()) return;
  std::string msg = "corrupt manifest — multiple key=value pairs packed on one physical line "
                    "(split each onto its own line in a { } block):";
  for (const auto& w : fatal) msg += "\n  - " + w;
  throw MalConfig(msg);
}
```
In `Agent build_agent(Blackboard& bb, const Manifest& m) {` insert as the **first** statement:
```cpp
  enforce_manifest(m);   // refuse to build from a manifest with packed multi-kv lines
```

`app/hades_main.cpp` — in `main`, right after `const Manifest manifest = parse_manifest(read_file(argv[1]));`, print any warnings and enforce **before** `resolve_api_key` so the corrupt-manifest error is reported first:
```cpp
    for (const auto& w : manifest.warnings)
      std::cerr << "hades: manifest warning: " << w << "\n";
    enforce_manifest(manifest);   // throws MalConfig on packed multi-kv lines (caught below)
```
(`app/agent_wiring.h` and `hades/launcher.h` are already included in `hades_main.cpp`; the `catch (const MalConfig&)` handler already prints `hades: configuration error: …` and returns 1.)

- [ ] **Step 4: Build + full suite** — `nix develop --command cmake -S . -B build -G Ninja && nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure`. Expected: **all green** — the new throw test passes; `FullRosterBuildsAllModules` / `RosterOmittingServeYieldsNullServe` / `MisorderedRosterStillBuilds` stay green (their `kFullRoster`/`misordered` blocks are already multi-line / single-kv inline, so the detector does not flag them); `test_serve_config` `ShippedDevManifestResolvesCleanly` and the new `ShippedDevManifestHasNoWarningsOrFatals` confirm `dev.hades` is clean; 119 prior + new tests.

- [ ] **Step 5: Commit**
```bash
git add app/agent_wiring.h app/agent_wiring.cpp app/hades_main.cpp tests/test_pantler_wiring.cpp
git commit -m "feat: refuse to start on a corrupt manifest (promote packed multi-kv to MalConfig)"
```

---

## Self-Review (against the spec)

- **Spec coverage:** detector `packs_second_kv` + `kMultiKvWarning` + `fatal_warnings` (T1); `enforce_manifest` at the `build_agent`/`main` boundary throwing `MalConfig` (T2). Parser stays non-throwing; the boundary fails loud.
- **Catches the bug:** `Memory { store=x top_n=3 }`, `Serve { host=1 port=2 webroot=w }`, and multi-line `store = x top_n = 3` all flagged → `MalConfig` at launch.
- **Preserves the legit form:** `Tool = fs { native = ./x }` parses to one kv with **no** warning (regression test). Shipped `dev.hades` stays warning-clean (lock test).
- **False positives controlled:** URL-with-query and base64-padding values are not flagged (tests); the free-text-with-spaces case is the documented, accepted limitation (spec Open question).
- **Decoupling honored:** throwing `enforce_manifest` lives in `agent_wiring` (has `MalConfig`); `config.h` gains only a pure constant + pure classifier.
- **Defense-in-depth, single logic:** `main` enforces early (best error ordering + prints warnings); `build_agent` enforces as a library invariant; both share `enforce_manifest`.

## Verification

1. `nix develop --command cmake -S . -B build -G Ninja && nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure` → all green.
2. Manual loud-failure check: copy `manifests/dev.hades`, collapse its `Memory` block to `Memory { store=x top_n=5 }`, run `nix develop --command ./build/hades <copy>` → exits 1 with `hades: configuration error: corrupt manifest — multiple key=value pairs packed on one physical line …` (and a `hades: manifest warning: …` line naming the offending line). The unmodified `dev.hades` still starts normally.
