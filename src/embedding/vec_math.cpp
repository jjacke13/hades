#include "hades/embedding/vec_math.h"
#include <cmath>
namespace hades {
bool l2_normalize(std::vector<float>& v) {
  double s = 0.0;
  for (float x : v) s += static_cast<double>(x) * x;
  if (s <= 1e-12) return false;            // zero / degenerate
  const float inv = static_cast<float>(1.0 / std::sqrt(s));
  for (float& x : v) x *= inv;
  return true;
}
float dot(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.size() != b.size()) return 0.0f;
  float s = 0.0f;
  for (std::size_t i = 0; i < a.size(); ++i) s += a[i] * b[i];
  return s;
}
}  // namespace hades
