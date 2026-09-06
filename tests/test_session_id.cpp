// tests/test_session_id.cpp — unit tests for session-id generation + path resolution
//
// make_session_id() returns a "YYYYMMDD-HHMMSS" launch stamp; resolve_session_path()
// returns a SessionResolution{path, fresh_fallback}: a collision-safe NEW file when not
// resuming (a `-N` suffix when same-second files already exist), a NAMED file on
// `--resume <id>` (MalConfig if absent), the lexical-newest file on `--resume`, or a
// fresh path with fresh_fallback=true when `--resume` finds an empty/missing directory.

#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <gtest/gtest.h>
#include "hades/launcher.h"      // MalConfig
#include "hades/session_id.h"
using namespace hades;

namespace {
// A fresh, empty directory unique to each test (avoids ::testing::TempDir() cross-talk).
std::string fresh_dir(const std::string& tag) {
  std::filesystem::path d =
      std::filesystem::temp_directory_path() / ("hades_sid_" + tag);
  std::filesystem::remove_all(d);
  std::filesystem::create_directories(d);
  return d.string();
}
void touch(const std::string& path) { std::ofstream f(path); f << "\n"; }
}  // namespace

TEST(SessionId, MakeSessionIdFormat) {
  const std::string id = make_session_id();
  ASSERT_EQ(id.size(), 15u);          // YYYYMMDD-HHMMSS
  EXPECT_EQ(id[8], '-');
  for (std::size_t i = 0; i < id.size(); ++i) {
    if (i == 8) continue;
    EXPECT_TRUE(std::isdigit(static_cast<unsigned char>(id[i]))) << "non-digit at " << i;
  }
}

TEST(SessionId, ResolveNewSession) {
  const std::string dir = fresh_dir("new");
  const auto sr = resolve_session_path(dir, /*resume=*/false, "", "20260630-100000");
  EXPECT_EQ(sr.path, dir + "/20260630-100000.jsonl");  // new file path; existence NOT required
  EXPECT_FALSE(sr.fresh_fallback);                     // a deliberate new session, not a fallback
}

// A NEW session whose timestamp id already has a file on disk (a second hades launched in the
// same wall-clock second) must NOT reuse that path — it gets the first free `-N` suffix, so the
// two conversations never interleave into one file.
TEST(SessionId, SameSecondNewSessionGetsUniqueSuffix) {
  const std::string dir = fresh_dir("samesecond");
  touch(dir + "/20260630-100000.jsonl");  // a session already launched this second
  const auto sr1 = resolve_session_path(dir, /*resume=*/false, "", "20260630-100000");
  EXPECT_EQ(sr1.path, dir + "/20260630-100000-1.jsonl");  // collision avoided
  EXPECT_FALSE(sr1.fresh_fallback);

  touch(sr1.path);  // now a THIRD launch in the same second
  const auto sr2 = resolve_session_path(dir, /*resume=*/false, "", "20260630-100000");
  EXPECT_EQ(sr2.path, dir + "/20260630-100000-2.jsonl");  // next free suffix
  EXPECT_FALSE(sr2.fresh_fallback);
}

TEST(SessionId, ResolveSpecificId) {
  const std::string dir = fresh_dir("specific");
  touch(dir + "/abc.jsonl");
  const auto sr = resolve_session_path(dir, /*resume=*/true, "abc", "");
  EXPECT_EQ(sr.path, dir + "/abc.jsonl");
  EXPECT_FALSE(sr.fresh_fallback);
  // A named session that does not exist is a clear configuration error.
  EXPECT_THROW(resolve_session_path(dir, /*resume=*/true, "nope", ""), MalConfig);
}

TEST(SessionId, ResolveNewestSession) {
  const std::string dir = fresh_dir("newest");
  touch(dir + "/20260630-090000.jsonl");
  touch(dir + "/20260630-100000.jsonl");
  const auto sr = resolve_session_path(dir, /*resume=*/true, "", "");
  EXPECT_EQ(sr.path, dir + "/20260630-100000.jsonl");  // lexical-max == newest timestamp
  EXPECT_FALSE(sr.fresh_fallback);
}

// `--resume` against an empty/missing directory has nothing to resume: it returns the fresh
// new_id path AND signals the fallback explicitly via the flag (no string-compare coupling).
TEST(SessionId, EmptyDirResumeSetsFreshFallbackFlag) {
  const std::string dir = fresh_dir("emptyresume");
  const auto sr = resolve_session_path(dir, /*resume=*/true, "", "20260630-100000");
  EXPECT_EQ(sr.path, dir + "/20260630-100000.jsonl");  // empty dir -> plain new_id path
  EXPECT_TRUE(sr.fresh_fallback);                      // "you asked to resume but there's nothing"
}

// ── logical session date (one session per day) ──────────────────────────────────────────
// A session "day" runs from `cutoff_hour` to the same hour next day, LOCAL time, so anything
// before the cutoff still belongs to the previous day. logical_date() is pure — it takes the
// instant explicitly, so these run without touching the clock.

// Local-time helper for the tests: build a time_t from local Y-M-D H:M.
static std::time_t local_tm(int y, int mo, int d, int h, int mi) {
  std::tm tm{};
  tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
  tm.tm_hour = h; tm.tm_min = mi; tm.tm_isdst = -1;
  return std::mktime(&tm);
}

TEST(LogicalDate, BeforeCutoffBelongsToThePreviousDay) {
  EXPECT_EQ(logical_date(local_tm(2026, 9, 6, 3, 59), 4), "2026-09-05");
}
TEST(LogicalDate, AtCutoffStartsTheNewDay) {
  EXPECT_EQ(logical_date(local_tm(2026, 9, 6, 4, 0), 4), "2026-09-06");
}
TEST(LogicalDate, EveningIsTheSameDay) {
  EXPECT_EQ(logical_date(local_tm(2026, 9, 6, 23, 30), 4), "2026-09-06");
}
TEST(LogicalDate, AfterMidnightStillYesterday) {
  EXPECT_EQ(logical_date(local_tm(2026, 9, 7, 2, 0), 4), "2026-09-06");
}
TEST(LogicalDate, CutoffZeroIsPlainCalendarDate) {
  EXPECT_EQ(logical_date(local_tm(2026, 9, 6, 0, 1), 0), "2026-09-06");
  EXPECT_EQ(logical_date(local_tm(2026, 9, 6, 23, 59), 0), "2026-09-06");
}
TEST(LogicalDate, CrossesMonthAndYearBoundaries) {
  EXPECT_EQ(logical_date(local_tm(2026, 1, 1, 2, 0), 4), "2025-12-31");
  EXPECT_EQ(logical_date(local_tm(2026, 3, 1, 1, 0), 4), "2026-02-28");
}
TEST(LogicalDate, CutoffTwentyThreeIsAcceptedAndShiftsAlmostAFullDay) {
  EXPECT_EQ(logical_date(local_tm(2026, 9, 6, 22, 0), 23), "2026-09-05");
  EXPECT_EQ(logical_date(local_tm(2026, 9, 6, 23, 0), 23), "2026-09-06");
}
// A garbage cutoff must never shift the date wildly: clamped to [0,23], so it degrades to
// "plain calendar date" / "almost a full day", never to a multi-day jump.
TEST(LogicalDate, OutOfRangeCutoffIsClamped) {
  EXPECT_EQ(logical_date(local_tm(2026, 9, 6, 12, 0), -5), "2026-09-06");   // clamps to 0
  EXPECT_EQ(logical_date(local_tm(2026, 9, 6, 12, 0), 999), "2026-09-05");  // clamps to 23
}
TEST(LogicalDate, CurrentSessionIdIsADate) {
  const std::string id = current_session_id(kDefaultDayCutoffHour);
  ASSERT_EQ(id.size(), 10u);  // YYYY-MM-DD
  EXPECT_EQ(id[4], '-');
  EXPECT_EQ(id[7], '-');
}

// ── boot path: today's file is REJOINED, not suffixed ───────────────────────────────────
// The daily boot path passes OnCollision::Reuse: colliding with today's file is precisely how a
// restart rejoins the day's conversation. (OnCollision::Suffix — the default — keeps the old
// never-share-a-file contract for `/new`; SameSecondNewSessionGetsUniqueSuffix locks that.)
TEST(SessionId, DailyBootReusesTodaysExistingFile) {
  const std::string dir = fresh_dir("dailyreuse");
  touch(dir + "/2026-09-06.jsonl");  // this morning's session
  const auto sr = resolve_session_path(dir, /*resume=*/false, "", "2026-09-06", OnCollision::Reuse);
  EXPECT_EQ(sr.path, dir + "/2026-09-06.jsonl");  // rejoined, NOT 2026-09-06-1.jsonl
  EXPECT_FALSE(sr.fresh_fallback);
}
TEST(SessionId, DailyBootCreatesTodaysFileWhenAbsent) {
  const std::string dir = fresh_dir("dailycreate");
  const auto sr = resolve_session_path(dir, /*resume=*/false, "", "2026-09-06", OnCollision::Reuse);
  EXPECT_EQ(sr.path, dir + "/2026-09-06.jsonl");
  EXPECT_FALSE(sr.fresh_fallback);
}
