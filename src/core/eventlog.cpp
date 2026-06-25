#include "hades/eventlog.h"
#include <fstream>
namespace hades {
Eventlog::Eventlog(std::string path) : path_(std::move(path)) {}
void Eventlog::add_redaction(std::string s){ if(!s.empty()) secrets_.push_back(std::move(s)); }
const std::vector<Entry>& Eventlog::entries() const { return entries_; }
static std::string redact(std::string s, const std::vector<std::string>& secrets){
  for (const auto& sec : secrets) {
    for (auto pos = s.find(sec); pos != std::string::npos; pos = s.find(sec, pos))
      s.replace(pos, sec.size(), "***REDACTED***");
  }
  return s;
}
void Eventlog::append(const Entry& e){
  entries_.push_back(e);
  std::string val = redact(e.value.dump(), secrets_);
  std::string line = std::to_string(e.ts) + "\t" + e.key + "\t" + e.source + "\t" + val + "\n";
  if(!path_.empty()){ std::ofstream f(path_, std::ios::app); f << line; }
}
}  // namespace hades
