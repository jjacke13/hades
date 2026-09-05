// tests/test_simplex_parse.cpp — tolerant daemon-event parsing (canned frames, pure)
#include <gtest/gtest.h>
#include <string>
#include <vector>
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

// ── voice + file-transfer frames ───────────────────────────────────────────────────────────────

TEST(SimplexParse, VoiceMessageWithFileYieldsVoiceEvent) {
  const std::string frame = R"({"resp":{"type":"newChatItems","chatItems":[{
    "chatInfo":{"type":"direct","contact":{"contactId":7,"localDisplayName":"vaios"}},
    "chatItem":{"chatDir":{"type":"directRcv"},
      "content":{"type":"rcvMsgContent","msgContent":{"type":"voice","text":"","duration":5}},
      "file":{"fileId":42,"fileName":"voice.m4a","fileSize":1234,
              "fileStatus":{"type":"rcvInvitation"}}}}]}})";
  const auto evs = parse_simplex_events(frame);
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].kind, SxEvent::Kind::Voice);
  EXPECT_EQ(evs[0].contact_id, 7);
  EXPECT_EQ(evs[0].display_name, "vaios");
  EXPECT_EQ(evs[0].file_id, 42);
  EXPECT_EQ(evs[0].file_size, 1234);
  EXPECT_EQ(evs[0].duration, 5);
  // The offered name travels so the module can give the temp file a real audio extension: the
  // default http STT backend validates the format by the uploaded file's NAME.
  EXPECT_EQ(evs[0].file_name, "voice.m4a");
}

// An absent/mistyped fileName is not an error — the module falls back to a default extension.
TEST(SimplexParse, VoiceMessageWithoutFileNameYieldsEmptyName) {
  const std::string frame = R"({"resp":{"type":"newChatItems","chatItems":[{
    "chatInfo":{"type":"direct","contact":{"contactId":7,"localDisplayName":"vaios"}},
    "chatItem":{"chatDir":{"type":"directRcv"},
      "content":{"type":"rcvMsgContent","msgContent":{"type":"voice","text":"","duration":5}},
      "file":{"fileId":42,"fileName":17,"fileSize":1234}}}]}})";
  const auto evs = parse_simplex_events(frame);
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].file_id, 42);
  EXPECT_TRUE(evs[0].file_name.empty());
}

TEST(SimplexParse, VoiceMessageWithoutFileIsDropped) {
  const std::string frame = R"({"resp":{"type":"newChatItems","chatItems":[{
    "chatInfo":{"type":"direct","contact":{"contactId":7,"localDisplayName":"vaios"}},
    "chatItem":{"chatDir":{"type":"directRcv"},
      "content":{"type":"rcvMsgContent","msgContent":{"type":"voice","text":"","duration":5}}}}]}})";
  EXPECT_TRUE(parse_simplex_events(frame).empty());
}

TEST(SimplexParse, RcvFileCompleteYieldsFileDone) {
  const std::string frame = R"({"resp":{"type":"rcvFileComplete","chatItem":{
    "chatInfo":{"type":"direct","contact":{"contactId":7,"localDisplayName":"vaios"}},
    "chatItem":{"chatDir":{"type":"directRcv"},
      "content":{"type":"rcvMsgContent","msgContent":{"type":"voice","text":"","duration":5}},
      "file":{"fileId":42,"fileName":"voice.m4a","fileSize":1234,
              "fileSource":{"filePath":"/tmp/voice.m4a"},
              "fileStatus":{"type":"rcvComplete"}}}}}})";
  const auto evs = parse_simplex_events(frame);
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].kind, SxEvent::Kind::FileDone);
  EXPECT_EQ(evs[0].file_id, 42);
}

// chatItem_ is OPTIONAL on the error frame, so the id must come from rcvFileTransfer.
TEST(SimplexParse, RcvFileErrorWithoutChatItemStillYieldsFileFailed) {
  const std::string frame = R"({"resp":{"type":"rcvFileError",
    "rcvFileTransfer":{"fileId":42,"senderDisplayName":"vaios"}}})";
  const auto evs = parse_simplex_events(frame);
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].kind, SxEvent::Kind::FileFailed);
  EXPECT_EQ(evs[0].file_id, 42);
}

// The sender cancelled AFTER we accepted: terminal too. Without it the transfer would sit in
// the pending table until eviction and the sender would get no reply at all.
TEST(SimplexParse, RcvFileAcceptedSndCancelledYieldsFileFailed) {
  const std::string frame = R"({"resp":{"type":"rcvFileAcceptedSndCancelled",
    "rcvFileTransfer":{"fileId":11,"senderDisplayName":"vaios"}}})";
  const auto evs = parse_simplex_events(frame);
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].kind, SxEvent::Kind::FileFailed);
  EXPECT_EQ(evs[0].file_id, 11);
  EXPECT_EQ(evs[0].display_name, "vaios");
}

TEST(SimplexParse, RcvFileSndCancelledYieldsFileFailed) {
  const std::string frame = R"({"resp":{"type":"rcvFileSndCancelled",
    "rcvFileTransfer":{"fileId":9}}})";
  const auto evs = parse_simplex_events(frame);
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].kind, SxEvent::Kind::FileFailed);
  EXPECT_EQ(evs[0].file_id, 9);
}

// Non-terminal: a warning must NOT evict a pending transfer.
TEST(SimplexParse, RcvFileWarningIsIgnored) {
  const std::string frame = R"({"resp":{"type":"rcvFileWarning",
    "rcvFileTransfer":{"fileId":9}}})";
  EXPECT_TRUE(parse_simplex_events(frame).empty());
}

// value(key, default) throws type_error.306 on a non-object receiver — parse must stay tolerant
// (it also runs inside WsSimplexApi::command_ok_, where a throw would break fail-soft commands).
TEST(SimplexParse, MalformedNestedObjectsNeverThrow) {
  for (const std::string frame : {
           R"({"resp":{"type":"rcvFileComplete","chatItem":null}})",
           R"({"resp":{"type":"rcvFileComplete","chatItem":"nope"}})",
           R"({"resp":{"type":"rcvFileComplete","chatItem":{"chatItem":null}}})",
           R"({"resp":{"type":"rcvFileComplete","chatItem":{"chatItem":42}}})",
           R"({"resp":{"type":"rcvFileComplete","chatItem":{"chatItem":{"file":7}}}})",
           R"({"resp":{"type":"newChatItems","chatItems":[{"chatItem":null}]}})",
           // pre-existing (predates voice): a DIRECT item whose chatItem is not an object.
           R"({"resp":{"type":"newChatItems","chatItems":[{"chatInfo":{"type":"direct",
                "contact":{"contactId":7}},"chatItem":null}]}})",
           R"({"resp":{"type":"newChatItems","chatItems":[{"chatInfo":{"type":"direct",
                "contact":{"contactId":7}},"chatItem":"nope"}]}})",
       }) {
    std::vector<SxEvent> evs;
    EXPECT_NO_THROW(evs = parse_simplex_events(frame)) << frame;
    EXPECT_TRUE(evs.empty()) << frame;
  }
}
