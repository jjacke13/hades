// src/apps/scope/scope.cpp — Eventlog line filter by key prefix
//
// Implements scope_filter(): given raw tab-separated Eventlog lines and a key prefix,
// returns only the lines whose key field starts with that prefix. Called by
// scope_main.cpp to back the hades-scope CLI's filter/replay behaviour (uXMS analog).

#include "hades/obs/scope.h"
#include <sstream>

namespace hades {

std::vector<std::string> scope_filter(const std::vector<std::string>& lines,
                                      const std::string& prefix) {
  if (prefix.empty()) return lines;
  std::vector<std::string> out;
  for (const auto& l : lines) {
    std::istringstream s(l);
    std::string ts, key;
    std::getline(s, ts, '\t');
    std::getline(s, key, '\t');
    if (key.rfind(prefix, 0) == 0) out.push_back(l);
  }
  return out;
}

}  // namespace hades
