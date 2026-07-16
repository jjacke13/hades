// tests/test_extract.cpp — pure auto-extract helpers: reply parse, digest build, artifact gate
#include <gtest/gtest.h>
#include <string>
#include "hades/extract/extract.h"
using namespace hades;

TEST(Extract, ParsesJsonArrayOfStrings) {
  auto v = parse_extract_reply(R"(["user prefers metric", "timezone is EET"])", 3);
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v[0], "user prefers metric");
  EXPECT_EQ(v[1], "timezone is EET");
}

TEST(Extract, NoneAndGarbageYieldEmpty) {
  EXPECT_TRUE(parse_extract_reply("NONE", 3).empty());
  EXPECT_TRUE(parse_extract_reply("  none \n", 3).empty());
  EXPECT_TRUE(parse_extract_reply("", 3).empty());
  EXPECT_TRUE(parse_extract_reply("I think the user likes metric.", 3).empty());  // prose, not JSON
  EXPECT_TRUE(parse_extract_reply(R"({"facts":["x"]})", 3).empty());              // object, not array
  EXPECT_TRUE(parse_extract_reply("[1, 2, 3]", 3).empty());                       // non-string items dropped
}

TEST(Extract, ToleratesFencedBlockAndMixedItems) {
  auto v = parse_extract_reply("```json\n[\"kept\", 42, \"also kept\"]\n```", 5);
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v[0], "kept");
  EXPECT_EQ(v[1], "also kept");
}

TEST(Extract, ClampsCapsAndCleans) {
  auto v = parse_extract_reply(R"(["a", "b", "c", "d"])", 2);
  EXPECT_EQ(v.size(), 2u);                                        // max_facts clamp
  auto w = parse_extract_reply("[\"  line1\\nline2  \", \"\", \"   \"]", 5);
  ASSERT_EQ(w.size(), 1u);                                        // empties dropped
  EXPECT_EQ(w[0], "line1 line2");                                 // trimmed, newline -> space
  std::string big(1000, 'x');
  auto u = parse_extract_reply("[\"" + big + "\"]", 5);
  ASSERT_EQ(u.size(), 1u);
  EXPECT_EQ(u[0].size(), 500u);                                   // 500-char cap
}

TEST(Extract, DigestBuildsAndTruncates) {
  EXPECT_EQ(build_extract_digest("hi", "hello"), "U: hi\nA: hello");
  const std::string d = build_extract_digest(std::string(3000, 'u'), std::string(3000, 'a'));
  EXPECT_EQ(d.size(), std::string("U: \nA: ").size() + 2000 + 2000);
}

TEST(Extract, ArtifactGate) {
  for (const char* a : {"[blocked: rm -rf]", "[declined by user]",
                        "[stopped: reached max tool steps]", "[timed out]", "[new session]"})
    EXPECT_TRUE(is_turn_artifact(a)) << a;
  EXPECT_FALSE(is_turn_artifact("[1] citation style answer"));
  EXPECT_FALSE(is_turn_artifact("normal answer"));
  EXPECT_FALSE(is_turn_artifact(""));
}
