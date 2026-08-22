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
}  // namespace hades
