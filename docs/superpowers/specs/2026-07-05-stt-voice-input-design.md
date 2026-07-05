# STT voice input — design

**Date:** 2026-07-05
**Status:** Approved (Vaios — brainstorm 2026-07-05, three sections approved)
**Branch:** `feat/voice-stt` (off `main` @ `5fe5f3c`)

## Problem

hades front-ends are text-only. Vaios wants to **talk to the agent**: send a voice message and
have it acted on. Voice is split into two independent halves — **STT** (voice → the agent, this
spec) and **TTS** (the agent talks back, a separate later spec). STT ships and stands alone.

Key requirement (Vaios): **STT is source-agnostic.** A clip is transcribed the same way whether it
arrives over Telegram or, later, a local mic. So STT is not a Telegram feature — it is a **provider
seam** (audio path in → text out) that a user-facing front-end feeds. This mirrors the existing
**embedding provider seam** exactly (one interface, two transports: HTTP + local subprocess).

WhatsApp was dropped (Cloud API is webhook-push → mandatory inbound public HTTPS / TLS / tunnel;
Vaios is P2P/self-host). Voice notes ride Telegram's existing **outbound long-poll** → NAT-free, no
inbound endpoint.

## Decisions (Vaios)

- **STT first**, TTS is a separate later spec (the two halves are independent).
- **Both** transports behind one seam, like the embedding seam: `provider = http` (default) and
  `provider = command` (local). Manifest selects.
- Local backend = **whisper** (whisper.cpp reference wrapper). **NOT** `qwen3_asr_rs` — that is a
  forked experiment, not Vaios's, explicitly excluded.
- **English only v1** (`language = en`); auto-detect / multi-language deferred to v2.
- **User-facing surfaces only.** The **Bridge is excluded** — agent↔agent `/ask` carries text; no
  `SttProvider` is ever wired into `BridgeModule`.
- Transcript posted as **plain text** (indistinguishable from a typed message).

## Architecture

STT is a **provider seam injected into a front-end**. Audio→text happens *before* the turn — it is a
front-end **input transform**, not a bus Module and not a tool (a tool is agent-invoked mid-turn;
this converts the incoming message). The seam is generic; the Telegram front-end is v1's only
consumer, a local-mic front-end later injects the same provider.

### The seam (`include/hades/stt/`, `src/stt/`)

```cpp
struct SttResult { bool ok = false; std::string text; std::string language; std::string error; };

class SttProvider {                       // include/hades/stt/provider.h
 public:
  virtual ~SttProvider() = default;
  // Transcribe the audio file at `audio_path`. NEVER throws — any failure returns
  // {ok:false, error:…}. Language is fixed at construction (English v1), not a per-call arg.
  virtual SttResult transcribe(const std::string& audio_path) = 0;
};
```

Two implementations in **one TU** `src/stt/stt_providers.cpp` (ponytail: fewest files, mirrors
`src/apps/embedding_memory/providers.cpp`):

1. **`HttpSttProvider`** — POSTs a multipart form to `<endpoint>/audio/transcriptions` over an
   **injected `SttHttpClient` seam** (like the embedding provider's injected `HttpClient` — tests
   supply a fake returning canned JSON, no socket; the glibc-getaddrinfo TSan caveat is why the seam
   is injected). Fields: `file` (the audio), `model`, `language` (when non-empty), `Authorization:
   Bearer <api_key>` from the env var → parse JSON `{"text":…}` (+ optional `language`). Non-2xx /
   unparseable / missing `text` → `{ok:false}`. **Endpoint is the BASE url**, provider appends
   `/audio/transcriptions` — SAME gotcha as embedding's `/embeddings` (`endpoint =
   https://api.ppq.ai/v1`). The real transport `cpr_stt_http(timeout_s)` uses
   `cpr::Multipart{{"file", cpr::File{path}}, {"model", …}, {"language", …}}`, redirects off
   (SSRF-hardening precedent), `timeout_s` → cpr timeout.
2. **`CommandSttProvider`** — `run_subprocess(argv + [audio_path], "", timeout_s)` (core, no shell),
   read the **plain transcript from stdout** (trimmed), non-zero exit / timeout / empty stdout →
   `{ok:false}`. `run_subprocess` has no env-passing, and v1 is English-only, so language is **baked
   into the wrapper** (not threaded through) — the argv contract is simply "last arg = audio path".
   Ship a reference wrapper `tools/whisper_reference.sh` (whisper.cpp: `whisper-cli -l en -f <audio>
   -nt -otxt` → transcript to stdout), documented like `tools/embed_reference.py`. One-shot per clip
   — **no warm child** (voice notes are human-paced; warm mode is a v2 seam).

### Config (`Stt` block, mirrors `Embedding`, opt-in)

```
Stt
{
  provider    = http                    # http | command
  endpoint    = https://api.ppq.ai/v1   # BASE url; provider appends /audio/transcriptions
  model       = whisper-1
  api_key_env = HADES_API_KEY
  language    = en                      # v1: English only (auto-detect deferred to v2)
  timeout_s   = 60
  # command   = ./tools/whisper_reference.sh   # provider = command instead
}
```

- `resolve_stt(const Block&) -> std::unique_ptr<SttProvider>` in wiring (single source of truth).
- `provider=http` **requires** `endpoint`; `provider=command` **requires** `command` — else
  `MalConfig` at launch (fail-loud precedent). Unknown provider → `MalConfig`.
- Secret via `api_key_env` **only**, never in the manifest; redacted in `session.log` (LLM/embedding
  precedent — `hades_main` best-effort resolves and redacts it).
- `command` is whitespace-split into argv; the audio path is appended as the last arg.
- **Inert unless the `Stt` block is present.** No block → `Agent.stt == nullptr` → Telegram stays
  text-only (voice skipped), zero coupling. dev.hades ships the block **commented**.

### Telegram integration (`src/apps/telegram/`, `include/hades/telegram/`)

- **Parse:** capture `message.voice.file_id` (+ `voice.mime_type` unused v1). `audio`/`video_note`/
  documents still skipped. A `TgUpdate` gains a `voice_file_id` field; a voice update has empty
  `text` + non-empty `voice_file_id`.
- **API shell (`cpr_telegram_api`):** add `get_file(file_id) -> file_path` (`getFile`) and
  `download_file(file_path) -> bytes` (GET `https://api.telegram.org/file/bot<token>/<file_path>`;
  token already redacted in logs).
- **Module:** `TelegramModule` gains `SttProvider* stt_ = nullptr` + `set_stt(SttProvider*)`
  (injected in `wire_agent`, like the LLM provider into LLMModule). In the poll loop, a voice update
  is handled by `handle_voice_(u)`:
  1. `stt_ == nullptr` → text reply `voice input isn't enabled`, done.
  2. `getFile` + download → write to a temp file (`std::filesystem::temp_directory_path` + unique
     name, `.oga`; ffmpeg/whisper read any format). RAII-unlinked on all exits.
  3. `stt_->transcribe(temp)` → on `ok` with non-empty text: `drive_turn_(chat_id,
     nlohmann::json(text), "USER_MESSAGE")` (identical to `handle_text_`). Runs **on the poll thread** (already off the
     bus) → **no Executor**. The whole block is try/catch-guarded.
- Auth/DM guards unchanged: allowlist + `chat_id == from_id` apply to voice exactly as to text.

### Data flow

```
phone 🎤 → Telegram voice msg
  → poll thread: getFile + download .oga → temp file
  → SttProvider.transcribe   (http PPQ whisper | command whisper.cpp)
  → text → lock TurnGate + post USER_MESSAGE
  → normal turn (this agent's objectives/gates) → text reply
  (TTS = separate later spec)
```

### Wiring / teardown (`app/agent_wiring.{h,cpp}`)

- `Agent` owns `std::unique_ptr<SttProvider> stt` (built from the `Stt` block via `resolve_stt`).
- **Load-bearing order:** `stt` is declared **before** `telegram` (telegram is the LAST member →
  destroyed FIRST; its dtor joins the poll thread, which may be mid-`transcribe` touching `stt_`).
  Declared-before → destroyed-after → the provider outlives the poll thread. (Same teardown
  discipline as `executor`/`bridge`.)
- `wire_agent`: if `a.stt && a.telegram` → `a.telegram->set_stt(a.stt.get())` before `on_attach`.
- The test `build_agent(bb, provider, …)` overload leaves `stt == nullptr` (embedding precedent —
  existing tests unaffected).

## Error handling (fail-soft — never crash the poll loop, never start a turn on garbage)

- Transcription fails (HTTP non-2xx / subprocess non-zero / timeout / unparseable) → text reply
  `couldn't transcribe your voice message: <short err>`; **no** `USER_MESSAGE` posted.
- Empty transcript (silence) → text reply `didn't catch that`; no empty turn.
- `getFile`/download fails → same text-reply path.
- Temp file **always** unlinked, even on error (RAII).
- No `Stt` block but a voice arrives → text nudge `voice input isn't enabled`.
- The provider `transcribe` NEVER throws (typed guards, try/catch); the module's voice block is also
  try/catch-guarded (existing per-turn pattern).

## Testing

- **`HttpSttProvider`** — injected-response seam (no real socket; the glibc-getaddrinfo TSan caveat
  applies, use cpr): parse `{"text":…}`; non-2xx → `ok:false`; missing `text` → `ok:false`.
- **`CommandSttProvider`** — fake wrapper script echoes a transcript (ok); fail cases: non-zero
  exit, empty stdout, timeout → `ok:false`.
- **Telegram voice parse** — `message.voice.file_id` captured into `voice_file_id`; text/photo/
  sticker still skipped; a voice update has empty `text`.
- **Telegram flow** — inject a **fake `SttProvider`**: voice update → `USER_MESSAGE` posted with the
  transcript; failing provider → text reply + **no** `USER_MESSAGE`; `stt_==null` → nudge, no turn.
- **Wiring** — `Stt` block → provider built + injected; no block → `stt` null + voice skipped;
  `provider=http` without `endpoint` / `provider=command` without `command` / unknown provider →
  `MalConfig`; whitespace command splits into argv.
- Full suite + **TSan lane** green (baseline **381**).

## Non-goals / v2 seams

- **TTS** — separate later spec (the agent talking back).
- **Local-mic front-end** — the seam is source-agnostic and ready; not wired in v1.
- **Bridge/peer path** — text-only agent↔agent; no STT (hard scope boundary).
- **Web-UI voice input** — same seam later.
- `voice` only — `audio` / `video_note` / document uploads skipped.
- **No warm persistent child** — one-shot per clip; warm mode is a v2 seam if latency bites.
- **English only** (`language = en`) — auto-detect / multi-language deferred.
- No streaming/partial transcription, no diarization, no timestamps.
- Transcript posted as **plain text** (not marked "(voice)") — marking is a trivial v2 tweak.
