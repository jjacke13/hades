// tests/test_tts_providers.cpp — TTS providers: HTTP (injected HttpClient) + command (real subprocess)
#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include "hades/tts/http_tts_provider.h"
#include "hades/tts/command_tts_provider.h"
using namespace hades;
namespace fs = std::filesystem;

// ---- HttpTtsProvider (fake HttpClient, no socket) ----
TEST(HttpTtsProvider, ReturnsAudioBytesOn2xx) {
  HttpTtsProvider p("https://x/v1", "k", "tts-1", "alloy",
    [&](const std::string& url, const std::vector<std::pair<std::string,std::string>>& headers,
        const std::string& body) {
      EXPECT_NE(url.find("/audio/speech"), std::string::npos);
      EXPECT_NE(body.find("\"input\""), std::string::npos);
      EXPECT_NE(body.find("\"response_format\":\"opus\""), std::string::npos);
      EXPECT_NE(body.find("\"voice\":\"alloy\""), std::string::npos);
      bool auth = false;
      for (auto& [h, v] : headers) if (h == "Authorization") { auth = (v == "Bearer k"); }
      EXPECT_TRUE(auth);
      return HttpResponse{200, std::string("\x4f\x67\x67OPUSBYTES", 12)};   // fake ogg bytes
    });
  TtsResult r = p.synthesize("hello");
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.audio.size(), 12u);
}

TEST(HttpTtsProvider, NonOkStatusIsSoftError) {
  HttpTtsProvider p("https://x/v1", "k", "m", "v",
    [](auto, auto, auto) { return HttpResponse{500, "boom"}; });
  EXPECT_FALSE(p.synthesize("x").ok);
}

TEST(HttpTtsProvider, EmptyBodyIsSoftError) {
  HttpTtsProvider p("https://x/v1", "k", "m", "v",
    [](auto, auto, auto) { return HttpResponse{200, ""}; });
  EXPECT_FALSE(p.synthesize("x").ok);
}

TEST(HttpTtsProvider, TransportThrowIsSoftError) {
  HttpTtsProvider p("https://x/v1", "k", "m", "v",
    [](auto, auto, auto) -> HttpResponse { throw std::runtime_error("net"); });
  EXPECT_FALSE(p.synthesize("x").ok);   // provider must catch, never propagate
}

// ---- CommandTtsProvider (real wrapper scripts) ----
static std::string write_script(const char* tag, const std::string& body) {
  const std::string path =
      (fs::path(::testing::TempDir()) /
       ("tts_" + std::string(tag) + "_" + std::to_string(::getpid()) + ".sh")).string();
  std::ofstream f(path);
  f << "#!/bin/sh\n" << body;
  f.close();
  ::chmod(path.c_str(), 0755);
  return path;
}

TEST(CommandTtsProvider, ReturnsStdoutBytesAndPassesTextOnStdin) {
  // The wrapper echoes back whatever arrived on stdin -> proves the text reached the child + bytes returned raw.
  const std::string s = write_script("ok", "cat\n");
  CommandTtsProvider p({s}, 30.0);
  TtsResult r = p.synthesize("SPOKEN TEXT");
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.audio, "SPOKEN TEXT");   // raw, NOT trimmed (binary-safe passthrough)
}

TEST(CommandTtsProvider, NonZeroExitIsSoftError) {
  const std::string s = write_script("fail", "exit 4\n");
  EXPECT_FALSE(CommandTtsProvider({s}, 30.0).synthesize("x").ok);
}

TEST(CommandTtsProvider, EmptyStdoutIsSoftError) {
  const std::string s = write_script("empty", "printf ''\n");
  EXPECT_FALSE(CommandTtsProvider({s}, 30.0).synthesize("x").ok);
}

TEST(CommandTtsProvider, TimeoutIsSoftError) {
  const std::string s = write_script("slow", "sleep 5\n");
  EXPECT_FALSE(CommandTtsProvider({s}, 0.2).synthesize("x").ok);
}

TEST(CommandTtsProvider, EmptyArgvIsSoftError) {
  EXPECT_FALSE(CommandTtsProvider({}, 30.0).synthesize("x").ok);
}
