// src/core/eventlog.cpp — Eventlog append and secret-redaction implementation
//
// Implements Eventlog::append(): redacts all registered secrets from key, source, aux,
// and value before storing to the in-memory entries_ vector and writing a tab-separated
// line to the .alog file on disk. Called by Blackboard::post() on every published Entry;
// the resulting file backs replay and the hades-scope CLI.

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
  const std::string raw = e.value.dump();
  const std::string red = redact(raw, secrets_);
  Entry stored = e;
  stored.key    = redact(e.key, secrets_);
  stored.source = redact(e.source, secrets_);
  stored.aux    = redact(e.aux, secrets_);
  if (red != raw) stored.value = red;
  const std::string line = std::to_string(e.ts) + "\t" + stored.key + "\t" +
                           stored.source + "\t" + red + "\n";
  entries_.push_back(std::move(stored));
  if(!path_.empty()){
    std::ofstream f(path_, std::ios::app);
    f << line;
    if(!f) std::cerr << "[eventlog] write failed: " << path_ << "\n";
  }
}
}  // namespace hades
