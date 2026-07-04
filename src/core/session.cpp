// src/core/session.cpp — session identity + persisted-conversation reads
//
// Merged (2026-07-04 src reorg): session_id (launch-timestamp ids, collision-safe
// unique_fresh_path, --resume resolution) + session_history (tolerant per-session
// jsonl reader shared by the Arbiter's load_history and GET /history).

#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include "hades/session_id.h"
#include "hades/session_history.h"
#include "hades/launcher.h"  // MalConfig

// ── session-id generation + per-session jsonl path resolution (was src/core/session_id.cpp) ──────────────
namespace hades {

// First NON-EXISTING path among dir/<id>.jsonl, dir/<id>-1.jsonl, dir/<id>-2.jsonl, … so two
// hades resolving the same id in the same wall-clock second (1s id resolution) never resolve to one
// file and interleave their conversations. In practice 1-2 iterations; the cap guards a pathological
// burst. Exported (session_id.h) so the Arbiter's `/new` rotation shares this collision-avoidance.
std::string unique_fresh_path(const std::string& dir, const std::string& id) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const std::string base = dir + "/" + id + ".jsonl";
  if (!fs::exists(base, ec)) return base;
  constexpr int kMaxSuffix = 10000;
  for (int n = 1; n < kMaxSuffix; ++n) {
    const std::string cand = dir + "/" + id + "-" + std::to_string(n) + ".jsonl";
    if (!fs::exists(cand, ec)) return cand;
  }
  return base;  // 10k same-second collisions is not real; fall back to base rather than loop forever
}

std::string make_session_id() {
  const std::time_t now = std::time(nullptr);
  std::tm tm_buf{};
  localtime_r(&now, &tm_buf);  // POSIX, thread-safe (platform is linux)
  char buf[16] = {};
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm_buf);  // 15 chars + NUL
  return std::string(buf);
}

SessionResolution resolve_session_path(const std::string& dir, bool resume,
                                       const std::string& id, const std::string& new_id) {
  namespace fs = std::filesystem;
  // New session: collision-safe fresh path (append creates it). Not a fallback — it's deliberate.
  if (!resume) return {unique_fresh_path(dir, new_id), false};

  if (!id.empty()) {
    const std::string named = dir + "/" + id + ".jsonl";
    std::error_code ec;
    if (!fs::exists(named, ec))  // the user named a session that isn't there -> clear error
      throw MalConfig("no such session: " + id);
    return {named, false};
  }

  // resume with no id: pick the lexical-MAX *.jsonl filename (== newest timestamp). All entries
  // share the .jsonl suffix, so the full filename ordering matches the stem ordering.
  std::error_code ec;
  std::string newest;  // filename only
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() != ".jsonl") continue;
    const std::string fn = entry.path().filename().string();
    if (fn > newest) newest = fn;
  }
  // Nothing to resume (none found / dir missing) -> fresh path AND signal the fallback explicitly.
  if (newest.empty()) return {unique_fresh_path(dir, new_id), true};
  return {dir + "/" + newest, false};
}

}  // namespace hades

// ── tolerant per-session jsonl reader (was src/core/session_history.cpp) ──────────────
namespace hades {
std::vector<nlohmann::json> read_session_jsonl(const std::string& path) {
  std::vector<nlohmann::json> out;
  if (path.empty()) return out;
  std::ifstream f(path);
  if (!f) return out;  // missing file: fresh/absent session, not an error
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    auto j = nlohmann::json::parse(line, nullptr, false);  // false = no throw, returns discarded
    if (!j.is_discarded() && j.is_object()) out.push_back(std::move(j));
  }
  return out;
}
}  // namespace hades
