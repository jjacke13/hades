// tests/test_cron_store.cpp — pure cron.jsonl store: fold, compact, serialize, parse_at, id
#include <gtest/gtest.h>
#include <ctime>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/heartbeat/cron_store.h"
using namespace hades;

TEST(CronStore, FoldAddCancelDone) {
  std::string s =
      add_record({"a1", "one", "cron", "*/5 * * * *", 0, "p1", true, 100}) + "\n" +
      add_record({"a2", "two", "once", "", 200, "p2", false, 101}) + "\n" +
      cancel_record("a1") + "\n";
  auto v = fold_cron_store(s);
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0].id, "a2");
  EXPECT_EQ(v[0].kind, "once");
  EXPECT_EQ(v[0].fire_epoch, 200);
  // a `done` tombstone also removes it
  auto v2 = fold_cron_store(s + done_record("a2") + "\n");
  EXPECT_TRUE(v2.empty());
}

TEST(CronStore, TolerantOfTornAndBlankLines) {
  std::string s = "\n{ this is not json\n" +
                  add_record({"a1", "n", "cron", "* * * * *", 0, "p", false, 5}) + "\n" +
                  "{\"op\":\"add\"}\n";   // no id -> skipped
  auto v = fold_cron_store(s);
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0].id, "a1");
}

TEST(CronStore, AddRecordJsonShape) {
  auto j = nlohmann::json::parse(add_record({"a1", "n", "cron", "0 9 * * *", 0, "hi", true, 7}));
  EXPECT_EQ(j["op"], "add");
  EXPECT_EQ(j["kind"], "cron");
  EXPECT_EQ(j["schedule"], "0 9 * * *");
  EXPECT_TRUE(j["fire_epoch"].is_null());     // cron -> null fire_epoch
  auto j2 = nlohmann::json::parse(add_record({"a2", "n", "once", "", 123, "hi", false, 7}));
  EXPECT_TRUE(j2["schedule"].is_null());      // once -> null schedule
  EXPECT_EQ(j2["fire_epoch"], 123);
}

TEST(CronStore, CompactDropsTombstoned) {
  std::string s = add_record({"a1", "n", "cron", "* * * * *", 0, "p", false, 1}) + "\n" +
                  add_record({"a2", "n", "once", "", 9, "p", false, 2}) + "\n" +
                  cancel_record("a1") + "\n";
  std::string c = compact_cron_store(s);
  auto v = fold_cron_store(c);
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0].id, "a2");
  EXPECT_EQ(c.find("cancel"), std::string::npos);   // no tombstone survives compaction
}

TEST(CronStore, ParseAtIsoAndHhmm) {
  // ISO absolute: round-trips through local mktime/localtime
  auto e = parse_at("2030-06-15T09:30", 0);
  ASSERT_TRUE(e.has_value());
  std::time_t tt = static_cast<std::time_t>(*e);
  std::tm lt{}; localtime_r(&tt, &lt);
  EXPECT_EQ(lt.tm_hour, 9);
  EXPECT_EQ(lt.tm_min, 30);
  EXPECT_EQ(lt.tm_year, 130);
  // bare HH:MM -> strictly in the future relative to now
  long long now = 1000000000;   // fixed reference
  auto h = parse_at("08:00", now);
  ASSERT_TRUE(h.has_value());
  EXPECT_GT(*h, now);
  EXPECT_LE(*h - now, 24 * 3600);   // within the next day
  EXPECT_FALSE(parse_at("not-a-time", now).has_value());
  EXPECT_FALSE(parse_at("25:99", now).has_value());
}

TEST(CronStore, MakeTaskIdFormat) {
  EXPECT_EQ(make_task_id(1751900000, 0xa3f9), "t1751900000-a3f9");
  EXPECT_EQ(make_task_id(5, 0x000f), "t5-000f");
}

TEST(CronStore, FoldSkipsTypeCorruptLineWithoutThrowing) {
  std::string s = add_record({"a1", "one", "cron", "*/5 * * * *", 0, "p1", true, 100}) + "\n" +
                  std::string(R"({"op":"add","id":"bad","name":123,"kind":"cron"})") + "\n";
  std::vector<CronTask> v;
  EXPECT_NO_THROW(v = fold_cron_store(s));
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0].id, "a1");
}

TEST(CronStore, ParseAtIsoOutOfRangeRejected) {
  EXPECT_FALSE(parse_at("2030-13-40T25:99", 0).has_value());
  EXPECT_TRUE(parse_at("2030-06-15T09:30", 0).has_value());
}

TEST(CronStore, WhenKindRoundTrips) {
  CronTask t{"w1", "watch", "when", "", 0, "check it", true, 42};
  t.when = "PEER.pi0.card changes";
  t.cooldown_s = 120;
  auto v = fold_cron_store(add_record(t) + "\n");
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0].kind, "when");
  EXPECT_EQ(v[0].when, "PEER.pi0.card changes");
  EXPECT_EQ(v[0].cooldown_s, 120);
  // non-when kinds serialize when as null and default cooldown
  auto j = nlohmann::json::parse(add_record({"c1", "n", "cron", "* * * * *", 0, "p", false, 1}));
  EXPECT_TRUE(j["when"].is_null());
}

TEST(CronStore, MissingWhenFieldsDefaultTolerantly) {
  // A pre-when record (older store) folds with when="" and cooldown_s=60.
  const char* old_rec =
      R"({"op":"add","id":"a1","name":"n","kind":"cron","schedule":"* * * * *","fire_epoch":null,"prompt":"p","notify":false,"created":1})";
  auto v = fold_cron_store(std::string(old_rec) + "\n");
  ASSERT_EQ(v.size(), 1u);
  EXPECT_TRUE(v[0].when.empty());
  EXPECT_EQ(v[0].cooldown_s, 60);
}
