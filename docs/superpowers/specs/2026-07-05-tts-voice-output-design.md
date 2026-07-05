# TTS voice output — design

**Date:** 2026-07-05
**Status:** Approved (Vaios — brainstorm 2026-07-05, three sections approved)
**Branch:** `feat/voice-tts` (off `main` @ `4c93b69`)

## Problem

STT (shipped, live) lets a user talk TO the agent. TTS is the other voice half — the agent talks
BACK. When a user sends a Telegram **voice** message, the reply should come back as a **voice note**
too, not just text. This is the second of the two independent voice halves; it builds on STT's
front-end plumbing and mirrors its provider-seam design on the OUTPUT side.

## Decisions (Vaios)

- **Trigger = mirror modality.** Voice-in → voice-out: a turn that *originated* from a voice message
  gets a spoken reply (**plus** the text); a typed turn stays text-only (no TTS, no cost). Reuses the
  turn-origin state already in the module. (Rejected: always-speak — noisy + costs TTS every turn;
  toggle command — extra session state.)
- **Both transports behind one seam** (like STT): `provider = http` (OpenAI-compat `/audio/speech`)
  + `provider = command` (local wrapper, e.g. piper). Manifest selects.
- **Text is the anchor, TTS is a best-effort bonus** — the text reply is ALWAYS sent; a TTS failure
  is silent (skip the voice note, text stands).
- **English only v1** (voice/model config); user-facing front-ends only, **Bridge excluded**.

## Architecture

TTS is STT mirrored on the OUTPUT side — a provider seam injected into the front-end, synthesis on
the poll thread (off-bus, no Executor).

### The seam (`include/hades/tts/`, `src/tts/`)

```cpp
struct TtsResult { bool ok = false; std::string audio; std::string error; };  // audio = ogg-opus bytes

class TtsProvider {                       // include/hades/tts/provider.h
 public:
  virtual ~TtsProvider() = default;
  // Synthesize `text` to ogg-opus audio bytes. NEVER throws — any failure returns
  // {ok:false, error:…} (fail-soft; a bad synth must never crash the poll loop).
  virtual TtsResult synthesize(const std::string& text) = 0;
};
```

Two implementations in **one TU** `src/tts/tts_providers.cpp` (mirrors STT's `stt_providers.cpp`):

1. **`HttpTtsProvider`** — `POST <endpoint>/audio/speech` with a JSON body
   `{model, input:text, voice, response_format:"opus"}`, `Authorization: Bearer <key>` → the response
   body is **raw ogg-opus bytes** (not JSON). **Reuses the existing `HttpClient`/`cpr_http` seam**
   (`include/hades/llm/http.h`) — `/audio/speech` is a plain JSON POST returning bytes, so no custom
   multipart client is needed (STT needed one; TTS does not). `HttpResponse.body` is byte-safe
   (`std::string`). Fail-soft: non-2xx / empty body / transport throw → `{ok:false}`. **Endpoint is the
   BASE url**, provider appends `/audio/speech` (same base-url gotcha as embedding's `/embeddings` and
   STT's `/audio/transcriptions`). `response_format` fixed to `opus` in v1.
2. **`CommandTtsProvider`** — `run_subprocess(argv, text, timeout_s)` (core, no shell): the reply
   **text is fed on stdin**, **ogg-opus bytes read from stdout**. non-zero exit / timeout / empty
   stdout → `{ok:false}`. Ship a reference wrapper `tools/piper_reference.sh` (piper → wav →
   `ffmpeg -f ogg -c:a libopus` → ogg-opus on stdout), documented like STT's whisper wrapper. One-shot
   per reply — no warm child.

### Telegram integration (`src/apps/telegram/`, `include/hades/telegram/`)

- **API:** `TelegramApi::send_voice(chat_id, ogg_bytes) -> bool` — `sendVoice` multipart
  (`voice` = the audio file). `CprTelegramApi` writes the bytes to a temp `.ogg` (RAII-unlinked),
  `cpr::Multipart{{"chat_id", …}, {"voice", cpr::File{tmp}}}`, `Redirect{false}`, logs method only.
- **Module:** `TelegramModule` gains `TtsProvider* tts_ = nullptr` + `set_tts(TtsProvider*)`, a
  `double tts_max_chars_` (default 4000), and a per-turn `bool speak_reply_` flag. `handle_voice_`
  sets `speak_reply_ = true` before `drive_turn_`; `handle_text_`/`handle_callback_` leave it false.
  In `drive_turn_`, after `got_reply_` and **after `send_reply_(text)` is sent** (the anchor): if
  `speak_reply_ && tts_ && last_reply_.size() <= tts_max_chars_`, call `tts_->synthesize(last_reply_)`
  → on `ok`, `api_->send_voice(chat_id, audio)`. `speak_reply_` is reset each turn (the existing RAII
  `Reset` guard, alongside `my_turn_`). All best-effort + try/catch-guarded: a synth/send failure logs
  to stderr and leaves the already-sent text as the reply. Runs **on the poll thread** (no Executor).

### Data flow

```
voice turn: STT → USER_MESSAGE → turn → reply text ready
  → send_reply_(text)                         # ALWAYS (anchor)
  → if voice-origin + tts_ + len<=max_chars:
      TtsProvider.synthesize(text)            # http /audio/speech opus | command piper→ogg-opus
      → temp .ogg → sendVoice → 🔊            # best-effort; fail → text already delivered
typed turn: speak_reply_ false → text only, no TTS.
```

### Wiring / teardown (`app/agent_wiring.{h,cpp}`)

- `Agent` owns `std::unique_ptr<TtsProvider> tts`, built from the `Tts` block via `resolve_tts`.
- **Load-bearing order:** `tts` declared **before `telegram`** (telegram is destroyed FIRST; its dtor
  joins the poll thread, which may be mid-`synthesize()` touching `tts_`). Declared-before →
  destroyed-after → the provider outlives the poll thread. Sits next to `stt` (both before telegram).
- `wire_agent`: if `a.tts && a.telegram` → `a.telegram->set_tts(a.tts.get())` before `on_attach`
  (before `hades_main` starts the poll thread). Test `build_agent` overload leaves `tts == nullptr`.

## Config (`Tts` block, opt-in, mirrors `Stt`)

```
Tts
{
  provider    = http                       # http | command
  endpoint    = https://api.openai.com/v1  # BASE url; provider appends /audio/speech (PPQ if it exposes TTS)
  model       = gpt-4o-mini-tts            # or tts-1
  voice       = alloy
  api_key_env = HADES_API_KEY
  max_chars   = 4000                        # replies longer than this skip TTS (text still sent)
  timeout_s   = 60
  # command   = ./tools/piper_reference.sh  # provider = command instead
}
```

- `resolve_tts(const Block&) -> std::unique_ptr<TtsProvider>` in wiring. `provider=http` requires
  `endpoint`; `provider=command` requires `command`; unknown provider → `MalConfig` (fail-loud).
- Secret via `api_key_env` **only**, never in the manifest; redacted in `session.log`.
- `command` whitespace-split into argv (text on stdin, so no path arg appended — unlike STT).
- **Inert unless the `Tts` block is present** → `Agent.tts == nullptr` → replies stay text-only, zero
  coupling. dev.hades ships the block **commented**.

## Error handling (fail-soft — text is the anchor)

- The text reply is **always sent first**. TTS is a bonus: synthesize fails (non-2xx / empty bytes /
  subprocess non-zero / timeout / transport throw) → **silently skip** the voice note (log to stderr),
  text already delivered. No error reply.
- `synthesize` NEVER throws (typed guards + try/catch); the module's voice-send block is
  try/catch-guarded; the temp `.ogg` is **always** unlinked.
- `send_voice` (Telegram) failure → logged, text stands.
- Reply longer than `max_chars` → skip TTS (text still sent) — avoids a huge-dump synth.
- **Format:** the provider owns ogg-opus (http `response_format=opus`; command wrapper emits it). A
  non-opus payload → `sendVoice` rejects → logged, no voice, text stands. The module does NOT
  transcode or fall back to `sendAudio` (v2 seam).

## Testing

- **`HttpTtsProvider`** — injected fake `HttpClient`: 2xx + bytes → `ok` with those bytes; non-2xx →
  `ok:false`; empty body → `ok:false`; transport throw → `ok:false`. Verify the request body carries
  `input`/`voice`/`response_format:opus` and the URL ends `/audio/speech`.
- **`CommandTtsProvider`** — fake wrapper script echoes canned bytes from stdin; fail cases: non-zero
  exit, empty stdout, timeout → `ok:false`; verify the text reaches the child on stdin.
- **Telegram flow** — inject a fake `TtsProvider` + fake api: a voice-origin turn → text sent **and**
  synthesize called **and** `send_voice` called; a typed turn → text only, **no** synthesize; TTS
  `ok:false` → text sent, **no** `send_voice`; reply > `max_chars` → text only, no synthesize.
- **Wiring** — `Tts` block → provider built + injected; no block → `tts` null; `provider=http` without
  `endpoint` / `provider=command` without `command` / unknown provider → `MalConfig`.
- Full suite + **TSan lane** green (baseline **405**).

## Non-goals / v2 seams

- Module-side transcode / `sendAudio` fallback for non-opus audio.
- Streaming TTS (synth arrives whole).
- SSML / voice-cloning / per-turn voice switching.
- `response_format` other than `opus`.
- English-only voices v1 (voice/model are config; no auto voice selection).
- Local-mic front-end (the seam is source-agnostic and ready; not wired).
- Web-UI audio playback (same seam later).
- **Bridge/peer path** — no voice for peers (hard scope boundary).
