// include/hades/tts/provider.h — text-to-speech provider seam (text -> ogg-opus audio bytes)
//
// The output mirror of the STT seam: a front-end hands reply text to a provider and gets ogg-opus
// audio back (which Telegram sendVoice accepts directly). synthesize NEVER throws — any failure
// returns {ok:false, error:…} (fail-soft; TTS is a best-effort bonus, the text reply already stands).
// Language/voice are fixed at construction (English v1), not per-call.
#pragma once
#include <string>
namespace hades {
struct TtsResult {
  bool ok = false;
  std::string audio;   // ogg-opus bytes (valid iff ok)
  std::string error;   // human-readable reason (valid iff !ok)
};
class TtsProvider {
 public:
  virtual ~TtsProvider() = default;
  virtual TtsResult synthesize(const std::string& text) = 0;
};
}  // namespace hades
