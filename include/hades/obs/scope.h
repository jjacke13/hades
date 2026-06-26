#pragma once
#include <string>
#include <vector>

namespace hades {

/// Returns all lines if prefix is empty; otherwise only lines whose key field
/// (2nd tab-separated column) starts with prefix.
[[nodiscard]] std::vector<std::string> scope_filter(const std::vector<std::string>& lines,
                                      const std::string& prefix);

}  // namespace hades
