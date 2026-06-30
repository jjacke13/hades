// include/hades/session_history.h — tolerant reader for a per-session conversation jsonl.
//
// Pure: reads one JSON message per line from `path`, skipping blank lines and any line that does
// not parse to a JSON object (a corrupt interior line, or a partially-written trailing line from a
// concurrent append). Returns the raw stored messages AS-IS — NO orphan-pair sanitize (this feeds
// the browser GET /history for display, not a provider request). Empty path / missing file -> [].
#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
namespace hades {
std::vector<nlohmann::json> read_session_jsonl(const std::string& path);
}  // namespace hades
