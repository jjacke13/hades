// tests/test_cron.cpp — pure cron matcher (5-field, *,N,A-B,*/N,A,B; machine-local; AND fields)
#include <gtest/gtest.h>
#include <ctime>
#include "hades/heartbeat/cron.h"
using namespace hades;

// Build a std::tm for a given wall-clock; only the fields cron reads are set.
static std::tm mk(int min, int hour, int mday, int mon /*1-12*/, int wday /*0=Sun*/) {
  std::tm t{};
  t.tm_min = min; t.tm_hour = hour; t.tm_mday = mday; t.tm_mon = mon - 1; t.tm_wday = wday;
  return t;
}

TEST(Cron, Wildcard) {
  EXPECT_TRUE(cron_matches("* * * * *", mk(0, 0, 1, 1, 0)));
  EXPECT_TRUE(cron_matches("* * * * *", mk(37, 13, 25, 12, 3)));
}
TEST(Cron, ExactMinuteHour) {
  EXPECT_TRUE(cron_matches("0 6 * * *", mk(0, 6, 15, 3, 2)));
  EXPECT_FALSE(cron_matches("0 6 * * *", mk(1, 6, 15, 3, 2)));   // minute off
  EXPECT_FALSE(cron_matches("0 6 * * *", mk(0, 7, 15, 3, 2)));   // hour off
}
TEST(Cron, Step) {
  for (int m : {0, 10, 20, 30, 40, 50}) EXPECT_TRUE(cron_matches("*/10 * * * *", mk(m, 4, 1, 1, 1))) << m;
  for (int m : {1, 5, 11, 59})          EXPECT_FALSE(cron_matches("*/10 * * * *", mk(m, 4, 1, 1, 1))) << m;
}
TEST(Cron, RangeAndListAndDow) {
  EXPECT_TRUE(cron_matches("0 6 * * 1-5", mk(0, 6, 1, 1, 3)));    // Wed in 1-5
  EXPECT_FALSE(cron_matches("0 6 * * 1-5", mk(0, 6, 1, 1, 0)));   // Sun not in 1-5
  EXPECT_TRUE(cron_matches("0 9,17 * * *", mk(0, 17, 1, 1, 1)));  // list
  EXPECT_FALSE(cron_matches("0 9,17 * * *", mk(0, 12, 1, 1, 1)));
}
TEST(Cron, MonthAndDom) {
  EXPECT_TRUE(cron_matches("0 0 1 1 *", mk(0, 0, 1, 1, 4)));      // Jan 1 00:00
  EXPECT_FALSE(cron_matches("0 0 1 1 *", mk(0, 0, 2, 1, 5)));     // Jan 2
}
TEST(Cron, MalformedIsFalse) {
  EXPECT_FALSE(cron_matches("", mk(0, 0, 1, 1, 0)));
  EXPECT_FALSE(cron_matches("* * * *", mk(0, 0, 1, 1, 0)));        // 4 fields
  EXPECT_FALSE(cron_matches("* * * * * *", mk(0, 0, 1, 1, 0)));    // 6 fields
  EXPECT_FALSE(cron_matches("bogus * * * *", mk(0, 0, 1, 1, 0)));
  EXPECT_FALSE(cron_matches("*/0 * * * *", mk(0, 0, 1, 1, 0)));    // step 0
  EXPECT_FALSE(cron_matches("99 * * * *", mk(0, 0, 1, 1, 0)));     // out of range value never matches
}
TEST(Cron, Valid) {
  EXPECT_TRUE(cron_valid("*/10 * * * *"));
  EXPECT_TRUE(cron_valid("0 6 * * 1-5"));
  EXPECT_TRUE(cron_valid("0 9,17 * * *"));
  EXPECT_FALSE(cron_valid("* * * *"));       // 4 fields
  EXPECT_FALSE(cron_valid("bogus * * * *"));
  EXPECT_FALSE(cron_valid("*/0 * * * *"));
  EXPECT_FALSE(cron_valid("70 * * * *"));    // minute out of range
}
