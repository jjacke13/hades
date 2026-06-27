// tests/test_subprocess.cpp — unit tests for run_subprocess isolation primitive
//
// Covers normal stdin->stdout pass-through via cat, non-zero exit codes,
// wall-clock timeout with SIGKILL (timed_out flag), and a 1 MB large-stdin
// round-trip to confirm the interleaved I/O loop avoids deadlock — the
// foundation ToolRunner uses to sandbox every native tool call.

#include <gtest/gtest.h>
#include "hades/tool/subprocess.h"
using namespace hades;
TEST(Subprocess, EchoesStdinThroughCat) {
  auto r=run_subprocess({"cat"}, "hello", 5.0);
  EXPECT_FALSE(r.timed_out); EXPECT_EQ(r.code,0); EXPECT_EQ(r.out,"hello");
}
TEST(Subprocess, NonZeroExit) {
  auto r=run_subprocess({"sh","-c","exit 3"}, "", 5.0);
  EXPECT_EQ(r.code,3);
}
TEST(Subprocess, TimesOut) {
  auto r=run_subprocess({"sleep","5"}, "", 0.2);
  EXPECT_TRUE(r.timed_out);
}
TEST(Subprocess, LargeStdinThroughCatNoDeadlock) {
  std::string big(1u<<20, 'x');   // 1 MB through cat: consumes large stdin AND emits large stdout
  auto r=run_subprocess({"cat"}, big, 10.0);
  EXPECT_FALSE(r.timed_out);
  EXPECT_EQ(r.code, 0);
  ASSERT_EQ(r.out.size(), big.size());
  EXPECT_EQ(r.out, big);
}
