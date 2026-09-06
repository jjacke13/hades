# One session per day — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to
> implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** the session id becomes a logical date instead of a launch timestamp, so a restart mid-day
rejoins the same conversation and the session ends when the day does.

**Architecture:** three layers — a pure `logical_date` function plus boot path resolution, then the
Arbiter's lazy rollover at turn start, then the embeddings exclusion re-point plus config and docs.

**Tech Stack:** C++20, GoogleTest. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-09-06-daily-session-design.md` — read it first.

## Global Constraints

- **Baseline is 853 tests on BOTH lanes.** `build/` (ASan+UBSan) and `build-tsan/` must both end
  green. **Never reconfigure an existing build dir** — sanitizer flags live only in the CMake cache
  and a bare reconfigure silently drops them.
- **Do not edit existing test assertions** unless the behaviour genuinely changed; if it did, say so
  explicitly in your report and make the new assertion pin the new behaviour, not just a count.
- Work in the worktree `~/Desktop/repos/hades-wt-daily-session` on branch `feat/daily-session`.
  **Never touch `~/Desktop/repos/hades`** — it holds unrelated uncommitted work (an Android cross
  build). Verify with `git rev-parse --show-toplevel` before editing.
- Commit only files you changed; never `git add -A`.
- **Machine-local time throughout** (the cron precedent). No UTC conversion anywhere.
- House rules: a garbage or out-of-range numeric config value falls back to its documented default,
  never to 0.
- Fail-soft: session IO is best-effort and must never throw out of a turn.

---

### Task 1: `logical_date` and boot path resolution

**Files:**
- Modify: `include/hades/session_id.h`, `src/core/session.cpp`
- Modify: `app/hades_main.cpp` (use the new id; resume semantics)
- Test: `tests/test_session_id.cpp` (create if absent; if created, add it to `CMakeLists.txt`)

**Interfaces:**
- Produces: `std::string logical_date(std::time_t t, int cutoff_hour);` and
  `std::string current_session_id(int cutoff_hour);`
- Consumes: nothing from earlier tasks.

- [ ] **Step 1: Write the failing tests**

`logical_date` is pure — it takes an explicit time so it is testable without touching the clock.

```cpp
// Local-time helper for the tests: build a time_t from local Y-M-D H:M.
static std::time_t local_tm(int y, int mo, int d, int h, int mi) {
  std::tm tm{};
  tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
  tm.tm_hour = h; tm.tm_min = mi; tm.tm_isdst = -1;
  return std::mktime(&tm);
}

TEST(LogicalDate, BeforeCutoffBelongsToThePreviousDay) {
  EXPECT_EQ(logical_date(local_tm(2026, 9, 6, 3, 59), 4), "2026-09-05");
}
TEST(LogicalDate, AtCutoffStartsTheNewDay) {
  EXPECT_EQ(logical_date(local_tm(2026, 9, 6, 4, 0), 4), "2026-09-06");
}
TEST(LogicalDate, EveningIsTheSameDay) {
  EXPECT_EQ(logical_date(local_tm(2026, 9, 6, 23, 30), 4), "2026-09-06");
}
TEST(LogicalDate, AfterMidnightStillYesterday) {
  EXPECT_EQ(logical_date(local_tm(2026, 9, 7, 2, 0), 4), "2026-09-06");
}
TEST(LogicalDate, CutoffZeroIsPlainCalendarDate) {
  EXPECT_EQ(logical_date(local_tm(2026, 9, 6, 0, 1), 0), "2026-09-06");
  EXPECT_EQ(logical_date(local_tm(2026, 9, 6, 23, 59), 0), "2026-09-06");
}
TEST(LogicalDate, CrossesMonthAndYearBoundaries) {
  EXPECT_EQ(logical_date(local_tm(2026, 1, 1, 2, 0), 4), "2025-12-31");
  EXPECT_EQ(logical_date(local_tm(2026, 3, 1, 1, 0), 4), "2026-02-28");
}
TEST(LogicalDate, CutoffTwentyThreeIsAcceptedAndShiftsAlmostAFullDay) {
  EXPECT_EQ(logical_date(local_tm(2026, 9, 6, 22, 0), 23), "2026-09-05");
  EXPECT_EQ(logical_date(local_tm(2026, 9, 6, 23, 0), 23), "2026-09-06");
}
```

- [ ] **Step 2: Run them, confirm they fail**

```
cd ~/Desktop/repos/hades-wt-daily-session
nix develop --command cmake --build build && nix develop --command ctest --test-dir build -R LogicalDate
```
Expected: compile failure (`logical_date` undeclared).

- [ ] **Step 3: Implement**

In `include/hades/session_id.h`:

```cpp
// Logical session date for an instant, as "YYYY-MM-DD". A session "day" runs from
// `cutoff_hour` to the same hour next day, LOCAL time, so anything before the cutoff belongs to
// the previous day (03:59 with cutoff 4 is still yesterday). cutoff_hour 0 = plain calendar date.
// Out-of-range cutoffs are clamped to [0,23] here so a bad config can never shift the date wildly.
std::string logical_date(std::time_t t, int cutoff_hour);

// logical_date() of the current local clock.
std::string current_session_id(int cutoff_hour);
```

Implement by subtracting `cutoff_hour` hours from `t` and formatting the resulting **local**
`std::tm` with `%Y-%m-%d`. Use `localtime_r`. Do NOT hand-roll calendar arithmetic — subtract
seconds and let the C library normalise, which gets month, year and DST right for free.

- [ ] **Step 4: Boot resolution in `hades_main.cpp`**

Replace `make_session_id()` with `current_session_id(cutoff)` for the fresh-session id, and change
the no-flag path so an EXISTING file for today is reused rather than suffixed:

- no `--resume`: if `<dir>/<id>.jsonl` exists → use it (and load its history, which
  `set_session_path` + the Arbiter's existing `load_history` already do on resume); else create it.
- `--resume <id>`: unchanged (`MalConfig` when absent).
- bare `--resume`: unchanged (newest file).

**Read `resolve_session_path` before changing it** — its current contract is "a NEW session that
collides gets the first free `-N`". That collision-avoidance must stay for `/new` (Task 2) but must
NOT apply to the daily boot path, where colliding with today's file is the entire point. Make that
distinction explicit in the code, not incidental.

`make_session_id()` stays for now (other callers/tests may use it); do not delete it in this task.

- [ ] **Step 5: Run the full suite on both lanes, commit**

```bash
git add include/hades/session_id.h src/core/session.cpp app/hades_main.cpp tests/test_session_id.cpp CMakeLists.txt
git commit -m "feat: logical session date + resume today's session at boot"
```

---

### Task 2: Arbiter rollover at turn start

**Files:**
- Modify: `include/hades/arbiter.h`, `src/apps/arbiter/arbiter.cpp`
- Test: `tests/test_arbiter.cpp`

**Interfaces:**
- Consumes: `current_session_id` from Task 1.
- Produces: the `SESSION_ROTATED` bus key `{from, to, path}`; `Arbiter::set_day_cutoff_hour(int)`.

- [ ] **Step 1: Write the failing tests**

The Arbiter must not read the wall clock directly in tests. Add a seam — an injectable "now"
(`std::function<std::time_t()>` defaulting to `std::time`) or a settable current-id override —
whichever fits the class's existing style. Read `test_arbiter.cpp` first and follow its rig.

```cpp
TEST(Arbiter, TurnAfterTheCutoffRotatesTheSession);
TEST(Arbiter, TurnBeforeTheCutoffDoesNotRotate);
TEST(Arbiter, RotationClearsHistorySummaryAndPendingAndBumpsTheEpoch);
TEST(Arbiter, RotationPostsSessionRotatedWithFromToAndPath);
TEST(Arbiter, ANewSessionCommandAlsoPostsSessionRotated);
TEST(Arbiter, RotationHappensBeforeTheTurnIsSentNotAfter);
```

- [ ] **Step 2: Run, confirm failure**

- [ ] **Step 3: Implement**

At the top of `start_turn`, before assembling the request: recompute the current logical id; if it
differs from the running session's **base** id, rotate. Rotation reuses the `NEW_SESSION` path —
clear `history_`, `summary_text_`, `summarized_upto_`, `pending_compact_`, `clear_pending()`,
`++turn_epoch_` — then switches the append path to the new date's file and posts `SESSION_ROTATED`.

**The base-id comparison matters:** after `/new` the session file is `2026-09-06-2`, whose base id
is still `2026-09-06`. Compare against the base, or `/new` would make every subsequent turn think
the day had changed and rotate on every turn. Store the base id as a member rather than re-deriving
it from the filename.

`NEW_SESSION` must post `SESSION_ROTATED` too (Task 3 subscribes to it for the embeddings fix).

- [ ] **Step 4: Full suite both lanes, commit**

---

### Task 3: Every live-session-path consumer follows rotation, config, docs

**SCOPE EXPANDED 2026-09-06** after the Task 2 review. The original scope named only the embeddings
exclusion. There are in fact **three** consumers that cache the live session path, and all three go
stale on every rollover — same bug class, one fix pattern, so one task owns them:

1. `EmbeddingMemoryModule::live_session_path_` — the originally specced one.
2. **`session_search`** — its exclusion filename is baked into the subprocess argv at wiring time
   (`app/agent_wiring.cpp`), so no bus event can move it. After a rollover the tool SKIPS yesterday
   (which it should now be searching) and RANKS today's live file (which it should be skipping),
   handing back the current conversation as past-session excerpts. Fix in the house style: have the
   Arbiter inject the live filename at dispatch and strip any LLM-supplied one — the same pattern
   `expect_version` already uses for the staleness guard. Alternative: pass only the directory and
   let the tool exclude the newest-mtime `*.jsonl`.
3. **`HttpServerModule::session_path_`** — set once in `hades_main`, read on an httplib worker
   thread, so an overnight `--serve` renders yesterday's transcript as the current one. This is
   NOT a one-line hook: pump-thread write vs httplib-thread read needs the same mutex treatment and
   the same TSan gate as (1).

4. **Release the OLD file's flock on rotation.** Carried over from the Task 2 review: `rotate_session_`
   claims the new path via `lock_session_file` but the old fd is never released, because that
   function keeps its fd for the process lifetime and does not hand it back. Two costs: one leaked
   fd per rotation (trivial), and — the real one — a **closed** session stays locked, so another
   process doing `--resume <yesterday>` hard-fails with "session is open in another running hades"
   for a session nobody occupies. Needs a Task-1 signature change: return an RAII handle, or add an
   explicit release. Note the trap the Task 2 fixer found: because the fd is held for the process
   lifetime, any code that probes a path with `lock_session_file` before rotating will make the
   rotation divert to `-N`.

Two notes from the reviewer that bind this task:
- **Ignore an empty `path` in `SESSION_ROTATED`.** With an empty `sessions_dir` the event carries
  `{to:"", path:""}`; a subscriber assigning it blindly would DISABLE the exclusion entirely.
- Module attach order puts `embedding` before `arbiter`, so on a rotating turn recall runs BEFORE
  the rotation and `SESSION_ROTATED` lands on the next dispatch. Fine for the exclusion — but do not
  build anything that depends on the opposite order.

### Task 3 (original heading): Embeddings re-point, config, docs

**Files:**
- Modify: `include/hades/module/embedding_memory_module.h`, `src/apps/embedding_memory/embedding_memory.cpp`
- Modify: `app/agent_wiring.cpp` (parse `day_cutoff_hour`, pass to the Arbiter)
- Modify: `docs/manifest-reference.md`, `CLAUDE.md`
- Test: `tests/test_embedding_memory_module.cpp`, `tests/test_wiring*.cpp` (follow existing names)

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(EmbeddingMemory, SessionRotatedRepointsTheLiveExclusion);
TEST(EmbeddingMemory, RotationToASuffixedNewSessionAlsoRepoints);   // the retired /new gotcha
TEST(SessionWiring, DayCutoffHourIsParsed);
TEST(SessionWiring, GarbageDayCutoffHourFallsBackToFour);
TEST(SessionWiring, OutOfRangeDayCutoffHourFallsBackToFour);
```

- [ ] **Step 2: Run, confirm failure**

- [ ] **Step 3: Implement**

`EmbeddingMemoryModule` subscribes `SESSION_ROTATED` and updates `live_session_path_`.

**This is the first runtime write to that member and it is cross-thread**: the pump thread writes
it, the index worker reads it. It was previously safe *only* because it was written once before the
worker existed. Guard it — a small dedicated mutex around the read in `run_index_` and the write in
the subscriber, or reuse the existing `index_mu_` if that does not widen its critical section into
the network call. Do NOT leave it unsynchronised; **the TSan lane is the gate on this task.**

`day_cutoff_hour` is parsed from the `Session` block in `wire_agent` (default 4; `<0` or `>23` or
unparseable → 4) and passed to the Arbiter.

- [ ] **Step 4: Both lanes**

```
nix develop --command cmake --build build      && nix develop --command ctest --test-dir build
nix develop --command cmake --build build-tsan && nix develop --command ctest --test-dir build-tsan
```

- [ ] **Step 5: Docs**

`docs/manifest-reference.md`: `day_cutoff_hour` in the `Session` key table; a short "Session
lifetime" note — one session per logical day, shared across all front-ends, restart rejoins,
`/new` makes a suffixed same-day session, old timestamp sessions still resumable.

`CLAUDE.md`: a subsection in house style covering the logical-date rule, lazy rollover at turn
start, the base-id comparison that keeps `/new` from rotating every turn, and — explicitly — that
**this retires the documented gotcha** that `/new` does not re-point the embeddings exclusion.
Update that gotcha entry rather than leaving it contradicting the new behaviour.

- [ ] **Step 6: Commit**
