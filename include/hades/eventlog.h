#pragma once
#include <string>
#include <vector>
#include "hades/entry.h"
namespace hades {
// Append-only transcript: one TSV line per Entry (ts \t key \t source \t value-compact-json)
// to `path` (+ in-memory copy). Registered secret substrings masked to "***REDACTED***".
class Eventlog {
public:
  explicit Eventlog(std::string path);    // "" => in-memory only
  void append(const Entry& e);
  void add_redaction(std::string secret); // ignored if empty
  const std::vector<Entry>& entries() const;
private:
  std::string path_;
  std::vector<std::string> secrets_;
  std::vector<Entry> entries_;
};
}  // namespace hades
