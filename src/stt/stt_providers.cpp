// src/stt/stt_providers.cpp — STT providers: OpenAI-compat HTTP + local command (fail-soft, never throw)
#include "hades/stt/http_stt_provider.h"
#include "hades/stt/command_stt_provider.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"   // run_subprocess
namespace hades {
namespace {
std::string trim(const std::string& s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  auto b = std::find_if(s.begin(), s.end(), not_space);
  auto e = std::find_if(s.rbegin(), s.rend(), not_space).base();
  return (b < e) ? std::string(b, e) : std::string{};
}
}  // namespace

// ── HttpSttProvider: multipart POST to <endpoint>/audio/transcriptions over the injected seam ──
HttpSttProvider::HttpSttProvider(std::string e, std::string k, std::string m, std::string lang,
                                 SttHttpClient h)
    : endpoint_(std::move(e)), key_(std::move(k)), model_(std::move(m)),
      language_(std::move(lang)), http_(std::move(h)) {}

SttResult HttpSttProvider::transcribe(const std::string& audio_path) {
  try {  // never-throw invariant: whole body fail-soft (fields alloc, transport, json parse)
    std::vector<std::pair<std::string, std::string>> fields{{"model", model_}};
    if (!language_.empty()) fields.push_back({"language", language_});
    HttpResponse resp = http_(endpoint_ + "/audio/transcriptions", key_, audio_path, fields);
    if (resp.status < 200 || resp.status >= 300)
      return {false, "", "", "stt http status " + std::to_string(resp.status)};
    auto j = nlohmann::json::parse(resp.body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return {false, "", "", "stt http: unparseable response"};
    auto t = j.find("text");
    if (t == j.end() || !t->is_string()) return {false, "", "", "stt http: no text field"};
    SttResult r;
    r.ok = true;
    r.text = t->get<std::string>();
    if (auto l = j.find("language"); l != j.end() && l->is_string()) r.language = l->get<std::string>();
    return r;
  } catch (...) {
    return {false, "", "", "stt http internal error"};
  }
}

SttHttpClient cpr_stt_http(double timeout_s) {
  return [timeout_s](const std::string& url, const std::string& bearer,
                     const std::string& audio_path,
                     const std::vector<std::pair<std::string, std::string>>& fields) {
    cpr::Multipart mp{{"file", cpr::File{audio_path}}};
    for (const auto& [k, v] : fields) mp.parts.emplace_back(k, v);
    cpr::Header hdr;
    if (!bearer.empty()) hdr["Authorization"] = "Bearer " + bearer;
    auto r = cpr::Post(cpr::Url{url}, hdr, mp,
                       cpr::Timeout{static_cast<int>(timeout_s * 1000)}, cpr::Redirect{false});
    return HttpResponse{r.status_code, r.text};
  };
}

// ── CommandSttProvider: run a local wrapper, read the transcript off stdout ──
CommandSttProvider::CommandSttProvider(std::vector<std::string> argv, double timeout_s)
    : argv_(std::move(argv)), timeout_s_(timeout_s) {}

SttResult CommandSttProvider::transcribe(const std::string& audio_path) {
  if (argv_.empty()) return {false, "", "", "stt command not configured"};
  try {  // never-throw invariant: guards a bad_alloc from the argv copy/push_back
    std::vector<std::string> argv = argv_;
    argv.push_back(audio_path);   // contract: last arg is the audio file
    ProcResult pr = run_subprocess(argv, "", timeout_s_);
    if (pr.timed_out) return {false, "", "", "stt command timed out"};
    if (pr.code != 0) return {false, "", "", "stt command exit " + std::to_string(pr.code)};
    std::string text = trim(pr.out);
    if (text.empty()) return {false, "", "", "stt command produced no transcript"};
    return {true, text, "", ""};
  } catch (...) {
    return {false, "", "", "stt command internal error"};
  }
}
}  // namespace hades
