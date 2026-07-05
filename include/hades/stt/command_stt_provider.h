// include/hades/stt/command_stt_provider.h — local STT via a subprocess wrapper (whisper.cpp)
//
// Runs `argv + [audio_path]` with run_subprocess (fork/exec, NO shell) and reads the plain
// transcript from stdout (trimmed). One-shot per clip — no warm child (voice notes are human-paced).
// Fail-soft: empty argv, non-zero exit, timeout, or empty stdout -> {ok:false}. Language is baked into
// the wrapper (English v1); run_subprocess passes no env, so it is not threaded here.
#pragma once
#include <string>
#include <vector>
#include "hades/stt/provider.h"
namespace hades {
class CommandSttProvider : public SttProvider {
 public:
  CommandSttProvider(std::vector<std::string> argv, double timeout_s);
  SttResult transcribe(const std::string& audio_path) override;

 private:
  std::vector<std::string> argv_;
  double timeout_s_;
};
}  // namespace hades
