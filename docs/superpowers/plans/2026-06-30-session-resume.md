# Session resume Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. **Implementers run on OPUS**. Read current files fully first. **DISCIPLINE:** build + FULL suite + `git commit` + verify `git log` + write report before replying; do NOT report DONE unless `git log` shows your commit.

**Goal:** Persist the turn-by-turn conversation per session to `.hades/sessions/<id>.jsonl` and reload it on `--resume`, so a restart (REPL or `--serve`) keeps context. The full history persists; only a bounded recent window is sent to the LLM each turn (overflow guard). Spec: `docs/superpowers/specs/2026-06-30-session-resume-design.md` (read first).

**Architecture:** The Arbiter owns the conversation (`history_`). Add persistence (`append_history`/`load_history`/`set_session_path`) + a per-turn overflow window in `start_turn()`. `hades_main` resolves `--resume [id]` → a session file and wires it. `/new` (REPL) rotates to a fresh session.

**Tech Stack:** C++20 · CMake + Ninja · Nix dev shell · GoogleTest · `<filesystem>`. Build/test ONLY inside `nix develop`.

## Global Constraints

- **C++20**, g++ 15.2. Every command inside `nix develop`.
- **TDD:** RED → GREEN → COMMIT per task. Commit `<type>: <desc>`, **no attribution footer**.
- **Backward-compat:** with NO session path set (the test `build_agent` overload + a no-`--resume` run that still writes a new file — but tests set no path), `append_history` is push-only and the overflow window is a no-op below the default budget → the 175 existing tests stay green.
- **Append-only / crash-safe:** one JSON message per line, appended as the Arbiter adds it. Tolerant load (skip a corrupt trailing line, like `load_memories` in `src/memory/store.cpp`).
- **Store paths whitespace-free** (existing wiring rule). `.hades/` is gitignored. The session file is conversation, never the api key.

## File Structure

```
include/hades/arbiter.h          T1,T2,T4 (modify)  session_path_, budget, id-gen members + methods
src/arbiter/arbiter.cpp          T1,T2,T4 (modify)  append_history/load_history/window/NEW_SESSION
include/hades/session_id.h       T3 (new)           make_session_id() + resolve_session_path()
src/core/session_id.cpp          T3 (new)
app/hades_main.cpp               T3 (modify)        parse --resume [id]; resolve + wire
src/module/chat_module.cpp       T4 (modify)        intercept /new -> post NEW_SESSION
manifests/dev.hades              T3 (modify)        Session: sessions_dir, history_budget_chars (optional/doc)
tests/test_arbiter.cpp           T1,T2,T4 (extend)
tests/test_session_id.cpp        T3 (new)
tests/test_chat.cpp              T4 (extend)
```

---

## Task 1: Arbiter conversation persistence (`append_history` / `load_history` / `set_session_path`)

**Files:** Modify `include/hades/arbiter.h`, `src/arbiter/arbiter.cpp`, `tests/test_arbiter.cpp`. **Read `src/arbiter/arbiter.cpp` fully** — the 4 `history_.push_back(...)` sites (USER_MESSAGE handler; the assistant `dispatch_or_gate` push; the `on_tool_result` tool-msg push; the `on_confirm` approved `pending_msg_` push) and `start_turn()` (which copies `history_` into the request).

**Interfaces — Produces:** `void set_session_path(std::string)`, `void load_history()`, `void append_history(const nlohmann::json&)`. With no path set: `append_history` only does `history_.push_back` (identical to today).

- [ ] **Step 1: Failing tests** (`tests/test_arbiter.cpp`):
  - `AppendHistoryWritesFileWhenPathSet`: `Arbiter a; a.on_attach(bb); a.set_session_path(tmp+"/s.jsonl");` drive a `USER_MESSAGE` + a scripted `LLM_RESPONSE` (plain answer) → after pump, read the file: 2 lines, line 1 = `{"role":"user",...}`, line 2 = `{"role":"assistant","content":...}` (verbatim `dump()`).
  - `AppendHistoryPushOnlyWhenNoPath`: no `set_session_path` → same drive → `history_` has the messages but NO file is created (assert the path doesn't exist). (Locks backward-compat.)
  - `LoadHistoryRoundTripsAndToleratesCorruptTail`: write a jsonl with 3 valid message lines + a 4th truncated/garbage line; `Arbiter b; b.set_session_path(thatfile); b.load_history();` → `history_` has the 3 valid messages in order; the corrupt trailing line is skipped (no throw). (Expose `history_` size via a getter `std::size_t history_size() const` for the assertion, or assert via a subsequent LLM_REQUEST carrying them.)
- [ ] **Step 2: Run, expect FAIL** (methods undeclared).
- [ ] **Step 3: Implement.**
  - `arbiter.h`: add `std::string session_path_;` + `void set_session_path(std::string p){ session_path_ = std::move(p); }` + declare `void load_history();` + `void append_history(const nlohmann::json& msg);` + (for tests) `std::size_t history_size() const { return history_.size(); }`. Include `<fstream>`/`<filesystem>` in the .cpp.
  - `arbiter.cpp`: `append_history(msg){ history_.push_back(msg); if(!session_path_.empty()){ std::ofstream f(session_path_, std::ios::app); if(f) f << msg.dump() << "\n"; } }`. Replace the 4 `history_.push_back(...)` sites with `append_history(...)`. `load_history(){ if(session_path_.empty()) return; std::ifstream f(session_path_); std::string line; while(std::getline(f,line)){ if(line.empty()) continue; auto j = nlohmann::json::parse(line, nullptr, false); if(!j.is_discarded() && j.is_object()) history_.push_back(j); } }` (tolerant — `parse(...,false)` returns discarded on bad input; skip). Create the parent dir on first append if needed (`std::filesystem::create_directories(path.parent_path())` guarded).
- [ ] **Step 4: Build + test** `-R Arbiter`, then FULL suite green (175 + new). The test overload sets no path → unchanged.
- [ ] **Step 5: Commit** `feat: Arbiter persists conversation to a session jsonl (append_history/load_history; push-only without a path)`

---

## Task 2: Per-turn overflow window in `start_turn()`

**Files:** Modify `include/hades/arbiter.h`, `src/arbiter/arbiter.cpp`, `tests/test_arbiter.cpp`. **Read `start_turn()`** (it does `for (const auto& m : history_) messages.push_back(m);` after the system+memory messages).

**Interfaces — Produces:** `void set_history_budget_chars(double)`; the LLM request includes only the most-recent suffix of `history_` whose cumulative `dump()` size ≤ budget, beginning on a valid (non-orphan-tool) boundary. Default budget (no setter / 0) = `kDefaultHistoryBudgetChars` (120000).

- [ ] **Step 1: Failing tests** (`tests/test_arbiter.cpp`):
  - `HistoryWindowSendsRecentSuffixWithinBudget`: set a tiny budget (`a.set_history_budget_chars(200)`); seed `history_` with several messages (drive turns, or expose a way to preload — simplest: drive multiple USER/ASSISTANT round trips so history_ grows), capture the `LLM_REQUEST` messages → assert only the recent ones are present and total serialized size of the history-portion ≤ ~budget (allow the system/memory messages on top), and the FIRST history message in the request is NOT an orphan `{role:tool}` (it's a `user` or a complete `assistant`).
  - `SmallHistorySentWholeUnderDefaultBudget`: default budget, a few messages → ALL history messages present in the request (no trimming). (Locks the no-op-below-budget backward-compat.)
- [ ] **Step 2: Run, expect FAIL** (no windowing; setter undeclared).
- [ ] **Step 3: Implement.**
  - `timeouts.h`-style constant: add `inline constexpr double kDefaultHistoryBudgetChars = 120000.0;` (put in `include/hades/session_id.h` from T3 or a small `include/hades/history_budget.h` — single-source; coordinate with T3, but T2 may land first so define it where arbiter can include it now, e.g. top of arbiter.h or a tiny header).
  - `arbiter.h`: `double history_budget_chars_ = kDefaultHistoryBudgetChars;` + `void set_history_budget_chars(double c){ if(c > 0) history_budget_chars_ = c; }` + private `std::vector<nlohmann::json> windowed_history_() const;`.
  - `arbiter.cpp`: `windowed_history_()` — walk `history_` from the END accumulating `m.dump().size()` until adding the next would exceed `history_budget_chars_`; that gives a start index. Then ADVANCE the start forward past any leading `{role:tool}` message (orphaned from its assistant tool_calls) — i.e. the window must begin on a message that is `role==user` OR (`role==assistant`); never start on `role==tool`. Return the suffix `[start, end)`. (If the whole history fits, start=0.) In `start_turn()`, replace the `for(history_)` loop with `for (const auto& m : windowed_history_()) messages.push_back(m);`.
- [ ] **Step 4: Build + test** `-R Arbiter`, FULL suite green (default budget → existing arbiter tests unchanged since their histories are tiny).
- [ ] **Step 5: Commit** `feat: bound the per-turn LLM request to a history char budget (tool-pairing-safe window)`

---

## Task 3: `--resume [id]` resolution + wiring

**Files:** Create `include/hades/session_id.h`, `src/core/session_id.cpp`, `tests/test_session_id.cpp`. Modify `app/hades_main.cpp`, `manifests/dev.hades`, `CMakeLists.txt`. **Read `app/hades_main.cpp`** (argv parsing for `--serve [port]`, `manifest.session()`, where it builds the Eventlog + Blackboard + `build_agent`, the REPL/serve dispatch).

**Interfaces — Produces:** `std::string make_session_id()` (clock → `YYYYMMDD-HHMMSS`); `std::string resolve_session_path(const std::string& dir, bool resume, const std::string& id, const std::string& new_id)`; `hades_main` parses `--resume [id]` and wires the Arbiter (`set_session_path` + `load_history` on resume; `set_history_budget_chars`; `set_session_path` to a new file otherwise).

- [ ] **Step 1: Failing tests** (`tests/test_session_id.cpp`):
  - `MakeSessionIdFormat`: `make_session_id()` matches `^\d{8}-\d{6}$` (and two close calls differ or are ordered — at minimum: is non-empty, right shape).
  - `ResolveNewSession`: `resolve_session_path(tmpdir, /*resume*/false, "", "20260630-100000")` → `tmpdir + "/20260630-100000.jsonl"` (a new file path; does not require existence).
  - `ResolveSpecificId`: with `tmpdir/abc.jsonl` present → `resolve_session_path(tmpdir, true, "abc", "")` → that path. Missing id → throws/returns empty per the chosen error contract (pick: throw `std::runtime_error`/`MalConfig`; assert it).
  - `ResolveNewestSession`: write `tmpdir/20260630-090000.jsonl` then `…-100000.jsonl`; `resolve_session_path(tmpdir, true, "", "")` → the `…-100000` one (lexical-max). Empty dir + resume → returns the new_id path (fresh) — assert the documented fallback.
- [ ] **Step 2: Run, expect FAIL** (no header).
- [ ] **Step 3: Implement.**
  - `session_id.h`/`.cpp`: `make_session_id()` via `std::time`/`std::localtime` + `strftime("%Y%m%d-%H%M%S")` (binary may use the real clock). `resolve_session_path(dir, resume, id, new_id)`: if `!resume` → `dir + "/" + new_id + ".jsonl"`; if `resume && !id.empty()` → `dir+"/"+id+".jsonl"` (throw a clear error if it doesn't exist); if `resume && id.empty()` → scan `dir` for `*.jsonl`, return the lexical-max (newest); if none → `dir+"/"+new_id+".jsonl"` (fresh) + a stderr note. Use `<filesystem>`. CMake: add `src/core/session_id.cpp` to `hades_core` + the test.
  - `hades_main.cpp`: parse `--resume` and an optional following non-flag token as the id (mirror `--serve [port]` parsing). Read `sessions_dir` (default `.hades/sessions`) + `history_budget_chars` (default `kDefaultHistoryBudgetChars`) from `manifest.session()`. Compute `new_id = make_session_id()`. `path = resolve_session_path(sessions_dir, resume, id, new_id)`. After `build_agent`, if `agent.arbiter`: `agent.arbiter->set_history_budget_chars(budget); agent.arbiter->set_session_path(path); if (resume) agent.arbiter->load_history();`. (Order: set path, then load.) Works for both REPL and `--serve` (resume composes with serve).
  - `dev.hades`: add `sessions_dir = .hades/sessions` and (optional, documents the knob) `history_budget_chars = 120000` to the multi-line `Session` block. (Multi-line — no packed kv.)
- [ ] **Step 4: Build + test** `-R "Session|Manifest|Pantler"`, FULL suite green. Manual: `--resume` newest, `--resume <id>`, no-flag new.
- [ ] **Step 5: Commit** `feat: --resume [id] session resolution + wiring (newest / specific / new); sessions_dir + history_budget_chars config`

---

## Task 4: `/new` REPL command → fresh session

**Files:** Modify `include/hades/arbiter.h`, `src/arbiter/arbiter.cpp`, `src/module/chat_module.cpp`, `tests/test_arbiter.cpp`, `tests/test_chat.cpp`. **Read** the Arbiter `on_attach` subscriptions + the ChatModule `run_repl`/`run_repl_readline` loop (the `/quit` intercept).

**Interfaces — Produces:** a `NEW_SESSION` bus message; the Arbiter clears `history_` + rotates `session_path_` to a fresh file via an injectable id generator; ChatModule intercepts `/new`.

- [ ] **Step 1: Failing tests.**
  - `tests/test_arbiter.cpp` `NewSessionClearsHistoryAndRotatesFile`: `a.set_session_dir(tmp); a.set_id_generator([]{ return std::string("NEWID"); }); a.set_session_path(tmp+"/old.jsonl");` drive a turn (writes to old.jsonl, history_ non-empty), then `bb.post("NEW_SESSION", json::object(), "chat"); bb.pump();` → `history_size()==0` AND a subsequent appended message lands in `tmp/NEWID.jsonl` (not old.jsonl). (Inject the id-gen so it's deterministic — no clock.)
  - `tests/test_chat.cpp` `SlashNewPostsNewSession`: drive `run_repl` with input `"/new\n/quit\n"`, subscribe to `NEW_SESSION` → assert it was posted once.
- [ ] **Step 2: Run, expect FAIL** (NEW_SESSION unhandled; `/new` not intercepted).
- [ ] **Step 3: Implement.**
  - `arbiter.h`: `std::string sessions_dir_;` + `std::function<std::string()> id_gen_;` (default unset → in the handler, fall back to `make_session_id` if `id_gen_` is null) + `void set_session_dir(std::string d){ sessions_dir_ = std::move(d); }` + `void set_id_generator(std::function<std::string()> g){ id_gen_ = std::move(g); }`. Include `<functional>`.
  - `arbiter.cpp` `on_attach`: subscribe `NEW_SESSION` → `history_.clear(); std::string id = id_gen_ ? id_gen_() : make_session_id(); if(!sessions_dir_.empty()) session_path_ = sessions_dir_ + "/" + id + ".jsonl";` (include `session_id.h`). (Also `clear_pending()` for safety.)
  - `hades_main.cpp` (small add): after wiring the session path, also `agent.arbiter->set_session_dir(sessions_dir);` so `/new` can rotate. (No id-gen injection in prod → defaults to `make_session_id`.)
  - `chat_module.cpp` `run_repl` + `run_repl_readline`: intercept `/new` (like `/quit`): `bb_->post("NEW_SESSION", nlohmann::json::object(), "chat"); bb_->pump();` + print a note (`assistant> [new session]` via the label path); `continue;` the loop (do NOT post it as a USER_MESSAGE).
- [ ] **Step 4: Build + FULL suite green.**
- [ ] **Step 5: Commit** `feat: /new REPL command rotates to a fresh session (NEW_SESSION clears history + new file)`

---

## Self-Review (against the spec)

- **Coverage:** persistence (T1), overflow window (T2), `--resume [id]` + config + wiring (T3), `/new` (T4). Web `GET /history` + embeddings/summarization explicitly out of scope.
- **Backward-compat:** test overload sets no session path → `append_history` push-only, window no-op below 120000 → 175 tests green.
- **Crash-safety:** append-per-message; tolerant load (corrupt trailing line skipped).
- **Type consistency:** `history_` messages persisted/loaded verbatim (`json::dump`/`parse`); budget is `double` chars; id is `YYYYMMDD-HHMMSS`; `kDefaultHistoryBudgetChars` single-sourced.
- **Pairing invariant:** the window never begins on an orphan `{role:tool}` (T2) — a real provider-rejection guard.

## Verification

1. Full suite green (persistence, window, resolution, `/new`). 2. Manual (needs key): chat a few turns, `/quit`, `--resume` → the agent recalls the prior turns; `--resume <id>` picks a specific session; no-flag starts fresh; `/new` mid-run rotates. 3. `--serve --resume` → the agent has context (browser blank, by design). 4. A very long session does not overflow (window trims the request; the file keeps everything).
