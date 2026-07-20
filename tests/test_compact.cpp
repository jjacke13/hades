// tests/test_compact.cpp — pure compaction helpers: span digest, sidecar codec, paths
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "hades/compact/compact.h"
using namespace hades;

static nlohmann::json msg(const char* role, const std::string& content) {
  return {{"role", role}, {"content", content}};
}

TEST(Compact, DigestKeepsRoleAndContent) {
  auto d = digest_span({msg("user", "hello"), msg("assistant", "hi there")});
  ASSERT_TRUE(d.is_array());
  ASSERT_EQ(d.size(), 2u);
  EXPECT_EQ(d[0]["role"], "user");
  EXPECT_EQ(d[0]["content"], "hello");
  EXPECT_EQ(d[1]["content"], "hi there");
}

TEST(Compact, DigestTruncatesPerMessageOnUtf8Boundary) {
  std::string big(998, 'a');
  big += "\xCE\xB1\xCE\xB1";                       // 2 two-byte codepoints straddling the cap
  auto d = digest_span({msg("user", big)}, 1000, 24000);
  const std::string c = d[0]["content"].get<std::string>();
  EXPECT_LE(c.size(), 1000u + 12u);                // cap + " (truncated)" marker
  EXPECT_NE(c.find(" (truncated)"), std::string::npos);
  EXPECT_NO_THROW(nlohmann::json(c).dump());       // strict dump = valid UTF-8 proof
}

TEST(Compact, DigestTotalCapDropsOldestWithMarker) {
  std::vector<nlohmann::json> span;
  for (int i = 0; i < 10; ++i)
    span.push_back(msg("user", "m" + std::to_string(i) + std::string(500, 'x')));
  auto d = digest_span(span, 1000, 2000);          // room for ~3-4 newest messages
  ASSERT_GE(d.size(), 2u);
  const std::string first = d[0]["content"].get<std::string>();
  EXPECT_NE(first.find("earlier messages omitted"), std::string::npos);
  const std::string last = d[d.size() - 1]["content"].get<std::string>();
  EXPECT_NE(last.find("m9"), std::string::npos);   // newest survives
  // no dropped-message content present
  for (const auto& e : d)
    EXPECT_EQ(e["content"].get<std::string>().find("m0x"), std::string::npos);
}

TEST(Compact, DigestNamesToolCallsAndToleratesNonStringContent) {
  nlohmann::json tc = {{"role", "assistant"}, {"content", nullptr},
                       {"tool_calls", nlohmann::json::array(
                            {{{"id", "c1"}, {"type", "function"},
                              {"function", {{"name", "web_search"}, {"arguments", "{}"}}}}})}};
  auto d = digest_span({tc, {{"role", "weird"}, {"content", 42}}});
  EXPECT_NE(d[0]["content"].get<std::string>().find("web_search"), std::string::npos);
  EXPECT_FALSE(d[1]["content"].get<std::string>().empty());   // dumped, not crashed
}

TEST(Compact, SidecarRoundTripAndTolerantParse) {
  const std::string s = serialize_summary_sidecar(7, "line one\nline two");
  EXPECT_EQ(s.rfind("upto: 7\n", 0), 0u);
  const SidecarSummary p = parse_summary_sidecar(s);
  EXPECT_EQ(p.upto, 7u);
  EXPECT_EQ(p.text, "line one\nline two");
  EXPECT_EQ(parse_summary_sidecar("").upto, 0u);
  EXPECT_TRUE(parse_summary_sidecar("garbage first line\n\nbody").text.empty());
  EXPECT_TRUE(parse_summary_sidecar("upto: notanumber\n\nbody").text.empty());
}

TEST(Compact, PathsDeriveFromSessionPath) {
  EXPECT_EQ(sidecar_path_for(".hades/sessions/20260720-1.jsonl"),
            ".hades/sessions/20260720-1.summary.md");
  EXPECT_EQ(sidecar_path_for(""), "");
  EXPECT_EQ(session_stem(".hades/sessions/20260720-1.jsonl"), "20260720-1");
  EXPECT_EQ(session_stem(""), "");
}
