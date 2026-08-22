// include/hades/memory/record.h — one persisted memory: text, save timestamp, optional topic
#pragma once
#include <string>
namespace hades {
// `topic` is an OPTIONAL supersession key: among records sharing the same non-empty topic,
// only the newest (greatest ts) is retrieval-eligible, so a corrected fact replaces the one
// it corrects instead of accumulating beside it. Empty topic (the default, and every record
// written before 2026-08-22) means "standalone" — never superseded, never superseding.
// Aggregate on purpose: `MemoryRecord{"text", ts}` must keep compiling.
struct MemoryRecord {
  std::string text;
  double ts = 0.0;
  std::string topic;
};

// Canonical form of a supersession key: trim surrounding whitespace, then lowercase.
// Load-bearing, not cosmetic — the bucket is matched by EXACT string, so without this
// "seat-pref " and "Seat-Pref" would be different buckets and the supersession the model
// asked for would silently not happen; and a whitespace-only topic (weak models fill every
// schema field — see the schedule_task exactly-one-of gotcha) would become a real bucket
// that swallows unrelated facts. Trimming to "" makes those count as ABSENT, per the house
// empty-is-absent rule. Header-inline so the standalone save_memory tool binary shares it
// without linking hades_core (the valid_skill_name / trunc_utf8_bytes pattern).
inline std::string normalize_topic(std::string t) {
  const auto first = t.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";           // whitespace-only -> absent
  t = t.substr(first, t.find_last_not_of(" \t\r\n") - first + 1);
  for (char& c : t)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');   // ASCII slugs only
  return t;
}
}  // namespace hades
