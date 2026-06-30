// include/hades/embedding/vec_math.h — cosine primitives. Normalize once at store+query time so
// similarity is a plain dot product.
#pragma once
#include <vector>
namespace hades {
// L2-normalize in place. Returns false (and leaves v unchanged) if the norm is ~0 (degenerate).
bool l2_normalize(std::vector<float>& v);
// Dot product of two equal-length vectors (== cosine when both are normalized). Size mismatch -> 0.
float dot(const std::vector<float>& a, const std::vector<float>& b);
}  // namespace hades
