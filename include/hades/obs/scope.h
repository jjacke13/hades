// include/hades/obs/scope.h — Eventlog key-prefix filter for hades-scope
//
// scope_filter() retains Eventlog lines whose key field (second tab-separated
// column) starts with a given prefix — the core of the hades-scope CLI
// (the uXMS analog) used to replay and inspect Eventlog transcripts.

#pragma once
#include <string>
#include <vector>

namespace hades {

/// Returns all lines if prefix is empty; otherwise only lines whose key field
/// (2nd tab-separated column) starts with prefix.
[[nodiscard]] std::vector<std::string> scope_filter(const std::vector<std::string>& lines,
                                      const std::string& prefix);

}  // namespace hades
