// tests/test_scope.cpp — unit tests for scope_filter TSV line filtering
//
// Verifies that scope_filter retains only TSV lines whose key column matches
// a given prefix (e.g. "TOOL_" keeps TOOL_REQUEST and TOOL_RESULT) and that
// an empty prefix passes all lines — the pure filter function backing the
// hades-scope CLI replay of the Eventlog.

#include <gtest/gtest.h>
#include "hades/obs/scope.h"   // std::vector<std::string> scope_filter(const std::vector<std::string>&, const std::string&)
using namespace hades;
TEST(Scope, FiltersByKeyPrefix) {
  std::vector<std::string> lines={
    "0.1\tUSER_MESSAGE\tchat\t\"hi\"",
    "0.2\tTOOL_REQUEST\tarbiter\t{}",
    "0.3\tTOOL_RESULT\ttool_runner\t{}"};
  auto out=scope_filter(lines,"TOOL_");
  ASSERT_EQ(out.size(),2u);
  EXPECT_NE(out[0].find("TOOL_REQUEST"), std::string::npos);
  EXPECT_NE(out[1].find("TOOL_RESULT"), std::string::npos);
  EXPECT_EQ(scope_filter(lines,"").size(), 3u);
}
