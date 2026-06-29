// src/memory/store.cpp — read the JSONL memory store, tolerant of junk lines
#include "hades/memory/store.h"
#include <fstream>
#include <nlohmann/json.hpp>
namespace hades {

std::vector<MemoryRecord> load_memories(const std::string& path) {
  std::vector<MemoryRecord> out;
  std::ifstream f(path);
  if (!f) return out;  // missing file: fresh agent, not an error
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    auto j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("text") || !j["text"].is_string())
      continue;  // skip malformed / text-less records
    out.push_back({j["text"].get<std::string>(), j.value("ts", 0.0)});
  }
  return out;
}

}  // namespace hades
