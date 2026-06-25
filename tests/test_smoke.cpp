#include <gtest/gtest.h>
#include "hades/version.h"
TEST(Smoke, Version) { EXPECT_EQ(hades::version(), "0.1.0"); }
