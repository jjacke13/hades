// src/core/session_id.cpp — session-id generation + per-session jsonl path resolution
//
// See session_id.h. The binary reads the real local clock for the launch id; path resolution
// is pure filesystem logic (newest-by-lexical-filename selection, MalConfig on a missing named
// session). `.jsonl` files are append-created by the Arbiter, so no directory is created here.

#include "hades/session_id.h"
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include "hades/launcher.h"  // MalConfig
namespace hades {

std::string make_session_id() {
  const std::time_t now = std::time(nullptr);
  std::tm tm_buf{};
  localtime_r(&now, &tm_buf);  // POSIX, thread-safe (platform is linux)
  char buf[16] = {};
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm_buf);  // 15 chars + NUL
  return std::string(buf);
}

std::string resolve_session_path(const std::string& dir, bool resume,
                                 const std::string& id, const std::string& new_id) {
  namespace fs = std::filesystem;
  const std::string fresh = dir + "/" + new_id + ".jsonl";
  if (!resume) return fresh;  // new session: no existence requirement (append creates it)

  if (!id.empty()) {
    const std::string named = dir + "/" + id + ".jsonl";
    std::error_code ec;
    if (!fs::exists(named, ec))  // the user named a session that isn't there -> clear error
      throw MalConfig("no such session: " + id);
    return named;
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
  if (newest.empty()) return fresh;  // none found / dir missing -> start fresh (caller may note)
  return dir + "/" + newest;
}

}  // namespace hades
