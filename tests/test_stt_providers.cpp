// tests/test_stt_providers.cpp — STT providers: HTTP (injected transport) + command (real subprocess)
#include <gtest/gtest.h>
#include <unistd.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include "hades/stt/http_stt_provider.h"
#include "hades/stt/command_stt_provider.h"
using namespace hades;
namespace fs = std::filesystem;

// ---- HttpSttProvider (fake SttHttpClient, no socket) ----
TEST(HttpSttProvider, ParsesTextField) {
  HttpSttProvider p("https://x/v1", "k", "whisper-1", "en",
    [&](const std::string& url, const std::string& bearer, const std::string& audio,
        const std::vector<std::pair<std::string,std::string>>& fields) {
      EXPECT_NE(url.find("/audio/transcriptions"), std::string::npos);
      EXPECT_EQ(bearer, "k");
      EXPECT_EQ(audio, "/tmp/clip.oga");
      bool has_model = false, has_lang = false;
      for (auto& [f, v] : fields) { if (f == "model") has_model = true; if (f == "language") has_lang = true; }
      EXPECT_TRUE(has_model);
      EXPECT_TRUE(has_lang);
      return HttpResponse{200, R"({"text":"hello world","language":"english"})"};
    });
  SttResult r = p.transcribe("/tmp/clip.oga");
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.text, "hello world");
  EXPECT_EQ(r.language, "english");
}

TEST(HttpSttProvider, NonOkStatusIsSoftError) {
  HttpSttProvider p("https://x/v1", "k", "m", "en",
    [](auto, auto, auto, auto) { return HttpResponse{500, "boom"}; });
  SttResult r = p.transcribe("/tmp/x.oga");
  EXPECT_FALSE(r.ok);
  EXPECT_FALSE(r.error.empty());
}

TEST(HttpSttProvider, MalformedJsonOrMissingTextIsSoftError) {
  HttpSttProvider p1("https://x/v1", "k", "m", "en",
    [](auto, auto, auto, auto) { return HttpResponse{200, "not json"}; });
  EXPECT_FALSE(p1.transcribe("/tmp/x.oga").ok);
  HttpSttProvider p2("https://x/v1", "k", "m", "en",
    [](auto, auto, auto, auto) { return HttpResponse{200, R"({"nope":1})"}; });
  EXPECT_FALSE(p2.transcribe("/tmp/x.oga").ok);
}

TEST(HttpSttProvider, EmptyLanguageOmitsField) {
  HttpSttProvider p("https://x/v1", "k", "m", "",   // language empty -> no language field
    [](const std::string&, const std::string&, const std::string&,
       const std::vector<std::pair<std::string,std::string>>& fields) {
      for (auto& [f, v] : fields) EXPECT_NE(f, "language");
      return HttpResponse{200, R"({"text":"x"})"};
    });
  EXPECT_TRUE(p.transcribe("/tmp/x.oga").ok);
}

TEST(HttpSttProvider, TransportThrowIsSoftError) {
  HttpSttProvider p("https://x/v1", "k", "m", "en",
    [](auto, auto, auto, auto) -> HttpResponse { throw std::runtime_error("net"); });
  EXPECT_FALSE(p.transcribe("/tmp/x.oga").ok);   // provider must catch, never propagate
}

// ---- CommandSttProvider (real wrapper scripts) ----
static std::string write_script(const char* tag, const std::string& body) {
  const std::string path =
      (fs::path(::testing::TempDir()) /
       ("stt_" + std::string(tag) + "_" + std::to_string(::getpid()) + ".sh")).string();
  std::ofstream f(path);
  f << "#!/bin/sh\n" << body;
  f.close();
  ::chmod(path.c_str(), 0755);
  return path;
}

TEST(CommandSttProvider, ReturnsTrimmedStdout) {
  const std::string s = write_script("ok", "echo '  transcribed text  '\n");
  CommandSttProvider p({s}, 30.0);
  SttResult r = p.transcribe("/tmp/ignored.oga");
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.text, "transcribed text");   // leading/trailing whitespace trimmed
}

TEST(CommandSttProvider, NonZeroExitIsSoftError) {
  const std::string s = write_script("fail", "exit 3\n");
  EXPECT_FALSE(CommandSttProvider({s}, 30.0).transcribe("/tmp/x.oga").ok);
}

TEST(CommandSttProvider, EmptyStdoutIsSoftError) {
  const std::string s = write_script("empty", "printf ''\n");
  EXPECT_FALSE(CommandSttProvider({s}, 30.0).transcribe("/tmp/x.oga").ok);
}

TEST(CommandSttProvider, TimeoutIsSoftError) {
  const std::string s = write_script("slow", "sleep 5\n");
  EXPECT_FALSE(CommandSttProvider({s}, 0.2).transcribe("/tmp/x.oga").ok);   // killed, soft error
}

TEST(CommandSttProvider, EmptyArgvIsSoftError) {
  EXPECT_FALSE(CommandSttProvider({}, 30.0).transcribe("/tmp/x.oga").ok);   // no command configured
}
