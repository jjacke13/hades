// tests/test_session_id.cpp — unit tests for session-id generation + path resolution
//
// make_session_id() returns a "YYYYMMDD-HHMMSS" launch stamp; resolve_session_path()
// picks the per-session jsonl: a NEW file when not resuming, a NAMED file on
// `--resume <id>` (MalConfig if absent), or the lexical-newest file on `--resume`
// (fresh fallback when the directory is empty/missing).

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
  EXPECT_EQ(resolve_session_path(dir, /*resume=*/false, "", "20260630-100000"),
            dir + "/20260630-100000.jsonl");   // new file path; existence NOT required
}

TEST(SessionId, ResolveSpecificId) {
  const std::string dir = fresh_dir("specific");
  touch(dir + "/abc.jsonl");
  EXPECT_EQ(resolve_session_path(dir, /*resume=*/true, "abc", ""), dir + "/abc.jsonl");
  // A named session that does not exist is a clear configuration error.
  EXPECT_THROW(resolve_session_path(dir, /*resume=*/true, "nope", ""), MalConfig);
}

TEST(SessionId, ResolveNewestSession) {
  const std::string dir = fresh_dir("newest");
  touch(dir + "/20260630-090000.jsonl");
  touch(dir + "/20260630-100000.jsonl");
  EXPECT_EQ(resolve_session_path(dir, /*resume=*/true, "", ""),
            dir + "/20260630-100000.jsonl");   // lexical-max == newest timestamp
}

TEST(SessionId, ResolveEmptyDirFallsBackToFresh) {
  const std::string dir = fresh_dir("empty");
  EXPECT_EQ(resolve_session_path(dir, /*resume=*/true, "", "20260630-120000"),
            dir + "/20260630-120000.jsonl");   // nothing to resume -> fresh new_id path
}
