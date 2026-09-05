# SimpleX voice input — design (2026-09-05)

**Goal:** a SimpleX voice message drives a normal hades turn, exactly as a Telegram voice note
already does. Today `parse_simplex_events` drops everything that is not
`msgContent.type == "text"` (`// v1: text only`), so a voice note is silently ignored — no turn,
no reply, no error to the sender.

**Scope: receive only.** The reply stays **text**. Sending a voice note back over SimpleX needs an
XFTP upload plus a `duration`-carrying send command — a materially larger surface than receiving,
and the same split STT/TTS took for Telegram (two features, two branches). Deferred, with the
reason recorded below.

---

## Protocol ground truth

Verified against upstream `bots/api/{TYPES,EVENTS,COMMANDS}.md` (branch `stable`) and
`simplex-chat --help` (v6.5.6.1, the version the flake ships). Not inferred:

- **A voice message is an ordinary `newChatItems` frame.** Same envelope we already parse
  (`chatInfo.type == "direct"`, `chatItem.chatDir.type == "directRcv"`,
  `content.type == "rcvMsgContent"`). Only the content type differs:

  ```
  MsgContent / Voice:
  - type: "voice"
  - text: string      (usually empty)
  - duration: int
  ```

- **The audio is not in that frame.** It hangs off `chatItem.file`:

  ```
  CIFile:
  - fileId: int64
  - fileName: string
  - fileSize: int64
  - fileSource: CryptoFile?      // absent until the file is on disk
  - fileStatus: CIFileStatus
  - fileProtocol: FileProtocol
  ```

- **`fileStatus` is a state machine.** A received file goes
  `rcvInvitation → rcvAccepted → rcvTransfer → rcvComplete` (with `rcvAborted`, `rcvCancelled`,
  `rcvError`, `rcvWarning` as failure exits). At `newChatItems` time the status is
  `rcvInvitation` — **the bytes are not on disk yet.**

- **`RcvFileComplete` is the "now it's readable" signal:**

  ```
  - type: "rcvFileComplete"
  - user: User
  - chatItem: AChatItem          // the same item, now with fileSource populated
  ```

- **Terminal failures are `rcvFileError` and `rcvFileSndCancelled`:**

  ```
  - type: "rcvFileError"          - type: "rcvFileSndCancelled"
  - chatItem_: AChatItem?         - chatItem: AChatItem
  - agentError: AgentErrorType    - rcvFileTransfer: RcvFileTransfer
  - rcvFileTransfer: RcvFileTransfer
  ```

  Note `chatItem_` is **optional** on the error path, so the file id must be read from
  `rcvFileTransfer.fileId` — which both frames carry — and never from `chatItem`.
  `rcvFileWarning` has the same shape but is **not** terminal (upstream: it fires when CLI settings
  block a file server), so we ignore it; a file that then never completes is reclaimed by the
  pending-table cap rather than by guessing that a warning means failure.

- **`CryptoFile { filePath: string, cryptoArgs: CryptoFileArgs? }`.** A populated `cryptoArgs`
  means the file on disk is **encrypted at rest** and cannot simply be read and POSTed to an STT
  backend. This is the sharpest edge in the whole feature.

- **Receive command:**

  ```
  /freceive <fileId>[ approved_relays=on][ encrypt=on|off][ inline=on|off][ <filePath>]
  ```

  `encrypt=off` is the knob that guarantees `cryptoArgs == null`, and the trailing `<filePath>`
  lets us name the destination instead of discovering it.

- **Daemon-side alternative:** `--files-folder FOLDER`, `-a/--auto-accept-files FILE_SIZE`, and
  `-f/--allow-instant-files` make the daemon accept files on its own.

---

## Decision: explicit `/freceive`, not daemon auto-accept

Two ways to get the bytes:

**(A) Daemon auto-accept** — start the daemon with `--files-folder` + `-a <size>`, wait for
`rcvFileComplete`, read `fileSource.filePath`. Least code.

**(B) Explicit per-file `/freceive fileId encrypt=off <path>`** — we decide what to accept.

**We take (B).** Three reasons, in order of weight:

1. **`encrypt=off` is not optional for us.** Under (A) the daemon decides encryption; if it
   encrypts, `cryptoArgs` is set and we cannot read the audio without implementing SimpleX's
   file-decryption scheme. (B) makes the guarantee explicit and local.
2. **Auto-accept accepts everything from everyone.** A non-allowlisted contact could push files at
   an agent that silently discards their messages — unbounded disk use with nothing on the other
   side ever reading them. Under (B) we only ever accept a file that is attached to a **voice**
   message from an **already-allowlisted** contact, under a size cap. That matches the module's
   existing posture, where `allow_contacts` is required and non-allowed senders are dropped.
3. It leaves the operator's daemon command line alone. `Simplex.command` already owns the daemon
   invocation; a feature that silently requires two extra daemon flags is a support trap.

Cost of (B): one new `SimplexApi` method and a small piece of state between the two events.

---

## Flow

All of it on the **event thread**. The module holds one persistent socket, and the branch review's
C1 finding was precisely that a cross-thread send corrupts it — the `drain_notifies_()` pattern
exists for that reason. Nothing here posts from another thread.

```
newChatItems (msgContent.type == "voice", chatItem.file present)
  │
  ├─ contact not in allow_contacts ......................... drop, silent (existing rule)
  ├─ no Stt provider wired ................................. reply "voice not enabled", no turn
  ├─ fileSize > voice_max_bytes ............................ reply "voice message too large", no turn
  │
  └─ /freceive <fileId> encrypt=off <tmp>   ................ remember fileId → {contact, tmp path}
        │
        ▼
rcvFileComplete (chatItem.file.fileId == remembered)
  │
  ├─ transcribe(tmp) via SttProvider ....................... fail → reply "didn't catch that", no turn
  ├─ delete tmp (always, success or failure)
  └─ empty transcript ...................................... reply "didn't catch that", no turn
        │
        ▼
  normal turn: TurnGate → TURN_ORIGIN=human → USER_MESSAGE → run_until → send_text
```

Failure exits (`rcvError`, `rcvCancelled`, `rcvAborted` on a remembered fileId) drop the pending
entry, delete any partial temp file, and tell the sender the voice message could not be received.

**Fail-soft throughout**, matching Telegram's `handle_voice_`: any error becomes a short text reply
to the sender, never a crashed turn and never a thrown exception out of the event loop.

---

## Components

### 1. `SxEvent` gains two kinds

```cpp
enum class Kind { None, Text, ContactRequest, Connected, Voice, FileDone, FileFailed };
```

New fields (kept flat, like the existing struct — no variant):

- `long long file_id = 0;`   — Voice / FileDone / FileFailed
- `long long file_size = 0;` — Voice
- `int duration = 0;`        — Voice (informational; logged, not used for gating in v1)

`Voice` carries `contact_id` + `display_name` like `Text` does. `FileDone`/`FileFailed` carry
`file_id` only — the module maps that back to a contact through its pending table, so a completion
event for a file we never accepted is ignored rather than trusted.

### 2. `parse_simplex_events` learns three frames

- `newChatItems` with `msgContent.type == "voice"` **and** a `chatItem.file` object → `Voice`
  (`file_id`, `file_size`, `duration`). A voice item without a `file` is malformed → dropped.
  The existing `"text"` branch is untouched; every other content type still falls through.
- `rcvFileComplete` → `FileDone`, file id from `chatItem.file.fileId` (`chatItem` is mandatory on
  this frame).
- `rcvFileError` and `rcvFileSndCancelled` → `FileFailed`, file id from **`rcvFileTransfer.fileId`**
  (`chatItem_` is optional on the error frame, so it cannot be the source). `rcvFileWarning` is
  deliberately **not** mapped — it is non-terminal.

Tolerant and non-throwing exactly as today: anything unrecognised yields `{}`.

### 3. `SimplexApi` gains one method

```cpp
// Accept an offered file into `dest_path`, unencrypted. false on failure (module logs, replies).
virtual bool receive_file(long long file_id, const std::string& dest_path) = 0;
```

`WsSimplexApi` sends `/freceive <file_id> encrypt=off <dest_path>` over the same corrId round-trip
every other command uses. `encrypt=off` is **hard-coded, not configurable** — an encrypted local
file is unreadable to us, so making it an option would only let an operator break the feature.

### 4. `SimplexModule`

- `void set_stt(SttProvider*)` — mirrors `TelegramModule`. No `set_tts` in this version: nothing
  can play audio back yet.
- `std::map<long long, PendingVoice> pending_voice_` — `{contact_id, tmp_path}`, keyed by fileId.
  Event-thread only, so no mutex. **Capped at `kMaxPendingVoice = 8`**; over the cap the oldest is
  dropped with its temp file deleted, so a contact spamming voice notes that never complete cannot
  grow it without bound.
- Temp files: `<temp_dir>/hades-sx-voice-<fileId>.<ext>`, deleted on every exit path. `temp_dir`
  comes from `std::filesystem::temp_directory_path()`, the Telegram precedent.

### 5. Wiring

```cpp
if (a.stt) a.simplex->set_stt(a.stt.get());
```

next to the existing Telegram lines in `wire_agent`, under the same null guards. The `Stt` block
stays the opt-in switch: no block → `Agent.stt == nullptr` → SimpleX stays text-only and behaves
exactly as it does today.

### 6. Config

One new key on the existing `Simplex` block:

| key | default | meaning |
|---|---|---|
| `voice_max_bytes` | `10485760` (10 MB) | refuse to accept a voice file larger than this |

Garbage or non-positive → default, the house rule for every other numeric key in the block.

---

## Security

- **Allowlist first.** The accept decision happens *after* the `allow_contacts` check, so a
  non-allowlisted contact can never cause a `/freceive`. This is the property daemon auto-accept
  would have given away.
- **Size cap before acceptance**, from `fileSize` in the offer — we refuse before any bytes move.
- **Voice only.** An `image`/`video`/`file` message is still dropped, as today. This feature does
  not open general file receipt.
- **Bridge unchanged.** It is never given an `SttProvider`; a peer cannot send audio.
- **Transcript is untrusted user input**, exactly like a typed SimpleX message — it becomes a
  `USER_MESSAGE` and passes every existing objective and capability gate. No new trust.

## Testing

Unit, against the existing `FakeApi` — no daemon, no socket, no network:

- `parse_simplex_events`: canned frames for voice-with-file, voice-without-file (dropped),
  `rcvFileComplete`, a failure status, and a regression case asserting a text frame still parses
  byte-identically.
- Module: allowlisted voice → `receive_file` called with `encrypt=off` and a temp path; completion
  → transcribe → `USER_MESSAGE` posted; non-allowlisted voice → **no** `receive_file`; oversize →
  no `receive_file` + a text reply; transcribe failure → reply, no turn; `FileDone` for an unknown
  fileId → ignored; pending-table cap evicts oldest and deletes its temp file.
- Wiring: `Stt` block present + `simplex` rostered → provider injected; absent → null.

A scripted `FakeStt` returns a canned transcript, so no STT backend is needed in the suite.

## Deferred

- **Sending voice back** (the TTS half). Needs XFTP upload plus a duration-carrying send; the
  reply stays text in this version.
- **Group voice messages** — the module is DM-only (`chatInfo.type == "direct"`), unchanged here.
- **Images / general files** — same parser seam, deliberately not opened.
- **`duration`-based gating** — parsed and logged; a "too long" policy can use it later without a
  protocol change.
