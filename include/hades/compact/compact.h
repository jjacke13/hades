// include/hades/compact/compact.h — session-compaction pure helpers
//
// Shared between the Arbiter (detect/apply/persist) and the CompactorModule (summarize):
// digest_span turns the about-to-drop history span into a bounded json array for the aux
// request (per-message + total byte caps, oldest dropped first — archival recall still
// covers them); the sidecar codec reads/writes `.summary.md` ("upto: N" line, blank line,
// markdown); the path helpers derive the sidecar path and the session id (filename stem)
// used by the COMPACT_REQUEST/SESSION_SUMMARY session guard.
#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
namespace hades {

struct SidecarSummary {
  std::size_t upto = 0;   // history_ messages the summary covers
  std::string text;       // markdown summary ("" = no summary)
};

// Digest a history span for the aux summarize request. Each message becomes {role, content}
// with content byte-capped at per_msg_cap on a UTF-8 boundary; the whole array is capped at
// total_cap bytes by dropping the OLDEST messages first, replaced by one omission marker
// entry. Non-string content is named ("[called tool: X]") or replace-dumped — never throws.
nlohmann::json digest_span(const std::vector<nlohmann::json>& span,
                           std::size_t per_msg_cap = 1000, std::size_t total_cap = 24000);

// Tolerant sidecar parse: expects "upto: N\n\n<text>"; anything malformed -> {0, ""}.
SidecarSummary parse_summary_sidecar(const std::string& file_text);
std::string serialize_summary_sidecar(std::size_t upto, const std::string& text);

// ".hades/sessions/<id>.jsonl" -> ".hades/sessions/<id>.summary.md"; "" -> "".
std::string sidecar_path_for(const std::string& session_path);
// Session id for the compaction bus guards: the session file's stem; "" -> "".
std::string session_stem(const std::string& session_path);

}  // namespace hades
