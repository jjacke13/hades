// tests/test_file_version.cpp — the shared content-hash behind the staleness guard
#include <gtest/gtest.h>
#include <string>
#include "hades/tool/file_version.h"
using namespace hades;

TEST(FileVersion, KnownEmptyHash) {
  // FNV-1a 64 offset basis — pins the algorithm (a change breaks every stored expectation).
  EXPECT_EQ(file_version(""), "cbf29ce484222325");
}

TEST(FileVersion, DeterministicAndFormat) {
  const std::string v = file_version("hello world\n");
  EXPECT_EQ(v.size(), 16u);
  EXPECT_EQ(v, file_version("hello world\n"));                 // stable
  for (char c : v) EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << c;
}

TEST(FileVersion, OneByteChangeChangesHash) {
  EXPECT_NE(file_version("aaaa"), file_version("aaab"));
  EXPECT_NE(file_version("x"), file_version(""));
}
