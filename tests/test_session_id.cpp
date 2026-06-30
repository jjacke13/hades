// tests/test_session_id.cpp — unit tests for session-id generation + path resolution
//
// make_session_id() returns a "YYYYMMDD-HHMMSS" launch stamp; resolve_session_path()
// returns a SessionResolution{path, fresh_fallback}: a collision-safe NEW file when not
// resuming (a `-N` suffix when same-second files already exist), a NAMED file on
// `--resume <id>` (MalConfig if absent), the lexical-newest file on `--resume`, or a
// fresh path with fresh_fallback=true when `--resume` finds an empty/missing directory.

#include <cctype>
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
