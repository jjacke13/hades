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
