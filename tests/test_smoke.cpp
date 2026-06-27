// tests/test_smoke.cpp — build-linkage sanity: version() returns expected string
//
// Single canary test that the hades library links and hades::version() is
// reachable. Fails immediately if the CMake target wiring or version header
// is broken before any Blackboard/Module tests run.

#include <gtest/gtest.h>
#include "hades/version.h"
TEST(Smoke, Version) { EXPECT_EQ(hades::version(), "0.1.0"); }
