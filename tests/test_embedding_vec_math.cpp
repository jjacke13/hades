#include <gtest/gtest.h>
#include <cmath>
#include <vector>
#include "hades/embedding/vec_math.h"
using namespace hades;

TEST(VecMath, NormalizeMakesUnitLength) {
  std::vector<float> v{3.0f, 4.0f};            // norm 5
  ASSERT_TRUE(l2_normalize(v));
  EXPECT_NEAR(v[0], 0.6f, 1e-5);
  EXPECT_NEAR(v[1], 0.8f, 1e-5);
  EXPECT_NEAR(std::sqrt(v[0]*v[0] + v[1]*v[1]), 1.0f, 1e-5);
}
TEST(VecMath, NormalizeZeroVectorReturnsFalse) {
  std::vector<float> z{0.0f, 0.0f, 0.0f};
  EXPECT_FALSE(l2_normalize(z));               // degenerate -> caller drops
}
TEST(VecMath, DotOfNormalizedIsCosine) {
  std::vector<float> a{1.0f, 0.0f}, b{1.0f, 0.0f}, c{0.0f, 1.0f};
  l2_normalize(a); l2_normalize(b); l2_normalize(c);
  EXPECT_NEAR(dot(a, b), 1.0f, 1e-5);          // identical -> 1
  EXPECT_NEAR(dot(a, c), 0.0f, 1e-5);          // orthogonal -> 0
}
TEST(VecMath, DotSizeMismatchIsZero) {
  std::vector<float> a{1.0f, 0.0f}, b{1.0f};
  EXPECT_FLOAT_EQ(dot(a, b), 0.0f);            // defensive: incomparable -> 0
}
