// include/hades/tts/command_tts_provider.h — local TTS via a subprocess wrapper (piper)
//
// Runs `argv` with run_subprocess (fork/exec, NO shell), feeding the reply text on STDIN and reading
// ogg-opus bytes from STDOUT (binary — never trimmed). One-shot per reply. Fail-soft: empty argv,
// non-zero exit, timeout, or empty stdout -> {ok:false}. The wrapper owns format (must emit ogg-opus).
#pragma once
#include <string>
#include <vector>
#include "hades/tts/provider.h"
namespace hades {
class CommandTtsProvider : public TtsProvider {
 public:
  CommandTtsProvider(std::vector<std::string> argv, double timeout_s);
  TtsResult synthesize(const std::string& text) override;

 private:
  std::vector<std::string> argv_;
  double timeout_s_;
};
}  // namespace hades
