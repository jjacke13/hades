#include "hades/session_history.h"
#include <fstream>
#include <string>
#include <utility>
namespace hades {
std::vector<nlohmann::json> read_session_jsonl(const std::string& path) {
  std::vector<nlohmann::json> out;
  if (path.empty()) return out;
  std::ifstream f(path);
  if (!f) return out;  // missing file: fresh/absent session, not an error
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    auto j = nlohmann::json::parse(line, nullptr, false);  // false = no throw, returns discarded
    if (!j.is_discarded() && j.is_object()) out.push_back(std::move(j));
  }
  return out;
}
}  // namespace hades
