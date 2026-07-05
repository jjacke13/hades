// tests/test_telegram_parse.cpp — pure Telegram parse/builder helpers
#include <gtest/gtest.h>
#include <string>
#include "hades/telegram/parse.h"
using namespace hades;

TEST(TelegramParse, ParsesTextMessage) {
  const std::string body = R"({"ok":true,"result":[
    {"update_id":7,"message":{"from":{"id":42},"chat":{"id":-100},"text":"hello"}}]})";
  auto p = parse_updates(body);
  ASSERT_TRUE(p.ok);
  ASSERT_EQ(p.updates.size(), 1u);
  EXPECT_EQ(p.updates[0].kind, "message");
  EXPECT_EQ(p.updates[0].update_id, 7);
  EXPECT_EQ(p.updates[0].from_id, 42);
  EXPECT_EQ(p.updates[0].chat_id, -100);
  EXPECT_EQ(p.updates[0].text, "hello");
}

TEST(TelegramParse, ParsesCallbackQuery) {
  const std::string body = R"({"ok":true,"result":[
    {"update_id":8,"callback_query":{"id":"cbq1","from":{"id":42},"data":"approve:c1",
     "message":{"chat":{"id":-100}}}}]})";
  auto p = parse_updates(body);
  ASSERT_TRUE(p.ok);
  ASSERT_EQ(p.updates.size(), 1u);
  EXPECT_EQ(p.updates[0].kind, "callback");
  EXPECT_EQ(p.updates[0].callback_id, "cbq1");
  EXPECT_EQ(p.updates[0].callback_data, "approve:c1");
  EXPECT_EQ(p.updates[0].from_id, 42);
  EXPECT_EQ(p.updates[0].chat_id, -100);
}

TEST(TelegramParse, SkipsNonTextAndMalformedEntries) {
  const std::string body = R"({"ok":true,"result":[
    {"update_id":9,"message":{"from":{"id":1},"chat":{"id":2}}},
    {"update_id":10,"message":{"from":"bad","chat":{"id":2},"text":"x"}},
    {"update_id":11},
    {"update_id":12,"message":{"from":{"id":5},"chat":{"id":6},"text":"good"}}]})";
  auto p = parse_updates(body);                       // photo/malformed entries skipped
  ASSERT_TRUE(p.ok);
  ASSERT_EQ(p.updates.size(), 1u);
  EXPECT_EQ(p.updates[0].text, "good");
}

TEST(TelegramParse, BadBodyIsNotOkAndNeverThrows) {
  EXPECT_FALSE(parse_updates("not json").ok);
  EXPECT_FALSE(parse_updates(R"({"ok":false})").ok);
  EXPECT_FALSE(parse_updates(R"({"ok":"true","result":[]})").ok);   // non-bool ok: no throw
  EXPECT_FALSE(parse_updates(R"({"ok":1,"result":[]})").ok);
  EXPECT_FALSE(parse_updates("").ok);
}

TEST(TelegramParse, SplitMessageRespectsLimit) {
  EXPECT_TRUE(split_message("").empty());
  EXPECT_EQ(split_message(std::string(4096, 'a')).size(), 1u);
  auto two = split_message(std::string(4097, 'a'));
  ASSERT_EQ(two.size(), 2u);
  EXPECT_EQ(two[0].size(), 4096u);
  EXPECT_EQ(two[1].size(), 1u);
  EXPECT_EQ(split_message("abcdef", 2), (std::vector<std::string>{"ab", "cd", "ef"}));
}

TEST(TelegramParse, BuildersProduceExactApiShapes) {
  auto msg = build_send_message(-100, "hi");
  EXPECT_EQ(msg["chat_id"], -100);
  EXPECT_EQ(msg["text"], "hi");
  auto conf = build_confirm_message(-100, "run shell?", "c1");
  EXPECT_EQ(conf["chat_id"], -100);
  EXPECT_EQ(conf["text"], "run shell?");
  const auto& row = conf["reply_markup"]["inline_keyboard"][0];
  EXPECT_EQ(row[0]["text"], "Approve");
  EXPECT_EQ(row[0]["callback_data"], "approve:c1");
  EXPECT_EQ(row[1]["text"], "Deny");
  EXPECT_EQ(row[1]["callback_data"], "deny:c1");
  EXPECT_EQ(build_answer_callback("cbq1")["callback_query_id"], "cbq1");
}

TEST(TelegramParse, VoiceMessageCapturesFileId) {
  std::string body = R"({"ok":true,"result":[{
    "update_id":7,
    "message":{"from":{"id":42},"chat":{"id":42},
               "voice":{"file_id":"AwACAgV","duration":3}}}]})";
  auto p = parse_updates(body);
  ASSERT_TRUE(p.ok);
  ASSERT_EQ(p.updates.size(), 1u);
  EXPECT_EQ(p.updates[0].kind, "message");
  EXPECT_TRUE(p.updates[0].text.empty());
  EXPECT_EQ(p.updates[0].voice_file_id, "AwACAgV");
  EXPECT_EQ(p.updates[0].from_id, 42);
  EXPECT_EQ(p.updates[0].chat_id, 42);
}

TEST(TelegramParse, VoiceWithoutFileIdIsSkipped) {
  std::string body = R"({"ok":true,"result":[{
    "update_id":8,
    "message":{"from":{"id":42},"chat":{"id":42},"voice":{"duration":3}}}]})";
  auto p = parse_updates(body);
  EXPECT_TRUE(p.ok);
  EXPECT_TRUE(p.updates.empty());   // no file_id -> nothing to fetch
}

TEST(TelegramParse, TextStillWinsAndPhotoStillSkipped) {
  std::string body = R"({"ok":true,"result":[
    {"update_id":9,"message":{"from":{"id":1},"chat":{"id":1},"text":"hi","voice":{"file_id":"X"}}},
    {"update_id":10,"message":{"from":{"id":1},"chat":{"id":1},"photo":[{"file_id":"P"}]}}]})";
  auto p = parse_updates(body);
  ASSERT_EQ(p.updates.size(), 1u);            // photo skipped
  EXPECT_EQ(p.updates[0].text, "hi");         // text preferred over voice when both present
  EXPECT_TRUE(p.updates[0].voice_file_id.empty());
}
