// src/core/compact.cpp — session-compaction pure helpers (see the header)
#include "hades/compact/compact.h"
#include <filesystem>
#include <sstream>
#include "hades/module/tool_runner.h"   // trunc_utf8_bytes (public inline, tool-offload)
namespace hades {
namespace {
// One message's text for the digest: string content as-is; an assistant tool-call pair-head
// is NAMED rather than dumped (the call structure is noise to a summarizer); anything else
// replace-dumped (never throws on invalid UTF-8).
std::string msg_text(const nlohmann::json& m) {
  if (m.contains("content") && m["content"].is_string()) return m["content"].get<std::string>();
  if (m.contains("tool_calls") && m["tool_calls"].is_array() && !m["tool_calls"].empty() &&
      m["tool_calls"][0].is_object()) {
    const auto& f = m["tool_calls"][0];
    return "[called tool: " +
           (f.contains("function") && f["function"].is_object()
                ? f["function"].value("name", std::string{"?"})
                : std::string{"?"}) + "]";
  }
  if (m.contains("content"))
    return m["content"].dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  return "";
}
}  // namespace

nlohmann::json digest_span(const std::vector<nlohmann::json>& span,
                           std::size_t per_msg_cap, std::size_t total_cap) {
  // Walk newest -> oldest, keeping messages while within total_cap; then emit oldest-first
  // with one omission marker for whatever fell off the front.
  std::vector<nlohmann::json> kept;
  std::size_t total = 0;
  for (std::size_t i = span.size(); i-- > 0;) {
    std::string c = trunc_utf8_bytes(msg_text(span[i]), per_msg_cap);
    const std::string role = span[i].is_object() ? span[i].value("role", "") : "";
    const std::size_t sz = c.size() + role.size();
    if (!kept.empty() && total + sz > total_cap) break;
    total += sz;
    kept.push_back({{"role", role}, {"content", std::move(c)}});
  }
  nlohmann::json out = nlohmann::json::array();
  const std::size_t omitted = span.size() - kept.size();
  if (omitted > 0)
    out.push_back({{"role", "system"},
                   {"content", "[... " + std::to_string(omitted) + " earlier messages omitted ...]"}});
  for (std::size_t i = kept.size(); i-- > 0;) out.push_back(std::move(kept[i]));
  return out;
}

SidecarSummary parse_summary_sidecar(const std::string& file_text) {
  SidecarSummary s;
  std::istringstream in(file_text);
  std::string first;
  if (!std::getline(in, first)) return s;
  if (first.rfind("upto: ", 0) != 0) return s;
  try {
    const long long n = std::stoll(first.substr(6));
    if (n <= 0) return s;
    s.upto = static_cast<std::size_t>(n);
  } catch (...) {
    return s;
  }
  std::string blank;
  std::getline(in, blank);   // the separator line (tolerate its absence)
  std::ostringstream rest;
  rest << in.rdbuf();
  s.text = rest.str();
  while (!s.text.empty() && s.text.back() == '\n') s.text.pop_back();
  if (s.text.empty()) s.upto = 0;   // a summary with no text is no summary
  return s;
}

std::string serialize_summary_sidecar(std::size_t upto, const std::string& text) {
  return "upto: " + std::to_string(upto) + "\n\n" + text + "\n";
}

std::string sidecar_path_for(const std::string& session_path) {
  if (session_path.empty()) return "";
  const std::filesystem::path p(session_path);
  return (p.parent_path() / (p.stem().string() + ".summary.md")).string();
}

std::string session_stem(const std::string& session_path) {
  if (session_path.empty()) return "";
  return std::filesystem::path(session_path).stem().string();
}
}  // namespace hades
