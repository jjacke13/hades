# Archival Memory Supersession + Blended Ranking Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Archival memory stops surfacing stale facts — a newer record on the same `topic` supersedes older ones — and retrieval ranks by relevance blended with recency and reinforcement instead of relevance alone.

**Architecture:** Two changes behind the existing pure-function seam. `MemoryRecord` gains an optional `topic`; `rank_memories` (a) admits only relevance>0 records, (b) drops all but the newest record per non-empty topic, (c) scores survivors with a weighted blend. The store stays append-only; `save_memory` gains an optional `topic` arg. No new manifest keys.

**Tech Stack:** C++20, CMake+Ninja in `nix develop`, nlohmann_json, GoogleTest.

**Spec:** `docs/superpowers/specs/2026-08-22-memory-supersession-design.md` (approved, committed `dadc534` on branch `feat/memory-supersession`).

## Global Constraints

- **Every build/test command runs inside `nix develop`.** ASan lane: `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure`. TSan lane: same with `build-tsan`. Baseline **798/798 both lanes**. NEVER reconfigure `build/` (its CMake cache carries `-fsanitize=address,undefined`; reconfiguring silently drops the sanitizer lane).
- Branch `feat/memory-supersession`. Commit style `<type>: <desc>` — NO attribution footer, NO Co-Authored-By.
- Never stage: `manifests/dev.local.hades`, `manifests/pi.hades`, `manifests/dev2.hades`, `memory/`, `skills/greek-greeting/`, `skills/ponytail/`, `build*/`.
- **BACKWARD-COMPAT IS A HARD GATE:** the existing assertions in `tests/test_memory_rank.cpp` and `tests/test_memory_store.cpp` must pass **UNCHANGED**. If an existing assertion fails, STOP and report — do not edit the test to match new behavior. (Adding NEW tests to those files is expected and fine.)
- Exact values (spec, verbatim): `kRelevanceWeight = 1.0`, `kRecencyWeight = 0.3`, `kReinforcementWeight = 0.2`, `kRecencyHalfLifeDays = 30.0`; relevance = `matched/total` query tokens; recency = `0.5^(age_days/half_life)`; reinforcement = `1 - 1/(1+bucket_size)`; sort = score desc, ts desc, text asc.
- Supersession applies ONLY to records with a **non-empty** topic; untopiced records never suppress anything.
- Recency/reinforcement are boosters among relevance>0 records — never admission criteria.
- `MemoryRecord` must remain an aggregate so `{"text", ts}` brace-init still compiles.
- Pump-thread handlers never throw; all bus/JSON reads stay guarded.

---

## File Structure

```
include/hades/memory/record.h   T1  + std::string topic
include/hades/memory/rank.h     T1  + weight constants, 4-arg overload
src/apps/memory/memory.cpp      T1  ranker rewrite + tolerant topic load
tests/test_memory_rank.cpp      T1  (append new tests; existing ones untouched)
tests/test_memory_store.cpp     T1  (append)
tools/save_memory_main.cpp      T2  optional topic arg + schema
tests/test_save_memory_tool.cpp T2  (append)
prompts/soul.md                 T3  topic guidance
docs/manifest-reference.md      T3  save_memory schema + behavior
CLAUDE.md                       T3  feature record
```

---

## Task 1: Record + store + blended ranker with supersession

**Files:**
- Modify: `include/hades/memory/record.h`, `include/hades/memory/rank.h`, `src/apps/memory/memory.cpp` (`rank_memories` ~lines 62-80, `load_memories` ~lines 87-101)
- Test: `tests/test_memory_rank.cpp`, `tests/test_memory_store.cpp` (append to both)

**Interfaces:**
- Produces: `MemoryRecord{text, ts, topic}`; `rank_memories(all, query, top_n)` (unchanged signature, clock-backed) and `rank_memories(all, query, top_n, now)` (pure, deterministic); weight constants in `rank.h`.
- Consumes: nothing new. Task 2's tool writes the `topic` field this task reads.

- [ ] **Step 1: Write the failing tests** — APPEND to `tests/test_memory_rank.cpp` (leave every existing test exactly as-is):

```cpp
// ── supersession + blended ranking (2026-08-22) ────────────────────────────────
// All new tests pin `now` via the 4-arg overload so scoring is deterministic.
namespace {
constexpr double kDay = 86400.0;
constexpr double kNow = 1'000'000'000.0;   // fixed reference "now" for these tests
}  // namespace

TEST(MemoryRank, NewerTopicRecordSupersedesOlder) {
  std::vector<MemoryRecord> all = {
      {"prefers aisle seats", kNow - 10 * kDay, "seat-pref"},
      {"prefers window seats", kNow - 1 * kDay, "seat-pref"},
  };
  auto top = rank_memories(all, "prefers seats", 5, kNow);
  ASSERT_EQ(top.size(), 1u);                       // stale value suppressed entirely
  EXPECT_EQ(top[0].text, "prefers window seats");
}

TEST(MemoryRank, UntopicedRecordsNeverSupersedeEachOther) {
  std::vector<MemoryRecord> all = {
      {"cat one", kNow - 10 * kDay},               // topic defaults to ""
      {"cat two", kNow - 1 * kDay},
  };
  auto top = rank_memories(all, "cat", 5, kNow);
  EXPECT_EQ(top.size(), 2u);                       // both kept: "" is not a bucket
}

TEST(MemoryRank, DistinctTopicsCoexist) {
  std::vector<MemoryRecord> all = {
      {"prefers window seats", kNow - 1 * kDay, "seat-pref"},
      {"prefers vegetarian meals", kNow - 2 * kDay, "meal-pref"},
  };
  auto top = rank_memories(all, "prefers", 5, kNow);
  EXPECT_EQ(top.size(), 2u);
}

TEST(MemoryRank, RelevanceStillDominatesRecency) {
  // Old but fully-matching beats brand-new but weakly-matching.
  std::vector<MemoryRecord> all = {
      {"alpha beta", 0.0},                          // rel 2/2 = 1.0, recency ~0
      {"alpha gamma delta", kNow},                  // rel 1/2 = 0.5, recency 1.0
  };
  auto top = rank_memories(all, "alpha beta", 5, kNow);
  ASSERT_EQ(top.size(), 2u);
  EXPECT_EQ(top[0].text, "alpha beta");             // 1.0 vs 0.5+0.3 = 0.8
}

TEST(MemoryRank, RecencyReordersEqualRelevance) {
  std::vector<MemoryRecord> all = {
      {"cat alpha", kNow - 365 * kDay},
      {"cat beta", kNow - 1 * kDay},
  };
  auto top = rank_memories(all, "cat", 5, kNow);
  ASSERT_EQ(top.size(), 2u);
  EXPECT_EQ(top[0].text, "cat beta");               // same relevance, newer wins
}

TEST(MemoryRank, ReinforcementLiftsRestatedTopic) {
  // Same relevance and same ts for the two survivors; the restated topic wins on
  // reinforcement (bucket of 3 -> 0.75 vs untopiced 0.5).
  const double t = kNow - 2 * kDay;
  std::vector<MemoryRecord> all = {
      {"cat restated", t, "cat-topic"},
      {"cat restated older", t - kDay, "cat-topic"},
      {"cat restated oldest", t - 2 * kDay, "cat-topic"},
      {"cat standalone", t},
  };
  auto top = rank_memories(all, "cat", 5, kNow);
  ASSERT_EQ(top.size(), 2u);                        // 3 topic records collapse to 1
  EXPECT_EQ(top[0].text, "cat restated");
}

TEST(MemoryRank, TieBreakIsFullyDeterministic) {
  // Identical relevance, ts and topic-state -> text ascending, stable across runs.
  std::vector<MemoryRecord> all = {
      {"cat zulu", kNow}, {"cat alpha", kNow}, {"cat mike", kNow}};
  auto a = rank_memories(all, "cat", 5, kNow);
  auto b = rank_memories(all, "cat", 5, kNow);
  ASSERT_EQ(a.size(), 3u);
  EXPECT_EQ(a[0].text, "cat alpha");
  EXPECT_EQ(a[1].text, "cat mike");
  EXPECT_EQ(a[2].text, "cat zulu");
  for (std::size_t i = 0; i < a.size(); ++i) EXPECT_EQ(a[i].text, b[i].text);
}

TEST(MemoryRank, SupersessionRespectsTopNCap) {
  std::vector<MemoryRecord> all = {
      {"cat a", kNow - 3 * kDay, "t1"}, {"cat a old", kNow - 9 * kDay, "t1"},
      {"cat b", kNow - 2 * kDay, "t2"}, {"cat c", kNow - 1 * kDay, "t3"}};
  auto top = rank_memories(all, "cat", 2, kNow);
  ASSERT_EQ(top.size(), 2u);                        // cap applied AFTER supersession
  EXPECT_EQ(top[0].text, "cat c");
  EXPECT_EQ(top[1].text, "cat b");
}

TEST(MemoryRank, ThreeArgOverloadStillWorks) {
  // Clock-backed overload: old records, so recency ~0 and relevance decides.
  std::vector<MemoryRecord> all = {{"cat sat", 1.0}, {"dog ran", 2.0}};
  auto top = rank_memories(all, "cat", 5);
  ASSERT_EQ(top.size(), 1u);
  EXPECT_EQ(top[0].text, "cat sat");
}
```

APPEND to `tests/test_memory_store.cpp`:

```cpp
TEST(MemoryStore, TopicRoundTripsAndDefaultsEmpty) {
  const std::string path = ::testing::TempDir() + "/store_topic.jsonl";
  {
    std::ofstream f(path);
    f << R"({"text":"with topic","ts":1.0,"topic":"seat-pref"})" << "\n";
    f << R"({"text":"no topic","ts":2.0})" << "\n";              // legacy line
    f << R"({"text":"bad topic","ts":3.0,"topic":42})" << "\n";  // non-string -> ""
  }
  auto v = load_memories(path);
  ASSERT_EQ(v.size(), 3u);
  EXPECT_EQ(v[0].topic, "seat-pref");
  EXPECT_EQ(v[1].topic, "");
  EXPECT_EQ(v[2].topic, "");        // tolerated, not thrown
  EXPECT_EQ(v[2].text, "bad topic");
}
```

- [ ] **Step 2: Run to verify they fail.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R "MemoryRank|MemoryStore"`
Expected: compile FAIL (no `topic` member, no 4-arg overload).

- [ ] **Step 3: Implement.**

`include/hades/memory/record.h`:

```cpp
// include/hades/memory/record.h — one persisted memory: text, save timestamp, optional topic
#pragma once
#include <string>
namespace hades {
// `topic` is an OPTIONAL supersession key: among records sharing the same non-empty topic,
// only the newest (greatest ts) is retrieval-eligible, so a corrected fact replaces the one
// it corrects instead of accumulating beside it. Empty topic (the default, and every record
// written before 2026-08-22) means "standalone" — never superseded, never superseding.
// Aggregate on purpose: `MemoryRecord{"text", ts}` must keep compiling.
struct MemoryRecord {
  std::string text;
  double ts = 0.0;
  std::string topic;
};
}  // namespace hades
```

`include/hades/memory/rank.h`:

```cpp
// include/hades/memory/rank.h — archival retrieval: supersession + blended scoring
//
// Two stages. (1) ADMISSION: a record must share at least one token with the query, and
// among records sharing a non-empty `topic` only the newest survives (supersession — the
// stale value stops surfacing). (2) SCORING: survivors are ordered by a weighted blend of
// relevance, recency and reinforcement. Recency/reinforcement only reorder RELEVANT records;
// they never admit an irrelevant one, or a merely-recent fact would flood the memory block.
// Pure: no files, no Blackboard, no clock (the 4-arg overload takes `now`) — this is the
// seam a v2 embeddings scorer slots in behind.
#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "hades/memory/record.h"
namespace hades {

// Blend weights + decay. The tuning seam: change here, no manifest key in v1. Relevance is
// deliberately dominant so a strong old match still beats a weak fresh one.
inline constexpr double kRelevanceWeight     = 1.0;
inline constexpr double kRecencyWeight       = 0.3;
inline constexpr double kReinforcementWeight = 0.2;
inline constexpr double kRecencyHalfLifeDays = 30.0;   // recency = 0.5^(age_days/half_life)

// `now` = reference epoch seconds for the recency term (test seam: deterministic scoring).
[[nodiscard]] std::vector<MemoryRecord> rank_memories(const std::vector<MemoryRecord>& all,
                                                      const std::string& query,
                                                      std::size_t top_n, double now);

// Clock-backed convenience overload (what the MemoryModule calls).
[[nodiscard]] std::vector<MemoryRecord> rank_memories(const std::vector<MemoryRecord>& all,
                                                      const std::string& query,
                                                      std::size_t top_n);
}  // namespace hades
```

In `src/apps/memory/memory.cpp`, add `#include <chrono>`, `#include <cmath>`, `#include <map>`, and REPLACE the whole `rank_memories` function with:

```cpp
std::vector<MemoryRecord> rank_memories(const std::vector<MemoryRecord>& all,
                                        const std::string& query, std::size_t top_n,
                                        double now) {
  const auto q = tokenize(query);
  if (q.empty()) return {};   // no query tokens -> nothing is relevant

  // Reinforcement input: how many records restate each non-empty topic. Counted over the
  // WHOLE store (not just relevant records) — restating a fact reinforces it regardless of
  // how the current query happens to word things.
  std::map<std::string, std::size_t> bucket;
  for (const auto& r : all)
    if (!r.topic.empty()) ++bucket[r.topic];

  // Supersession: newest ts wins per non-empty topic. Ties on ts are broken by index so the
  // choice is deterministic (later line in an append-only store = later write).
  std::map<std::string, std::size_t> newest;   // topic -> winning index
  for (std::size_t i = 0; i < all.size(); ++i) {
    if (all[i].topic.empty()) continue;
    auto it = newest.find(all[i].topic);
    if (it == newest.end() || all[i].ts >= all[it->second].ts) newest[all[i].topic] = i;
  }

  struct Scored { std::size_t idx; double score; };
  std::vector<Scored> scored;
  for (std::size_t i = 0; i < all.size(); ++i) {
    // Admission 1: superseded records never surface.
    if (!all[i].topic.empty() && newest[all[i].topic] != i) continue;
    // Admission 2: must be relevant to the query.
    const auto t = tokenize(all[i].text);
    std::size_t matched = 0;
    for (const auto& w : q) if (t.count(w)) ++matched;
    if (matched == 0) continue;

    const double relevance = static_cast<double>(matched) / static_cast<double>(q.size());
    const double age_days = (now - all[i].ts) / 86400.0;
    // Future/unknown timestamps clamp to full freshness / no boost rather than exploding.
    const double recency = age_days <= 0.0
                               ? 1.0
                               : std::pow(0.5, age_days / kRecencyHalfLifeDays);
    const std::size_t n = all[i].topic.empty() ? 1u : bucket[all[i].topic];
    const double reinforcement = 1.0 - 1.0 / (1.0 + static_cast<double>(n));

    scored.push_back({i, kRelevanceWeight * relevance + kRecencyWeight * recency +
                             kReinforcementWeight * reinforcement});
  }

  // Fully ordered: score desc, then ts desc, then text asc — no ties left to chance.
  std::sort(scored.begin(), scored.end(), [&all](const Scored& a, const Scored& b) {
    if (a.score != b.score) return a.score > b.score;
    if (all[a.idx].ts != all[b.idx].ts) return all[a.idx].ts > all[b.idx].ts;
    return all[a.idx].text < all[b.idx].text;
  });
  std::vector<MemoryRecord> out;
  for (std::size_t i = 0; i < scored.size() && i < top_n; ++i) out.push_back(all[scored[i].idx]);
  return out;
}

std::vector<MemoryRecord> rank_memories(const std::vector<MemoryRecord>& all,
                                        const std::string& query, std::size_t top_n) {
  const double now = std::chrono::duration<double>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  return rank_memories(all, query, top_n, now);
}
```

In `load_memories`, read the topic tolerantly — replace the `out.push_back(...)` line with:

```cpp
    std::string topic;   // absent or non-string -> "" (legacy line / junk): never throws
    if (j.contains("topic") && j["topic"].is_string()) topic = j["topic"].get<std::string>();
    out.push_back({j["text"].get<std::string>(), ts, topic});
```

- [ ] **Step 4: Build + test.** `-R "MemoryRank|MemoryStore"` → all pass, **including every pre-existing test unchanged**. Then the full suite: 798 + 10 = 808.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/memory/record.h include/hades/memory/rank.h src/apps/memory/memory.cpp tests/test_memory_rank.cpp tests/test_memory_store.cpp
git commit -m "feat: archival memory supersession by topic + relevance/recency/reinforcement ranking"
```

---

## Task 2: `save_memory` gains an optional `topic`

**Files:**
- Modify: `tools/save_memory_main.cpp`
- Test: `tests/test_save_memory_tool.cpp` (append)

**Interfaces:**
- Consumes: the store format Task 1 reads (`{"text","ts","topic"?}`).
- Produces: `save_memory {text, topic?}`; the record line carries `topic` ONLY when a non-empty string was given (legacy-identical lines otherwise).

- [ ] **Step 1: Write the failing tests** — APPEND to `tests/test_save_memory_tool.cpp` (match the file's existing helper style; if it has a call helper, reuse it rather than duplicating):

```cpp
TEST(SaveMemoryTool, WritesTopicWhenGiven) {
  const std::string store = ::testing::TempDir() + "/save_topic_" +
                            std::to_string(::getpid()) + ".jsonl";
  std::filesystem::remove(store);
  nlohmann::json call{{"call", "save_memory"},
                      {"args", {{"text", "prefers window seats"}, {"topic", "seat-pref"}}}};
  ProcResult r = run_subprocess({SAVE_MEMORY_BIN, store}, call.dump(), 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.value("ok", false)) << r.out;
  std::ifstream f(store);
  std::string line;
  ASSERT_TRUE(std::getline(f, line));
  auto rec = nlohmann::json::parse(line, nullptr, false);
  ASSERT_TRUE(rec.is_object());
  EXPECT_EQ(rec.value("text", ""), "prefers window seats");
  EXPECT_EQ(rec.value("topic", ""), "seat-pref");
}

TEST(SaveMemoryTool, OmitsTopicWhenAbsentOrEmpty) {
  const std::string store = ::testing::TempDir() + "/save_notopic_" +
                            std::to_string(::getpid()) + ".jsonl";
  std::filesystem::remove(store);
  for (const auto& args : {nlohmann::json{{"text", "standalone note"}},
                           nlohmann::json{{"text", "standalone note"}, {"topic", ""}}}) {
    nlohmann::json call{{"call", "save_memory"}, {"args", args}};
    ProcResult r = run_subprocess({SAVE_MEMORY_BIN, store}, call.dump(), 30.0);
    auto j = nlohmann::json::parse(r.out, nullptr, false);
    ASSERT_TRUE(j.value("ok", false)) << r.out;
  }
  std::ifstream f(store);
  std::string line;
  while (std::getline(f, line)) {
    auto rec = nlohmann::json::parse(line, nullptr, false);
    ASSERT_TRUE(rec.is_object());
    EXPECT_FALSE(rec.contains("topic"));   // legacy-identical line shape
  }
}

TEST(SaveMemoryTool, NonStringTopicFailsClosed) {
  const std::string store = ::testing::TempDir() + "/save_badtopic_" +
                            std::to_string(::getpid()) + ".jsonl";
  std::filesystem::remove(store);
  nlohmann::json call{{"call", "save_memory"},
                      {"args", {{"text", "x"}, {"topic", 42}}}};
  ProcResult r = run_subprocess({SAVE_MEMORY_BIN, store}, call.dump(), 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_FALSE(j.is_discarded());
  EXPECT_FALSE(j.value("ok", true));                    // house rule: non-string fails closed
  EXPECT_FALSE(std::filesystem::exists(store));         // and nothing was written
}

TEST(SaveMemoryTool, DescribeAdvertisesOptionalTopic) {
  ProcResult r = run_subprocess({SAVE_MEMORY_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.value("ok", false));
  const auto& schema = j["result"]["schema"];
  EXPECT_TRUE(schema["properties"].contains("topic"));
  const auto req = schema.value("required", nlohmann::json::array());
  EXPECT_EQ(std::find(req.begin(), req.end(), "topic"), req.end());   // optional
  EXPECT_NE(std::find(req.begin(), req.end(), "text"), req.end());    // text still required
}
```

Add `#include <algorithm>`, `#include <filesystem>`, `#include <fstream>`, `#include <unistd.h>` to that test file if not already present.

- [ ] **Step 2: Run — expect FAIL** (topic not written, not in schema, non-string tolerated).

- [ ] **Step 3: Implement.** In `tools/save_memory_main.cpp`:

Describe branch — replace the `schema` value so `topic` is advertised as optional and the description tells the model when to use it:

```cpp
             {"description",
              "Persist a fact or observation to long-term memory. Pass `topic` (a short "
              "stable slug) when this fact REPLACES something you saved before — the newest "
              "record for a topic is the one that gets recalled, so the stale value stops "
              "surfacing. Leave `topic` out for standalone observations."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"text", {{"type", "string"}}},
                 {"topic",
                  {{"type", "string"},
                   {"description",
                    "optional supersession key, e.g. \"seat-pref\"; newest record for a "
                    "topic wins"}}}}},
               {"required", {"text"}}}}}}};
```

Save branch — read the topic with the house fail-closed rule and write it only when non-empty:

```cpp
    bool has_text = args.contains("text") && args["text"].is_string();
    std::string text = has_text ? args["text"].get<std::string>() : "";
    // Non-string topic fails the WHOLE call (house rule; an empty string counts as absent).
    const bool bad_topic = args.contains("topic") && !args["topic"].is_string();
    std::string topic;
    if (!bad_topic && args.contains("topic")) topic = args["topic"].get<std::string>();
    if (bad_topic) {
      out = {{"ok", false}, {"result", {{"error", "topic must be a string"}}}};
    } else if (!has_text || text.empty()) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: text"}}}};
    } else {
      double ts = std::chrono::duration<double>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
      std::ofstream f(store, std::ios::app);  // append-only
      if (!f) {
        out = {{"ok", false}, {"result", {{"error", "cannot append: " + store}}}};
      } else {
        nlohmann::json rec{{"text", text}, {"ts", ts}};
        if (!topic.empty()) rec["topic"] = topic;   // absent when unused: legacy line shape
        f << rec.dump() << "\n";
        out = {{"ok", true}, {"result", {{"saved", true}}}};
      }
    }
```

(Keep the rest of the function — the closing `else` for unknown calls and the final `dump()` — as it is.)

- [ ] **Step 4: Build + test.** `-R SaveMemoryTool` → all pass (new + existing). Full suite: 812.
- [ ] **Step 5: Commit.**

```bash
git add tools/save_memory_main.cpp tests/test_save_memory_tool.cpp
git commit -m "feat: save_memory optional topic arg (supersession key)"
```

---

## Task 3: Ship — soul.md, docs, CLAUDE.md, both-lane verification

**Files:**
- Modify: `prompts/soul.md`, `docs/manifest-reference.md`, `CLAUDE.md`

- [ ] **Step 1: soul.md** — in the `## Memory` section, directly after the paragraph describing `save_memory` (archival memory), insert:

```markdown
When a fact you are saving REPLACES one you saved before — a preference that changed, a setting
that moved, a status that advanced — pass the same short `topic` slug on both saves (e.g.
`seat-pref`, `deploy-host`). Only the newest record for a topic is recalled, so the old value
stops surfacing instead of contradicting the new one. Leave `topic` off for standalone facts.
```

- [ ] **Step 2: manifest-reference.md** — find the `Memory` block section (grep `## .*\`Memory\` block`) and append a short paragraph after its key table:

```markdown
**Retrieval (archival).** Records are admitted only if they share a token with the query, then
ranked by a blend of relevance (query-token overlap), recency (30-day half-life) and
reinforcement (how often the topic was restated). `save_memory`'s optional `topic` is a
supersession key: among records sharing a non-empty topic only the NEWEST is eligible, so a
corrected fact replaces the one it corrects. Records without a topic are never superseded. The
store itself stays append-only — superseded values remain on disk for audit. Weights live in
`include/hades/memory/rank.h` (no manifest key). Note: the opt-in `embedding_memory` semantic
path has no topic data, so a superseded fact can still surface there.
```

- [ ] **Step 3: CLAUDE.md** — add a `### Archival memory supersession + blended ranking (shipped 2026-08-22, `feat/memory-supersession`)` subsection under Current state covering: the two-stage ranker (admission = relevance>0 + newest-per-topic; scoring = weighted blend with the four constants), rank-time-not-write-time rationale (append-only store preserved, audit trail kept, pure-function seam), `save_memory` optional `topic` + the house fail-closed non-string rule, backward compatibility (legacy lines → `""` → today's behavior; existing tests pass unchanged), and the documented v1 edges (auto-extract still writes untopiced facts; the embeddings path has no topic so it cannot supersede; nothing is ever deleted). Update the header test count. Also strike/annotate the memory-v2 work-list item "dedup/decay/importance" as partially shipped, pointing at the new section. Credit the mnem review as the prompt for the idea.

- [ ] **Step 4: Full verification, BOTH lanes.**

```bash
nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure
nix develop --command cmake --build build-tsan && nix develop --command ctest --test-dir build-tsan --output-on-failure
```

Expected: ALL green in both. **Use the ACTUAL final count in every doc number you write** (expected 812 = 798 + 10 + 4; verify with `ctest -N`, do not trust the estimate).

- [ ] **Step 5: Commit.**

```bash
git add prompts/soul.md docs/manifest-reference.md CLAUDE.md
git commit -m "feat: ship memory supersession — soul.md topic guidance, manifest-reference, CLAUDE.md"
```

---

## Verification (end-to-end)

1. Full suite both lanes, expected 812/812.
2. Manual smoke (Vaios, optional): `save_memory {text:"prefers window seats", topic:"seat-pref"}` after an earlier `{text:"prefers aisle seats", topic:"seat-pref"}` → ask a question mentioning seats → only the window record appears in the injected memory block; both lines still present in `.hades/memory.jsonl`.

## Execution

Subagent-driven development: fresh implementer per task (opus), per-task cpp-reviewer (opus), final whole-branch review, then ff-merge to main. Push only on Vaios's word.
