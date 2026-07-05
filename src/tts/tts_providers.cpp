// src/tts/tts_providers.cpp — TTS providers: OpenAI-compat HTTP + local command (fail-soft, never throw)
#include "hades/tts/http_tts_provider.h"
#include "hades/tts/command_tts_provider.h"
#include <string>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"   // run_subprocess
namespace hades {

// ── HttpTtsProvider: JSON POST to <endpoint>/audio/speech, body = raw ogg-opus bytes ──
HttpTtsProvider::HttpTtsProvider(std::string e, std::string k, std::string m, std::string v,
                                 HttpClient h)
    : endpoint_(std::move(e)), key_(std::move(k)), model_(std::move(m)), voice_(std::move(v)),
      http_(std::move(h)) {}

TtsResult HttpTtsProvider::synthesize(const std::string& text) {
  try {
    nlohmann::json body{{"model", model_}, {"input", text}, {"voice", voice_},
                        {"response_format", "opus"}};
    HttpResponse resp = http_(endpoint_ + "/audio/speech",
                              {{"Authorization", "Bearer " + key_},
                               {"Content-Type", "application/json"}},
                              body.dump());
    if (resp.status < 200 || resp.status >= 300)
      return {false, "", "tts http status " + std::to_string(resp.status)};
    if (resp.body.empty()) return {false, "", "tts http: empty audio"};
    return {true, resp.body, ""};
  } catch (...) {
    return {false, "", "tts http internal error"};
  }
}

// ── CommandTtsProvider: text on stdin -> ogg-opus bytes on stdout (binary, no trim) ──
CommandTtsProvider::CommandTtsProvider(std::vector<std::string> argv, double timeout_s)
    : argv_(std::move(argv)), timeout_s_(timeout_s) {}

TtsResult CommandTtsProvider::synthesize(const std::string& text) {
  if (argv_.empty()) return {false, "", "tts command not configured"};
  try {
    ProcResult pr = run_subprocess(argv_, text, timeout_s_);   // reply text on stdin
    if (pr.timed_out) return {false, "", "tts command timed out"};
    if (pr.code != 0) return {false, "", "tts command exit " + std::to_string(pr.code)};
    if (pr.out.empty()) return {false, "", "tts command produced no audio"};
    return {true, pr.out, ""};   // raw stdout bytes — do NOT trim (binary ogg-opus)
  } catch (...) {
    return {false, "", "tts command internal error"};
  }
}
}  // namespace hades
