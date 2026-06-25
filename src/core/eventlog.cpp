#include "hades/eventlog.h"
#include <fstream>
#include <iostream>
#include <string_view>
namespace hades {
Eventlog::Eventlog(std::string path) : path_(std::move(path)) {}
void Eventlog::add_redaction(std::string s){ if(!s.empty()) secrets_.push_back(std::move(s)); }
const std::vector<Entry>& Eventlog::entries() const { return entries_; }
static std::string redact(std::string s, const std::vector<std::string>& secrets){
  static constexpr std::string_view MASK = "***REDACTED***";
  for (const auto& sec : secrets) {
    if (sec.empty()) continue;
    for (auto pos = s.find(sec); pos != std::string::npos; pos = s.find(sec, pos + MASK.size()))
      s.replace(pos, sec.size(), MASK);
  }
  return s;
}
void Eventlog::append(const Entry& e){
  std::string raw = e.value.dump();
  std::string red = redact(raw, secrets_);
  Entry stored = e;
  if (red != raw) stored.value = red;        // a secret was present -> store masked string
  entries_.push_back(std::move(stored));
  std::string line = std::to_string(e.ts) + "\t" + e.key + "\t" + e.source + "\t" + red + "\n";
  if(!path_.empty()){
    std::ofstream f(path_, std::ios::app);
    f << line;
    if(!f) std::cerr << "[eventlog] write failed: " << path_ << "\n";   // surface write errors
  }
}
}  // namespace hades
