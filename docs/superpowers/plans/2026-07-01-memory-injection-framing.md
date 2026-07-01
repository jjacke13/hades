# Memory-injection framing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use `- [ ]`. **Implementers run on OPUS.** Read the named files fully first. **DISCIPLINE:** build + FULL `ctest` + `git commit` + verify `git log` + write report BEFORE replying DONE.

**Goal:** Make the agent treat injected retrieved memory as its own recall by splitting it into two labeled sub-blocks — saved **facts** (reliable) vs past-**session excerpts** (your memory of past conversations; may be stale) — with framing (Arbiter + `prompts/soul.md`) that stops it saying "first exchange" / "you're quoting me".

**Architecture:** `VectorCache::query` carries each hit's `src`; `EmbeddingMemoryModule` partitions hits into fact-hits (`RETRIEVED_MEMORY_SEMANTIC`, now facts-only) and session-hits (new `RETRIEVED_SESSION_SEMANTIC`); the Arbiter renders two labeled sub-blocks in the injected `{role:system}` message; soul.md is corrected + given standing guidance.

**Tech Stack:** C++20 · CMake + Ninja · Nix dev shell (`nix develop`) · nlohmann/json · GoogleTest.

Spec: `docs/superpowers/specs/2026-07-01-memory-injection-framing-design.md` (read first).

## Global Constraints

- **C++20**, g++ 15.2. **Every build/test command inside `nix develop`** (`nix develop --command cmake --build build`, `nix develop --command ctest --test-dir build`).
- **TDD:** RED → GREEN → COMMIT per task. Commit `<type>: <desc>`, **NO attribution footer / NO Co-Authored-By**.
- **Backward-compat:** the embedding module is inert unless rostered → `RETRIEVED_SESSION_SEMANTIC` never posted → Arbiter injects only the Facts block. Both-empty → **no block** injected. A keyword-only agent still works (its block wording changes from `"Relevant memories:"` to the Facts label — intentional).
- **Fail-soft unchanged:** any embedder failure → BOTH semantic keys `""` (the module's existing try/catch stays; every fail-soft return posts both keys empty).
- **The `RETRIEVED_MEMORY_SEMANTIC` key narrows to facts-only** (was facts+sessions); the Arbiter is its only consumer.
- **Intentional wording change:** the old `"Relevant memories:\n…"` string is replaced. Existing Arbiter/module tests that assert the exact old strings are UPDATED (not a regression) — see each task.
- **Exact label strings (use verbatim):**
  - Facts: `Facts from your memory (you saved these earlier; treat as reliable):`
  - Conversations: `Excerpts from earlier sessions with this same user — your own memory of past conversations. Treat them as things you and the user already discussed (do NOT say this is a first exchange, and do NOT treat them as the user quoting you). They record what was SAID then and may be out of date — re-verify current state (files, live data, tool results) before asserting a past action's result still holds:`

## File Structure

```
include/hades/embedding/vector_cache.h    T1 (modify)  ScoredMemory gains std::string src
src/embedding/vector_cache.cpp            T1 (modify)  query() copies r.src into each result
tests/test_vector_cache.cpp               T1 (extend)  query carries src

src/module/embedding_memory_module.cpp    T2 (modify)  partition hits by src -> 2 keys; fail-soft posts both ""
tests/test_embedding_memory_module.cpp    T2 (modify)  session hit -> new key; fact hit -> old key; failure -> both ""

src/arbiter/arbiter.cpp                    T3 (modify)  two labeled sub-blocks from 3 keys
tests/test_arbiter.cpp                     T3 (modify)  update old-string tests to new labels + new session-block test
prompts/soul.md                            T3 (modify)  fix stale "keyword-only" line + add recall-framing paragraph
```

---

## Task 1: `VectorCache::query` carries `src`

**Files:** Modify `include/hades/embedding/vector_cache.h`, `src/embedding/vector_cache.cpp`. Extend `tests/test_vector_cache.cpp`. **Read first:** all of `include/hades/embedding/vector_cache.h` + `src/embedding/vector_cache.cpp`.

**Interfaces — Produces:** `struct ScoredMemory { std::string text; float score; std::string src; };`

- [ ] **Step 1: Failing test** — append to `tests/test_vector_cache.cpp`:
```cpp
TEST(VectorCache, QueryCarriesSrc) {
  std::string p = tmp("vc_src.jsonl");
  std::remove(p.c_str());
  VectorCache c(p, "echo", 2); ASSERT_TRUE(c.load());
  c.put({"m0", "memory",  "a fact",     {1.0f, 0.0f}});
  c.put({"s0", "session", "U: hi\nA: yo", {0.0f, 1.0f}});
  auto fa = c.query({1.0f, 0.05f}, 5, 0.1f);   // closest to the memory record
  ASSERT_FALSE(fa.empty());
  EXPECT_EQ(fa[0].text, "a fact");
  EXPECT_EQ(fa[0].src, "memory");
  auto se = c.query({0.05f, 1.0f}, 5, 0.1f);   // closest to the session record
  ASSERT_FALSE(se.empty());
  EXPECT_EQ(se[0].src, "session");
}
```

- [ ] **Step 2: Run, expect FAIL** (`ScoredMemory` has no `src`): `nix develop --command cmake --build build` → compile error.

- [ ] **Step 3: Implement.** In `include/hades/embedding/vector_cache.h`, change the struct:
```cpp
struct ScoredMemory { std::string text; float score; std::string src; };
```
In `src/embedding/vector_cache.cpp`, in `query`, copy `r.src` into each result (line ~58):
```cpp
    if (s >= min_similarity) scored.push_back({r.text, s, r.src});
```

- [ ] **Step 4: Build + test** `-R VectorCache` PASS, then FULL suite (`nix develop --command ctest --test-dir build`) → 247 + 1 = **248**.
- [ ] **Step 5: Commit** `feat: VectorCache::query carries each hit's src (memory vs session)`

---

## Task 2: `EmbeddingMemoryModule` partitions hits into two keys

**Files:** Modify `src/module/embedding_memory_module.cpp`. Modify `tests/test_embedding_memory_module.cpp`. **Read first:** all of `src/module/embedding_memory_module.cpp` (esp. the `USER_MESSAGE` handler in `on_attach`), `tests/test_embedding_memory_module.cpp`.

**Interfaces — Consumes:** `ScoredMemory.src` (T1). **Produces:** posts `RETRIEVED_MEMORY_SEMANTIC` (facts-only) + `RETRIEVED_SESSION_SEMANTIC` (session excerpts) — both `""` on any fail-soft path.

- [ ] **Step 1: Update + add tests** in `tests/test_embedding_memory_module.cpp`:
  - **Change** `RetrievesPastSessionTurnWhenIndexSessionsEnabled`: the session hit now arrives on the NEW key. Change its subscription + assertion from `RETRIEVED_MEMORY_SEMANTIC` to `RETRIEVED_SESSION_SEMANTIC`:
    ```cpp
    bb.subscribe("RETRIEVED_SESSION_SEMANTIC", [&](const Entry& e) { got = e.value.get<std::string>(); });
    // ... after pump:
    EXPECT_NE(got.find("alpha is the first letter"), std::string::npos);  // the past session's turn -> SESSION key
    ```
  - **Change** the two fail-soft tests (`ProviderFailureIsSoftEmpty`, `CacheStampMismatchAtQueryIsSoftEmpty`): also assert the session key is empty. Add alongside the existing `RETRIEVED_MEMORY_SEMANTIC` check:
    ```cpp
    std::string sess = "UNSET";
    bb.subscribe("RETRIEVED_SESSION_SEMANTIC", [&](const Entry& e) { sess = e.value.get<std::string>(); });
    // ... after pump:
    EXPECT_EQ(sess, "");
    ```
  - **Add** a test that an archival (memory-src) hit lands on the FACT key and NOT the session key (`RetrievesSemanticMatchAndPosts` already asserts the fact key; extend it to assert the session key is empty for an archival-only store):
    ```cpp
    // in RetrievesSemanticMatchAndPosts, after the existing asserts:
    std::string sess;
    bb.subscribe("RETRIEVED_SESSION_SEMANTIC", [&](const Entry& e) { sess = e.value.get<std::string>(); });
    // (re-post + re-pump is awkward; instead subscribe BEFORE the post — move both subscribes above the bb.post)
    ```
    **Implementer note:** simplest is to subscribe to BOTH keys before the `bb.post("USER_MESSAGE", ...)` in that test, then assert `got` (facts) contains "alpha fact" and `sess` (sessions) is `""`. Restructure the test's subscribe order accordingly.

- [ ] **Step 2: Run, expect FAIL** (`RETRIEVED_SESSION_SEMANTIC` never posted today): `nix develop --command ctest --test-dir build -R EmbeddingMemoryModule`.

- [ ] **Step 3: Implement.** In `src/module/embedding_memory_module.cpp`, in the `USER_MESSAGE` handler (`on_attach`), add a fail-soft helper and replace the single render+post. Change the handler body so **every** early-return posts BOTH keys empty, and the success path partitions by src:
```cpp
  bb.subscribe("USER_MESSAGE", [this](const Entry& e) {
    // Post both semantic keys on EVERY path (never leave a stale value from a prior turn).
    auto none = [this] {
      bb_->post("RETRIEVED_MEMORY_SEMANTIC", "", "embedding_memory");
      bb_->post("RETRIEVED_SESSION_SEMANTIC", "", "embedding_memory");
    };
    try {
      if (!e.value.is_string() || !provider_) { none(); return; }
      EmbedResult q = provider_->embed({e.value.get<std::string>()});
      if (!q.error.empty() || q.vectors.size() != 1) { none(); return; }
      VectorCache vc(cache_path(cache_dir_), q.model, q.dim);
      if (!vc.load()) { none(); return; }  // stamp mismatch -> keyword only
      auto top = vc.query(q.vectors[0], top_n_, min_similarity_);
      std::string facts, convos;
      for (const auto& r : top) {
        std::string& dst = (r.src == "session") ? convos : facts;  // memory/unknown src -> facts
        dst += "- " + r.text + "\n";
      }
      if (!facts.empty() && facts.back() == '\n') facts.pop_back();
      if (!convos.empty() && convos.back() == '\n') convos.pop_back();
      bb_->post("RETRIEVED_MEMORY_SEMANTIC", facts, "embedding_memory");
      bb_->post("RETRIEVED_SESSION_SEMANTIC", convos, "embedding_memory");
    } catch (...) {
      none();  // fail-soft: never crash a turn
    }
  });
```
(This replaces the current single-key handler body; keep the surrounding subscribe/executor/index code unchanged.)

- [ ] **Step 4: Build + test** `-R EmbeddingMemoryModule` PASS, then FULL suite → **248** (no net new count if you only modified/added within the module test; confirm no regression).
- [ ] **Step 5: Commit** `feat: embedding module posts facts (RETRIEVED_MEMORY_SEMANTIC) + session excerpts (RETRIEVED_SESSION_SEMANTIC) split by src`

---

## Task 3: Arbiter two labeled sub-blocks + soul.md framing

**Files:** Modify `src/arbiter/arbiter.cpp` (the injection block in `start_turn`, lines ~194-207), `prompts/soul.md`. Modify `tests/test_arbiter.cpp`. **Read first:** `src/arbiter/arbiter.cpp:174-214` (`start_turn` incl. `merge_memory_blocks` at 154-172), the memory-injection tests in `tests/test_arbiter.cpp`, `prompts/soul.md`.

**Interfaces — Consumes:** `RETRIEVED_MEMORY` (kw facts), `RETRIEVED_MEMORY_SEMANTIC` (semantic facts), `RETRIEVED_SESSION_SEMANTIC` (session excerpts) (T2).

- [ ] **Step 1: Update + add tests** in `tests/test_arbiter.cpp`:
  - Add a small helper near the memory tests (or inline) to pull the injected system block. The existing tests already scan for a `{role:system}` message; update their expected substring.
  - **`SemanticAbsentIsUnchanged`** — rename intent to keyword-only; change the expected string from `"Relevant memories:\n- only keyword"` to:
    ```cpp
    EXPECT_EQ(block, "Facts from your memory (you saved these earlier; treat as reliable):\n- only keyword");
    ```
  - **`MergesKeywordAndSemanticMemoryDeduped`** — it posts `RETRIEVED_MEMORY` + `RETRIEVED_MEMORY_SEMANTIC` (both now FACTS). Update the block-prefix check from `rfind("Relevant memories:", 0)` to `rfind("Facts from your memory", 0)`; keep the shared-once + both-unique-lines assertions (they still hold under the Facts block).
  - **`SemanticOnlyWithAbsentKeywordStillInjectsBlock`** — it posts only `RETRIEVED_MEMORY_SEMANTIC` (a FACT). Update its expected to the Facts label:
    ```cpp
    EXPECT_EQ(block, "Facts from your memory (you saved these earlier; treat as reliable):\n- embedding result");
    ```
  - **Add `SessionExcerptsInjectedUnderConversationLabel`:**
    ```cpp
    TEST(Arbiter, SessionExcerptsInjectedUnderConversationLabel) {
      Blackboard bb; Arbiter a; a.on_attach(bb);
      std::vector<nlohmann::json> reqs;
      bb.subscribe("LLM_REQUEST", [&](const Entry& e) { reqs.push_back(e.value); });
      bb.post("RETRIEVED_MEMORY", "- a saved fact", "memory");
      bb.post("RETRIEVED_SESSION_SEMANTIC", "- U: spaceX?\nA: I fetched the page", "embedding_memory");
      bb.post("USER_MESSAGE", "hi", "chat");
      bb.pump();
      ASSERT_FALSE(reqs.empty());
      std::string block;
      for (const auto& m : reqs[0]["messages"])
        if (m.value("role", "") == "system" && m.value("content", "").rfind("Facts from your memory", 0) == 0)
          block = m["content"];
      ASSERT_FALSE(block.empty());
      EXPECT_NE(block.find("- a saved fact"), std::string::npos);                 // facts block
      EXPECT_NE(block.find("Excerpts from earlier sessions"), std::string::npos); // conversations label
      EXPECT_NE(block.find("I fetched the page"), std::string::npos);             // session excerpt
      EXPECT_NE(block.find("re-verify current state"), std::string::npos);        // staleness caveat present
    }
    ```
  - **Add `NoBlockWhenAllMemoryEmpty`** (backward-compat — no memory keys posted → no system block beyond the base prompt). If an equivalent test exists (`NoMemoryBlockWhenRetrievedEmpty`), extend it to also post the two semantic keys as `""` and assert no `Facts from your memory` / `Excerpts from earlier sessions` block:
    ```cpp
    TEST(Arbiter, NoBlockWhenAllMemoryEmpty) {
      Blackboard bb; Arbiter a; a.on_attach(bb);
      std::vector<nlohmann::json> reqs;
      bb.subscribe("LLM_REQUEST", [&](const Entry& e) { reqs.push_back(e.value); });
      bb.post("RETRIEVED_MEMORY", "", "memory");
      bb.post("RETRIEVED_MEMORY_SEMANTIC", "", "embedding_memory");
      bb.post("RETRIEVED_SESSION_SEMANTIC", "", "embedding_memory");
      bb.post("USER_MESSAGE", "hi", "chat");
      bb.pump();
      ASSERT_FALSE(reqs.empty());
      for (const auto& m : reqs[0]["messages"]) {
        std::string c = m.value("content", "");
        EXPECT_EQ(c.rfind("Facts from your memory", 0), std::string::npos);
        EXPECT_EQ(c.rfind("Excerpts from earlier sessions", 0), std::string::npos);
      }
    }
    ```

- [ ] **Step 2: Run, expect FAIL** on the updated assertions: `nix develop --command ctest --test-dir build -R Arbiter`.

- [ ] **Step 3: Implement** — in `src/arbiter/arbiter.cpp`, replace the injection block (currently lines ~194-207) with the two-labeled-block build:
```cpp
  // Inject retrieved memory as an ephemeral {role:system} block before the last user message, in TWO
  // labeled sub-blocks so the LLM treats it as its OWN recall: saved FACTS (reliable) vs past-SESSION
  // excerpts (its memory of earlier conversations with this user; may be stale). Both empty -> no block.
  // Recomputed each turn, never stored in history_.
  std::string kw, sem_facts, sem_convos;
  if (auto m = bb_->get("RETRIEVED_MEMORY"); m && m->value.is_string()) kw = m->value.get<std::string>();
  if (auto m = bb_->get("RETRIEVED_MEMORY_SEMANTIC"); m && m->value.is_string()) sem_facts = m->value.get<std::string>();
  if (auto m = bb_->get("RETRIEVED_SESSION_SEMANTIC"); m && m->value.is_string()) sem_convos = m->value.get<std::string>();
  std::string facts = merge_memory_blocks(kw, sem_facts);   // dedup keyword + semantic facts
  std::string content;
  if (!facts.empty())
    content += "Facts from your memory (you saved these earlier; treat as reliable):\n" + facts;
  if (!sem_convos.empty()) {
    if (!content.empty()) content += "\n\n";
    content +=
        "Excerpts from earlier sessions with this same user — your own memory of past conversations. "
        "Treat them as things you and the user already discussed (do NOT say this is a first exchange, "
        "and do NOT treat them as the user quoting you). They record what was SAID then and may be out "
        "of date — re-verify current state (files, live data, tool results) before asserting a past "
        "action's result still holds:\n" + sem_convos;
  }
  if (!content.empty()) {
    nlohmann::json block = {{"role", "system"}, {"content", content}};
    int last_user = -1;
    for (int i = 0; i < static_cast<int>(messages.size()); ++i)
      if (messages[i].value("role", "") == "user") last_user = i;
    if (last_user >= 0) messages.insert(messages.begin() + last_user, block);
  }
```
(Keep `merge_memory_blocks` as-is — it now merges keyword facts + semantic facts only.)

- [ ] **Step 4: soul.md** — in `prompts/soul.md`, make two edits:
  - Replace the archival sentence + the parenthetical `(Retrieval is keyword-based for now, not semantic.)` with:
    ```
    - **Archival memory** (`save_memory`): a searchable store (`.hades/memory.jsonl`). Call
      `save_memory(text)` for details to keep for later; each turn the most relevant entries are recalled —
      by keyword, and (when semantic memory is enabled) by meaning — and shown to you in a memory block.
    ```
  - Add a short paragraph after the two-memory-kinds list (before the final "Both write…" line):
    ```
    The memory block injected just before the user's message is YOUR memory: saved facts plus excerpts of
    earlier sessions with this same user. Recognize it as your own recall — reference past conversations
    naturally, and never claim this is a "first exchange" or that the user is quoting you when memory is
    present. Session excerpts record what was said before and may be out of date, so re-verify current
    state (files, live data, tool results) before asserting a past action's result still holds.
    ```
  Keep it concise (soul.md is prepended every turn). If a prompt-assembly test hard-asserts the removed
  sentence, update it; otherwise no test change for soul.md.

- [ ] **Step 5: Build + test** `-R Arbiter` PASS (updated + new), then FULL suite green (**248**, all updated tests passing). Confirm `dev.hades` still builds.
- [ ] **Step 6: Commit** `feat: Arbiter injects memory as two labeled blocks (facts vs past-session excerpts) + soul.md recall framing`

---

## Self-Review (against the spec)

- **Coverage:** ScoredMemory.src (T1); module partition → 2 keys + fail-soft both-empty (T2); Arbiter two labeled blocks + both-empty-no-block (T3); soul.md stale-line fix + recall paragraph (T3); staleness caveat in the Conversations label (T3); test updates for the intentional wording change (T2/T3).
- **Backward-compat:** module inert → only Facts block; both-empty → no block (`NoBlockWhenAllMemoryEmpty`); keyword-only agent gets the Facts label (updated `SemanticAbsentIsUnchanged`).
- **Type consistency:** `ScoredMemory{text,score,src}` (T1) consumed by the module partition on `r.src=="session"` (T2); keys `RETRIEVED_MEMORY_SEMANTIC`/`RETRIEVED_SESSION_SEMANTIC` produced by T2, consumed by T3; label strings identical between the Global-Constraints block, T3 code, and T3 tests.
- **Fail-soft:** every module early-return + catch posts BOTH keys `""` (T2).

## Verification

1. Full suite green at each task (248).
2. `query` carries src (T1); module splits fact vs session hits into the two keys, both `""` on failure (T2); Arbiter renders Facts + Conversations labels, both-empty → no block (T3).
3. soul.md no longer says "keyword-based … not semantic" and carries the recall-framing paragraph.
4. **Live smoke (Vaios):** restart with `embedding_memory` enabled; ask a past-session topic (e.g. "where are those spaceX files?") → the agent references the past conversation as its own memory (no "first exchange"), and re-verifies current state rather than asserting stale files exist.
