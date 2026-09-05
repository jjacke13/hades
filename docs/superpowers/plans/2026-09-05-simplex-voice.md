# SimpleX voice input — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to
> implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** a SimpleX voice message is transcribed and drives a normal hades turn; the reply stays text.

**Architecture:** three layers, mirroring how the SimpleX front-end is already built — parser +
daemon seam (`SimplexApi`) first, then the module that uses them, then wiring/config/docs. Everything
runs on the module's own event thread; nothing new is posted from another thread.

**Tech Stack:** C++20, nlohmann_json, GoogleTest. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-09-05-simplex-voice-design.md` — read it; it carries the
verified protocol shapes (upstream `bots/api/{TYPES,EVENTS,COMMANDS}.md`).

## Global Constraints

- **Baseline is 820/820 tests, and both sanitizer lanes must stay green:** `build/` (ASan+UBSan,
  configured `-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"`) and `build-tsan/`. **Never
  reconfigure an existing build dir** — sanitizer flags live only in the CMake cache and a bare
  reconfigure silently drops them.
- **Do not edit existing test assertions.** Add tests; changing one that already passes needs an
  explicit justification in the task report.
- **Event-thread-only.** `api_`, `known_ids_`, and the new pending-voice table are touched ONLY from
  the event thread (`step_once` and what it calls). The module owns one persistent socket; a
  cross-thread send corrupts it — that was the C1 finding on the original branch, and
  `drain_notifies_()` exists solely to marshal onto this thread. No new mutex is needed for the
  pending table precisely because it never leaves that thread.
- **Fail-soft.** Every failure path becomes a short text reply to the sender or a silent drop —
  never a thrown exception out of the event loop, never a crashed turn.
- **House rules:** an empty-string argument counts as absent; a non-string where a string is
  required fails the whole operation; a garbage or non-positive numeric config value falls back to
  its default (never 0).
- **`encrypt=off` is hard-coded**, never configurable — an encrypted local file is unreadable to us.
- **Allowlist gates acceptance.** A `/freceive` may only ever be issued for a contact that passed
  `allowed_()`.
- Work in the worktree `~/Desktop/repos/hades-wt-simplex-voice` on branch `feat/simplex-voice`.
  Never touch `~/Desktop/repos/hades` — it holds unrelated uncommitted work.
- Commit only files you changed; never `git add -A`.

---

### Task 1: Event vocabulary, parser, and the `receive_file` command

**Files:**
- Modify: `include/hades/simplex/api.h` (SxEvent kinds + fields, `receive_file` on the interface)
- Modify: `src/apps/simplex/simplex.cpp` (`parse_simplex_events`, `WsSimplexApi::receive_file`)
- Test: `tests/test_simplex_parse.cpp`, `tests/test_simplex_api.cpp`

**Interfaces:**
- Produces: `SxEvent::Kind::{Voice,FileDone,FileFailed}`; `SxEvent::file_id`, `file_size`,
  `duration`; `virtual bool SimplexApi::receive_file(long long file_id, const std::string& dest_path)`.
- Consumes: nothing from earlier tasks.

**Note:** adding a pure virtual to `SimplexApi` breaks every existing `FakeApi` in the test suite.
Finding and updating those fakes is part of this task — the suite must be green at its end.

- [ ] **Step 1: Write the failing parser tests**

Add to `tests/test_simplex_parse.cpp`. The voice frame is the existing text frame with the content
swapped and a `file` object added:

```cpp
TEST(SimplexParse, VoiceMessageWithFileYieldsVoiceEvent) {
  const std::string frame = R"({"resp":{"type":"newChatItems","chatItems":[{
    "chatInfo":{"type":"direct","contact":{"contactId":7,"localDisplayName":"vaios"}},
    "chatItem":{"chatDir":{"type":"directRcv"},
      "content":{"type":"rcvMsgContent","msgContent":{"type":"voice","text":"","duration":5}},
      "file":{"fileId":42,"fileName":"voice.m4a","fileSize":1234,
              "fileStatus":{"type":"rcvInvitation"}}}}]}})";
  const auto evs = parse_simplex_events(frame);
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].kind, SxEvent::Kind::Voice);
  EXPECT_EQ(evs[0].contact_id, 7);
  EXPECT_EQ(evs[0].display_name, "vaios");
  EXPECT_EQ(evs[0].file_id, 42);
  EXPECT_EQ(evs[0].file_size, 1234);
  EXPECT_EQ(evs[0].duration, 5);
}

TEST(SimplexParse, VoiceMessageWithoutFileIsDropped) {
  const std::string frame = R"({"resp":{"type":"newChatItems","chatItems":[{
    "chatInfo":{"type":"direct","contact":{"contactId":7,"localDisplayName":"vaios"}},
    "chatItem":{"chatDir":{"type":"directRcv"},
      "content":{"type":"rcvMsgContent","msgContent":{"type":"voice","text":"","duration":5}}}}]}})";
  EXPECT_TRUE(parse_simplex_events(frame).empty());
}

TEST(SimplexParse, RcvFileCompleteYieldsFileDone) {
  const std::string frame = R"({"resp":{"type":"rcvFileComplete","chatItem":{
    "chatInfo":{"type":"direct","contact":{"contactId":7,"localDisplayName":"vaios"}},
    "chatItem":{"chatDir":{"type":"directRcv"},
      "content":{"type":"rcvMsgContent","msgContent":{"type":"voice","text":"","duration":5}},
      "file":{"fileId":42,"fileName":"voice.m4a","fileSize":1234,
              "fileSource":{"filePath":"/tmp/voice.m4a"},
              "fileStatus":{"type":"rcvComplete"}}}}}})";
  const auto evs = parse_simplex_events(frame);
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].kind, SxEvent::Kind::FileDone);
  EXPECT_EQ(evs[0].file_id, 42);
}

// chatItem_ is OPTIONAL on the error frame, so the id must come from rcvFileTransfer.
TEST(SimplexParse, RcvFileErrorWithoutChatItemStillYieldsFileFailed) {
  const std::string frame = R"({"resp":{"type":"rcvFileError",
    "rcvFileTransfer":{"fileId":42,"senderDisplayName":"vaios"}}})";
  const auto evs = parse_simplex_events(frame);
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].kind, SxEvent::Kind::FileFailed);
  EXPECT_EQ(evs[0].file_id, 42);
}

TEST(SimplexParse, RcvFileSndCancelledYieldsFileFailed) {
  const std::string frame = R"({"resp":{"type":"rcvFileSndCancelled",
    "rcvFileTransfer":{"fileId":9}}})";
  const auto evs = parse_simplex_events(frame);
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].kind, SxEvent::Kind::FileFailed);
  EXPECT_EQ(evs[0].file_id, 9);
}

// Non-terminal: a warning must NOT evict a pending transfer.
TEST(SimplexParse, RcvFileWarningIsIgnored) {
  const std::string frame = R"({"resp":{"type":"rcvFileWarning",
    "rcvFileTransfer":{"fileId":9}}})";
  EXPECT_TRUE(parse_simplex_events(frame).empty());
}
```

Regression — a plain text frame must parse exactly as before. If an equivalent assertion already
exists in this file, do not duplicate it; note that in your report instead.

- [ ] **Step 2: Run the tests, confirm they fail**

```
cd ~/Desktop/repos/hades-wt-simplex-voice
nix develop --command cmake --build build && nix develop --command ctest --test-dir build -R SimplexParse
```
Expected: compile failure (`Kind::Voice` does not exist).

- [ ] **Step 3: Extend `SxEvent` in `include/hades/simplex/api.h`**

```cpp
struct SxEvent {
  enum class Kind { None, Text, ContactRequest, Connected, Voice, FileDone, FileFailed };
  Kind kind = Kind::None;
  long long contact_id = 0;     // Text / Voice / Connected
  std::string display_name;     // all kinds (the sender's local display name)
  long long request_id = 0;     // ContactRequest
  std::string text;             // Text
  long long file_id = 0;        // Voice / FileDone / FileFailed
  long long file_size = 0;      // Voice (from the offer, before any bytes move)
  int duration = 0;             // Voice (seconds; informational in v1)
};
```

And on the interface, with the contract in a comment:

```cpp
  // Accept an offered file into `dest_path`, UNENCRYPTED (encrypt=off is not negotiable — an
  // encrypted local file cannot be read back for transcription). false on failure; the caller
  // logs and tells the sender.
  virtual bool receive_file(long long file_id, const std::string& dest_path) = 0;
```

- [ ] **Step 4: Extend `parse_simplex_events`**

In the `newChatItems` loop, replace the `if (str(mc, "type") != "text") continue;` line with a
branch that keeps text behaviour byte-identical and adds voice:

```cpp
      const std::string mct = str(mc, "type");
      if (mct == "text") {
        SxEvent ev;
        ev.kind = SxEvent::Kind::Text;
        ev.contact_id = num(contact, "contactId");
        ev.display_name = str(contact, "localDisplayName");
        ev.text = str(mc, "text");
        if (ev.contact_id != 0 && !ev.text.empty()) out.push_back(std::move(ev));
      } else if (mct == "voice") {
        // The audio is NOT in this frame: it arrives via the file transfer, and the item is only
        // readable once rcvFileComplete fires. A voice item with no `file` is malformed -> drop.
        const auto f = item.find("file");
        if (f == item.end() || !f->is_object()) continue;
        SxEvent ev;
        ev.kind = SxEvent::Kind::Voice;
        ev.contact_id = num(contact, "contactId");
        ev.display_name = str(contact, "localDisplayName");
        ev.file_id = num(*f, "fileId");
        ev.file_size = num(*f, "fileSize");
        const auto d = mc.find("duration");
        ev.duration = (d != mc.end() && d->is_number_integer()) ? d->get<int>() : 0;
        if (ev.contact_id != 0 && ev.file_id != 0) out.push_back(std::move(ev));
      }
```

Then add the two new top-level branches next to `contactConnected`:

```cpp
  } else if (type == "rcvFileComplete") {
    // chatItem is mandatory on this frame; the file id lives on the item's file object.
    const auto& aci = resp.value("chatItem", nlohmann::json::object());
    const auto& item = aci.value("chatItem", nlohmann::json::object());
    const auto& file = item.value("file", nlohmann::json::object());
    SxEvent ev;
    ev.kind = SxEvent::Kind::FileDone;
    ev.file_id = num(file, "fileId");
    if (ev.file_id != 0) out.push_back(std::move(ev));
  } else if (type == "rcvFileError" || type == "rcvFileSndCancelled") {
    // chatItem_ is OPTIONAL on rcvFileError, so rcvFileTransfer is the only reliable id source.
    // rcvFileWarning has the same shape but is NOT terminal (it fires when CLI settings block a
    // file server), so it is deliberately not handled here.
    const auto& rft = resp.value("rcvFileTransfer", nlohmann::json::object());
    SxEvent ev;
    ev.kind = SxEvent::Kind::FileFailed;
    ev.file_id = num(rft, "fileId");
    ev.display_name = str(rft, "senderDisplayName");
    if (ev.file_id != 0) out.push_back(std::move(ev));
  }
```

- [ ] **Step 5: Implement `WsSimplexApi::receive_file`**

Next to `accept_request`, using the same `command_ok_` round-trip:

```cpp
  bool receive_file(long long file_id, const std::string& dest_path) override {
    // /freceive <fileId>[ encrypt=on|off][ <filePath>] — encrypt=off so the bytes on disk are
    // readable (an encrypted CryptoFile carries cryptoArgs and cannot be POSTed to an STT backend).
    return command_ok_("/freceive " + std::to_string(file_id) + " encrypt=off " + dest_path,
                       "rcvFileAccepted");
  }
```

- [ ] **Step 6: Update every existing `FakeApi` in the test suite**

Grep for classes deriving `SimplexApi` (`tests/test_simplex_module.cpp`, `tests/test_simplex_api.cpp`,
possibly others) and add an overriding `receive_file` that records its arguments and returns a
scriptable result. Keep every existing assertion unchanged.

- [ ] **Step 7: Add the api-level test**

In `tests/test_simplex_api.cpp`, follow whatever pattern that file already uses to assert a command
string, and assert `receive_file(42, "/tmp/x.m4a")` sends a cmd containing `/freceive 42`,
`encrypt=off`, and `/tmp/x.m4a`.

- [ ] **Step 8: Build and run the full suite**

```
nix develop --command cmake --build build && nix develop --command ctest --test-dir build
```
Expected: 820 + your new tests, all passing.

- [ ] **Step 9: Commit**

```bash
git add include/hades/simplex/api.h src/apps/simplex/simplex.cpp tests/test_simplex_parse.cpp tests/test_simplex_api.cpp tests/test_simplex_module.cpp
git commit -m "feat: simplex voice/file events + receive_file command"
```

---

### Task 2: Module voice handling

**Files:**
- Modify: `include/hades/module/simplex_module.h`
- Modify: `src/apps/simplex/simplex.cpp` (SimplexModule section)
- Test: `tests/test_simplex_module.cpp`

**Interfaces:**
- Consumes: everything Task 1 produced.
- Produces: `void SimplexModule::set_stt(SttProvider*)`, `void set_voice_max_bytes(long long)`.

- [ ] **Step 1: Write the failing module tests**

Use the file's existing `FakeApi`/fixture style. Add a fake STT beside it:

```cpp
class FakeStt : public SttProvider {
 public:
  std::string transcript = "hello from voice";
  bool fail = false;
  std::vector<std::string> paths;
  std::string transcribe(const std::string& audio_path) override {
    paths.push_back(audio_path);
    if (fail) throw std::runtime_error("stt down");
    return transcript;
  }
};
```

(Match `SttProvider`'s real signature — read `include/hades/stt/*` first and adapt; if it returns a
struct or takes extra arguments, follow that, do not change the interface.)

Tests to add:

```cpp
TEST(SimplexModuleVoice, AllowlistedVoiceIsAcceptedThenTranscribedIntoATurn);
TEST(SimplexModuleVoice, NonAllowlistedVoiceNeverCallsReceiveFile);
TEST(SimplexModuleVoice, OversizeVoiceIsRefusedWithoutReceiveFile);
TEST(SimplexModuleVoice, NonPositiveFileSizeIsRefusedWithoutReceiveFile);   // absent/mistyped/wrapped
TEST(SimplexModuleVoice, NoSttProviderMeansNoReceiveFile);
TEST(SimplexModuleVoice, TranscribeFailureRepliesAndPostsNoUserMessage);
TEST(SimplexModuleVoice, FileDoneForUnknownFileIdIsIgnored);
TEST(SimplexModuleVoice, FileFailedDropsThePendingEntry);
TEST(SimplexModuleVoice, PendingTableIsCappedAndEvictsOldest);
```

Each drives the module by scripting events into the `FakeApi` and calling `step_once()`, then
asserts on the fake's recorded calls and on the Blackboard. For the happy path assert both that
`receive_file` was called and that a `USER_MESSAGE` carrying the transcript reached the bus.

- [ ] **Step 2: Run, confirm failure**

```
nix develop --command cmake --build build && nix develop --command ctest --test-dir build -R SimplexModuleVoice
```
Expected: compile failure (`set_stt` does not exist).

- [ ] **Step 3: Header additions**

```cpp
#include "hades/stt/stt.h"     // adjust to the real STT header path
...
  void set_stt(SttProvider* s) { stt_ = s; }
  void set_voice_max_bytes(long long n) { if (n > 0) voice_max_bytes_ = n; }
 private:
  void handle_voice_(const SxEvent& ev);
  void handle_file_done_(const SxEvent& ev);
  void handle_file_failed_(const SxEvent& ev);
  struct PendingVoice { long long contact_id; std::string path; };
  // fileId -> where we told the daemon to put it. EVENT THREAD ONLY (no mutex by construction).
  // Bounded: a sender whose transfers never complete must not grow this without limit.
  std::map<long long, PendingVoice> pending_voice_;
  static constexpr std::size_t kMaxPendingVoice = 8;
  SttProvider* stt_ = nullptr;
  long long voice_max_bytes_ = 10 * 1024 * 1024;
```

- [ ] **Step 4: Implement the three handlers**

```cpp
void SimplexModule::handle_voice_(const SxEvent& ev) {
  if (!stt_) { send_reply_(ev.contact_id, "Voice messages aren't enabled on this agent."); return; }
  // A non-positive size means we do NOT know how big the file is: the field was absent or
  // mistyped (num() fails closed to 0), or it was an out-of-int64 unsigned that wrapped negative.
  // The size cap is a load-bearing safety control here — it is the reason we accept files
  // explicitly instead of letting the daemon auto-accept — so an unknown size must be refused,
  // not waved through by `0 > cap` being false. Gated in the module rather than dropped in the
  // parser so the sender still gets told, instead of the message vanishing silently.
  if (ev.file_size <= 0 || ev.file_size > voice_max_bytes_) {
    send_reply_(ev.contact_id, "That voice message is too large for me to process.");
    return;
  }
  // Bound the table BEFORE inserting: drop the oldest pending transfer (and its temp file) so a
  // stream of never-completing offers cannot grow it.
  while (pending_voice_.size() >= kMaxPendingVoice) {
    auto oldest = pending_voice_.begin();
    std::error_code ec;
    std::filesystem::remove(oldest->second.path, ec);
    pending_voice_.erase(oldest);
  }
  const std::string dest =
      (std::filesystem::temp_directory_path() /
       ("hades-sx-voice-" + std::to_string(ev.file_id) + ".bin")).string();
  if (!api_->receive_file(ev.file_id, dest)) {
    send_reply_(ev.contact_id, "I couldn't download that voice message.");
    return;
  }
  pending_voice_[ev.file_id] = PendingVoice{ev.contact_id, dest};
}

void SimplexModule::handle_file_done_(const SxEvent& ev) {
  auto it = pending_voice_.find(ev.file_id);
  if (it == pending_voice_.end()) return;      // not ours (or already reclaimed) — ignore
  const PendingVoice pv = it->second;
  pending_voice_.erase(it);
  std::string transcript;
  try {
    transcript = stt_ ? stt_->transcribe(pv.path) : std::string{};
  } catch (...) { transcript.clear(); }        // fail-soft: never escapes the event loop
  std::error_code ec;
  std::filesystem::remove(pv.path, ec);        // always, success or failure
  if (transcript.empty()) {
    send_reply_(pv.contact_id, "Sorry, I didn't catch that.");
    return;
  }
  drive_turn_(pv.contact_id, transcript, "USER_MESSAGE");   // match drive_turn_'s real signature
}

void SimplexModule::handle_file_failed_(const SxEvent& ev) {
  auto it = pending_voice_.find(ev.file_id);
  if (it == pending_voice_.end()) return;
  const PendingVoice pv = it->second;
  pending_voice_.erase(it);
  std::error_code ec;
  std::filesystem::remove(pv.path, ec);
  send_reply_(pv.contact_id, "That voice message didn't come through.");
}
```

Adapt `drive_turn_` usage to its real signature — read `handle_text_` and post `TURN_ORIGIN=human`
the same way it does, so a voice turn is indistinguishable from a typed one downstream.

- [ ] **Step 5: Dispatch the new kinds in `handle_event_`**

```cpp
    case SxEvent::Kind::Voice:
      if (allowed_(ev)) handle_voice_(ev);      // non-allowed: silently dropped, as for Text
      break;
    case SxEvent::Kind::FileDone:   handle_file_done_(ev); break;
    case SxEvent::Kind::FileFailed: handle_file_failed_(ev); break;
```

`FileDone`/`FileFailed` carry no contact, so they are not allowlist-gated — the pending-table lookup
is the gate: an id we never accepted is ignored.

- [ ] **Step 6: Run the tests, then the whole suite**

- [ ] **Step 7: Commit**

```bash
git add include/hades/module/simplex_module.h src/apps/simplex/simplex.cpp tests/test_simplex_module.cpp
git commit -m "feat: simplex module transcribes received voice messages into turns"
```

---

### Task 3: Wiring, config, docs

**Files:**
- Modify: `app/agent_wiring.cpp`
- Modify: `docs/manifest-reference.md` (§16 Simplex)
- Modify: `CLAUDE.md`
- Test: `tests/test_simplex_wiring.cpp`

**Interfaces:** consumes `set_stt` / `set_voice_max_bytes` from Task 2.

- [ ] **Step 1: Write the failing wiring tests**

```cpp
TEST(SimplexWiring, SttProviderIsInjectedWhenTheSttBlockIsPresent);
TEST(SimplexWiring, NoSttBlockLeavesSimplexTextOnly);
TEST(SimplexWiring, VoiceMaxBytesIsParsedFromTheSimplexBlock);
TEST(SimplexWiring, GarbageVoiceMaxBytesFallsBackToTheDefault);
```

Follow the existing wiring-test pattern in that file (manifest text → `wire_agent` → assert).

- [ ] **Step 2: Run, confirm failure**

- [ ] **Step 3: Wire it**

In `wire_agent`, beside the existing telegram injection and under the same null guards:

```cpp
  if (a.simplex) {
    if (a.stt) a.simplex->set_stt(a.stt.get());
    a.simplex->set_voice_max_bytes(cfg_voice_max_bytes);
  }
```

Parse `voice_max_bytes` from the `Simplex` block with the house numeric-parse pattern used by the
other keys in that block (garbage or non-positive → default, never 0).

- [ ] **Step 4: Run the full suite, both lanes**

```
nix develop --command cmake --build build      && nix develop --command ctest --test-dir build
nix develop --command cmake --build build-tsan && nix develop --command ctest --test-dir build-tsan
```

- [ ] **Step 5: Docs**

`docs/manifest-reference.md` §16: add `voice_max_bytes` to the key table; add a short "Voice
messages" paragraph stating that voice input needs an `Stt` block, that the reply is text (sending
voice back is not supported), that files are accepted only from allowlisted contacts and only for
voice content, and that the file is fetched with `encrypt=off` so it can be read.

`CLAUDE.md`: a subsection under Current state in the house style (what shipped, the protocol facts,
the accept-explicitly-not-auto-accept decision and why, the v1 edges), plus the Simplex gotcha entry
noting voice is receive-only.

- [ ] **Step 6: Commit**

```bash
git add app/agent_wiring.cpp tests/test_simplex_wiring.cpp docs/manifest-reference.md CLAUDE.md
git commit -m "feat: wire simplex voice input + docs"
```
