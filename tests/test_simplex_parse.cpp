// tests/test_simplex_parse.cpp — tolerant daemon-event parsing (canned frames, pure)
#include <gtest/gtest.h>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/simplex/api.h"
using namespace hades;

namespace {
// A canned newChatItems frame with one direct received text item.
std::string direct_text_frame(long long cid, const std::string& name, const std::string& text) {
  nlohmann::json item{
      {"chatInfo", {{"type", "direct"}, {"contact", {{"contactId", cid}, {"localDisplayName", name}}}}},
      {"chatItem",
       {{"chatDir", {{"type", "directRcv"}}},
        {"content", {{"type", "rcvMsgContent"}, {"msgContent", {{"type", "text"}, {"text", text}}}}}}}};
  nlohmann::json f{{"resp", {{"type", "newChatItems"}, {"chatItems", nlohmann::json::array({item})}}}};
  return f.dump();
}
}  // namespace

TEST(SimplexParse, DirectReceivedTextYieldsTextEvent) {
  auto evs = parse_simplex_events(direct_text_frame(2, "Vaios K", "hello agent"));
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].kind, SxEvent::Kind::Text);
  EXPECT_EQ(evs[0].contact_id, 2);
  EXPECT_EQ(evs[0].display_name, "Vaios K");
  EXPECT_EQ(evs[0].text, "hello agent");
}

TEST(SimplexParse, RightWrapperIsUnwrapped) {
  // Some CLI builds encode resp as a Haskell Either: {"resp":{"Right":{...}}}.
  nlohmann::json inner = nlohmann::json::parse(direct_text_frame(3, "N", "hi"))["resp"];
  nlohmann::json f{{"resp", {{"Right", inner}}}};
  auto evs = parse_simplex_events(f.dump());
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].contact_id, 3);
}

TEST(SimplexParse, OwnEchoGroupAndNonTextAreSkipped) {
  // directSnd (our own sent item), a group item, and a voice msgContent: all skipped.
  nlohmann::json snd{
      {"chatInfo", {{"type", "direct"}, {"contact", {{"contactId", 2}, {"localDisplayName", "V"}}}}},
      {"chatItem",
       {{"chatDir", {{"type", "directSnd"}}},
        {"content", {{"type", "rcvMsgContent"}, {"msgContent", {{"type", "text"}, {"text", "me"}}}}}}}};
  nlohmann::json grp{
      {"chatInfo", {{"type", "group"}, {"groupInfo", {{"groupId", 7}}}}},
      {"chatItem",
       {{"chatDir", {{"type", "groupRcv"}}},
        {"content", {{"type", "rcvMsgContent"}, {"msgContent", {{"type", "text"}, {"text", "grp"}}}}}}}};
  nlohmann::json voice{
      {"chatInfo", {{"type", "direct"}, {"contact", {{"contactId", 2}, {"localDisplayName", "V"}}}}},
      {"chatItem",
       {{"chatDir", {{"type", "directRcv"}}},
        {"content", {{"type", "rcvMsgContent"}, {"msgContent", {{"type", "voice"}, {"text", ""}}}}}}}};
  nlohmann::json f{{"resp", {{"type", "newChatItems"},
                             {"chatItems", nlohmann::json::array({snd, grp, voice})}}}};
  EXPECT_TRUE(parse_simplex_events(f.dump()).empty());
}

TEST(SimplexParse, MultipleItemsYieldMultipleEvents) {
  nlohmann::json a = nlohmann::json::parse(direct_text_frame(2, "V", "one"));
  nlohmann::json b = nlohmann::json::parse(direct_text_frame(5, "W", "two"));
  nlohmann::json f{{"resp", {{"type", "newChatItems"},
                             {"chatItems", nlohmann::json::array(
                                 {a["resp"]["chatItems"][0], b["resp"]["chatItems"][0]})}}}};
  auto evs = parse_simplex_events(f.dump());
  ASSERT_EQ(evs.size(), 2u);
  EXPECT_EQ(evs[0].text, "one");
  EXPECT_EQ(evs[1].contact_id, 5);
}

TEST(SimplexParse, ContactRequestEvent) {
  nlohmann::json f{{"resp", {{"type", "receivedContactRequest"},
                             {"contactRequest", {{"contactRequestId", 9},
                                                 {"localDisplayName", "stranger"}}}}}};
  auto evs = parse_simplex_events(f.dump());
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].kind, SxEvent::Kind::ContactRequest);
  EXPECT_EQ(evs[0].request_id, 9);
  EXPECT_EQ(evs[0].display_name, "stranger");
}

TEST(SimplexParse, ContactConnectedEvent) {
  nlohmann::json f{{"resp", {{"type", "contactConnected"},
                             {"contact", {{"contactId", 4}, {"localDisplayName", "friend"}}}}}};
  auto evs = parse_simplex_events(f.dump());
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].kind, SxEvent::Kind::Connected);
  EXPECT_EQ(evs[0].contact_id, 4);
  EXPECT_EQ(evs[0].display_name, "friend");
}

TEST(SimplexParse, GarbageAndUnknownTypesYieldNothing) {
  EXPECT_TRUE(parse_simplex_events("not json at all").empty());
  EXPECT_TRUE(parse_simplex_events("42").empty());
  EXPECT_TRUE(parse_simplex_events(R"({"resp":{"type":"somethingElse"}})").empty());
  EXPECT_TRUE(parse_simplex_events(R"({"resp":{"type":"newChatItems","chatItems":"nope"}})").empty());
  EXPECT_TRUE(parse_simplex_events(R"({"noresp":true})").empty());
}
