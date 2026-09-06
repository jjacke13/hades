// tests/test_session_id.cpp — unit tests for session-id generation + path resolution
//
// make_session_id() returns a "YYYYMMDD-HHMMSS" launch stamp; resolve_session_path()
// returns a SessionResolution{path, fresh_fallback}: a collision-safe NEW file when not
// resuming (a `-N` suffix when same-second files already exist), a NAMED file on
// `--resume <id>` (MalConfig if absent), the most recently modified file on `--resume`, or a
// fresh path with fresh_fallback=true when `--resume` finds an empty/missing directory.

#include <fcntl.h>     // open (the "other live hades" stand-in in the lock tests)
#include <sys/file.h>  // flock
#include <unistd.h>    // close
#include <cctype>
#include <chrono>
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
  EXPECT_EQ(sr.path, dir + "/20260630-100000.jsonl");  // written second -> newest mtime
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

// ── bare `--resume` picks the newest by MTIME, not by filename ──────────────────────────
// The two id shapes do not sort together: at index 4 a date id has '-' (0x2D) and a launch stamp
// a digit (0x30+), so every "YYYY-MM-DD.jsonl" sorts BELOW every "YYYYMMDD-HHMMSS.jsonl" — a
// lexical pick would resume a pre-upgrade July session forever, no matter how new the date file.
TEST(SessionId, ResumeNoIdPicksNewestByMtimeNotFilename) {
  namespace fs = std::filesystem;
  const std::string dir = fresh_dir("mtimeorder");
  const std::string old_stamp = dir + "/20260720-191734.jsonl";  // lexically the MAX
  const std::string today = dir + "/2026-09-06.jsonl";           // but written far more recently
  touch(old_stamp);
  touch(today);
  const auto now = fs::file_time_type::clock::now();
  fs::last_write_time(old_stamp, now - std::chrono::hours(48));
  fs::last_write_time(today, now - std::chrono::hours(1));

  const auto sr = resolve_session_path(dir, /*resume=*/true, "", "");
  EXPECT_EQ(sr.path, today);  // newest by mtime wins over the lexically larger old-format name
  EXPECT_FALSE(sr.fresh_fallback);
}

// Equal mtimes (coarse filesystem stamps) must still resolve deterministically: filename breaks
// the tie, so the pick never depends on directory-iteration order.
TEST(SessionId, ResumeNoIdBreaksMtimeTiesOnFilename) {
  namespace fs = std::filesystem;
  const std::string dir = fresh_dir("mtimetie");
  touch(dir + "/2026-09-05.jsonl");
  touch(dir + "/2026-09-06.jsonl");
  const auto same = fs::file_time_type::clock::now() - std::chrono::hours(2);
  fs::last_write_time(dir + "/2026-09-05.jsonl", same);
  fs::last_write_time(dir + "/2026-09-06.jsonl", same);

  const auto sr = resolve_session_path(dir, /*resume=*/true, "", "");
  EXPECT_EQ(sr.path, dir + "/2026-09-06.jsonl");
}

// ── lock_session_file: a concurrently LIVE process never shares the file ────────────────
// flock locks belong to the open file description, so a SECOND open() of the same file is denied
// even from within this process — which is exactly what a second hades looks like to the kernel,
// and lets the divert path be tested without spawning one.
TEST(SessionId, LockedSessionFileDivertsToAFreeSibling) {
  const std::string dir = fresh_dir("locked");
  const std::string today = dir + "/2026-09-06.jsonl";
  touch(today);
  const int held = ::open(today.c_str(), O_RDWR);  // stand-in for the other live hades
  ASSERT_GE(held, 0);
  ASSERT_EQ(::flock(held, LOCK_EX | LOCK_NB), 0);

  EXPECT_EQ(lock_session_file(today), dir + "/2026-09-06-1.jsonl");  // diverted, not shared
  ::close(held);
}

// The whole point of the day-session feature: a DEAD process released its lock, so an unheld file
// is returned untouched and the restart rejoins this morning's conversation.
TEST(SessionId, UnheldSessionFileIsClaimedAsIs) {
  const std::string dir = fresh_dir("unheld");
  const std::string today = dir + "/2026-09-06.jsonl";
  touch(today);
  EXPECT_EQ(lock_session_file(today), today);
}

// A missing sessions_dir (first boot ever) is created, not misreported as a lock conflict.
TEST(SessionId, MissingSessionsDirIsCreatedAndClaimed) {
  const std::string dir = fresh_dir("mkdir") + "/nested";
  const std::string today = dir + "/2026-09-06.jsonl";
  EXPECT_EQ(lock_session_file(today), today);
  EXPECT_TRUE(std::filesystem::exists(today));
}

// …but a NAMED `--resume <id>` asked for THAT session specifically: handing back a different file
// is the surprise, and it would be inconsistent with the `--resume <absent-id>` MalConfig. It must
// fail loudly instead of diverting. (Bare `--resume` keeps diverting — it means "newest, whatever
// that is", which LockedSessionFileDivertsToAFreeSibling covers.)
TEST(SessionId, NamedResumeOfAHeldSessionThrowsInsteadOfDiverting) {
  const std::string dir = fresh_dir("heldnamed");
  const std::string named = dir + "/2026-09-04.jsonl";
  touch(named);
  const int held = ::open(named.c_str(), O_RDWR);  // stand-in for the other live hades
  ASSERT_GE(held, 0);
  ASSERT_EQ(::flock(held, LOCK_EX | LOCK_NB), 0);

  EXPECT_THROW(lock_session_file(named, OnHeld::Fail), MalConfig);
  EXPECT_FALSE(std::filesystem::exists(dir + "/2026-09-04-1.jsonl"));  // no silent substitute
  ::close(held);
}

// ── SessionLock: a released lock stops holding a CLOSED session hostage ──────────────────────────
//
// lock_session_file's fd is normally leaked on purpose (= held for the process lifetime). With a
// handle it is owned instead, and dropping the handle releases the flock — which is how the daily
// rotation stops failing another process's `--resume <yesterday>` for a session it has left.
TEST(SessionId, SessionLockReleasesOnReset) {
  const std::string dir = fresh_dir("lockhandle");
  const std::string p = dir + "/2026-09-06.jsonl";
  SessionLock lk;
  EXPECT_EQ(lock_session_file(p, OnHeld::Divert, &lk), p);
  EXPECT_TRUE(lk.held());
  EXPECT_THROW(lock_session_file(p, OnHeld::Fail), MalConfig);   // held: a second claim fails
  lk.reset();
  EXPECT_FALSE(lk.held());
  EXPECT_EQ(lock_session_file(p, OnHeld::Fail), p);              // released: claimable again
}

// Move-assignment is how a rotation swaps locks: the new day's handle replaces the old one and
// the previous file is released by the same statement.
TEST(SessionId, MovingIntoAHeldSessionLockReleasesThePrevious) {
  const std::string dir = fresh_dir("lockmove");
  const std::string yesterday = dir + "/2026-09-06.jsonl";
  const std::string today = dir + "/2026-09-07.jsonl";
  SessionLock lk;
  ASSERT_EQ(lock_session_file(yesterday, OnHeld::Divert, &lk), yesterday);
  ASSERT_EQ(lock_session_file(today, OnHeld::Divert, &lk), today);   // assigns through the same handle
  EXPECT_EQ(lock_session_file(yesterday, OnHeld::Fail), yesterday);  // yesterday freed
  EXPECT_THROW(lock_session_file(today, OnHeld::Fail), MalConfig);   // today still held
}

// ── Session.day_cutoff_hour ─────────────────────────────────────────────────────────────────────
TEST(DayCutoffHour, IsParsed) {
  EXPECT_EQ(resolve_day_cutoff_hour("6"), 6);
  EXPECT_EQ(resolve_day_cutoff_hour("0"), 0);     // an explicit 0 IS a setting: calendar midnight
  EXPECT_EQ(resolve_day_cutoff_hour("23"), 23);
}
TEST(DayCutoffHour, GarbageFallsBackToFour) {
  // Never 0: garbage must not silently move every session boundary to midnight.
  for (const char* v : {"", "abc", "4h", "4 5", "4.5", " ", "--4"})
    EXPECT_EQ(resolve_day_cutoff_hour(v), kDefaultDayCutoffHour) << v;
}
TEST(DayCutoffHour, OutOfRangeFallsBackToFour) {
  EXPECT_EQ(resolve_day_cutoff_hour("-1"), kDefaultDayCutoffHour);
  EXPECT_EQ(resolve_day_cutoff_hour("24"), kDefaultDayCutoffHour);
  EXPECT_EQ(resolve_day_cutoff_hour("999999999999999999999"), kDefaultDayCutoffHour);  // out of int
}
