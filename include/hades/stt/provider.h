// include/hades/stt/provider.h — speech-to-text provider seam (audio path -> text)
//
// Source-agnostic: a front-end downloads/captures an audio clip to a file, then asks a provider to
// transcribe it. Two transports implement this (HTTP OpenAI-compat, local command). transcribe
// NEVER throws — any failure returns {ok:false, error:…} (fail-soft, so a bad clip cannot crash a
// front-end poll loop). Language is fixed at construction (English v1), not a per-call argument.
#pragma once
#include <string>
namespace hades {
struct SttResult {
  bool ok = false;
  std::string text;       // the transcript (valid iff ok)
  std::string language;   // detected/echoed language, when the backend reports it (optional)
  std::string error;      // human-readable reason (valid iff !ok)
};
class SttProvider {
 public:
  virtual ~SttProvider() = default;
  virtual SttResult transcribe(const std::string& audio_path) = 0;
};
}  // namespace hades
