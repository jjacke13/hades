// src/core/session.cpp — session identity + persisted-conversation reads
//
// Merged (2026-07-04 src reorg): session_id (launch-timestamp ids, collision-safe
// unique_fresh_path, --resume resolution) + session_history (tolerant per-session
// jsonl reader shared by the Arbiter's load_history and GET /history).

#include <fcntl.h>     // open
#include <sys/file.h>  // flock
#include <unistd.h>    // close
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <utility>
#include <nlohmann/json.hpp>
#include "hades/session_id.h"
#include "hades/session_history.h"
#include "hades/embedding/session_turns.h"
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

// Logical session date: the calendar date of (t − cutoff_hour). Implemented by subtracting
// SECONDS and letting localtime_r normalise — a hand-rolled day/month decrement gets 2026-01-01
// and DST days wrong; the C library gets both right for free. The cutoff is clamped so a garbage
// config can shift the date by at most a day, never wildly.
// ponytail: the shift is 4 ELAPSED hours, so on a spring-forward DST day the boundary lands one
// wall-clock hour late (04:30 still reads as yesterday). Once a year, invisible to a conversation;
// wall-clock-exact would need a normalise-twice dance for no user-visible gain.
std::string logical_date(std::time_t t, int cutoff_hour) {
  if (cutoff_hour < 0) cutoff_hour = 0;
  if (cutoff_hour > 23) cutoff_hour = 23;
  const std::time_t shifted = t - static_cast<std::time_t>(cutoff_hour) * 3600;
  std::tm tm_buf{};
  localtime_r(&shifted, &tm_buf);  // POSIX, thread-safe (platform is linux)
  char buf[16] = {};
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);  // 10 chars + NUL
  return std::string(buf);
}

std::string current_session_id(int cutoff_hour) {
  return logical_date(std::time(nullptr), cutoff_hour);
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
                                       const std::string& id, const std::string& new_id,
                                       OnCollision on_collision) {
  namespace fs = std::filesystem;
  // New session (append creates the file). Not a fallback — it's deliberate. What an EXISTING file
  // for `new_id` MEANS is the caller's call, and the two callers disagree by design:
  //   Reuse  — the daily boot path: `new_id` is today's logical date, so an existing file is this
  //            morning's conversation and rejoining it is the entire point of the feature.
  //   Suffix — never share a file: two sessions resolving one id get separate jsonls (`/new`).
  if (!resume) {
    if (on_collision == OnCollision::Reuse) return {dir + "/" + new_id + ".jsonl", false};
    return {unique_fresh_path(dir, new_id), false};
  }

  if (!id.empty()) {
    const std::string named = dir + "/" + id + ".jsonl";
    std::error_code ec;
    if (!fs::exists(named, ec))  // the user named a session that isn't there -> clear error
      throw MalConfig("no such session: " + id);
    return {named, false};
  }

  // resume with no id: pick the most recently MODIFIED *.jsonl. Ordering by filename is wrong now
  // that two id shapes coexist — at index 4 a date id has '-' (0x2D) and a launch stamp a digit
  // (0x30+), so EVERY "YYYY-MM-DD.jsonl" sorts below EVERY "YYYYMMDD-HHMMSS.jsonl" and a bare
  // `--resume` would append today's turns to a pre-upgrade session forever. mtime is what "newest"
  // means and is format-independent. An unreadable stamp sorts oldest; equal stamps (coarse
  // filesystem granularity) break the tie on filename, so the pick is always deterministic.
  std::error_code ec;
  std::string newest;  // filename only
  fs::file_time_type newest_mtime = fs::file_time_type::min();
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() != ".jsonl") continue;
    const std::string fn = entry.path().filename().string();
    std::error_code tec;
    fs::file_time_type mtime = entry.last_write_time(tec);
    if (tec) mtime = fs::file_time_type::min();
    if (newest.empty() || mtime > newest_mtime || (mtime == newest_mtime && fn > newest)) {
      newest = fn;
      newest_mtime = mtime;
    }
  }
  // Nothing to resume (none found / dir missing) -> fresh path AND signal the fallback explicitly.
  if (newest.empty()) return {unique_fresh_path(dir, new_id), true};
  return {dir + "/" + newest, false};
}

namespace {
constexpr int kHeldByOther = -1;  // flock refused: another LIVE process owns this file
constexpr int kCannotOpen = -2;   // open() itself failed (perms) — locking is moot, not a conflict

// Open (creating if absent) + exclusively flock `path`, non-blocking. Returns the fd on success —
// INTENTIONALLY leaked by the caller, because flock is released by the last close of the open file
// description (and by process exit), so "hold the lock for the process lifetime" is spelled "never
// close the fd".
int open_locked(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return kCannotOpen;
  if (::flock(fd, LOCK_EX | LOCK_NB) == 0) return fd;  // ours until we exit
  ::close(fd);
  return kHeldByOther;
}
}  // namespace

std::string lock_session_file(const std::string& path, OnHeld on_held) {
  if (path.empty()) return path;
  namespace fs = std::filesystem;
  const fs::path p(path);
  std::error_code ec;
  if (!p.parent_path().empty()) fs::create_directories(p.parent_path(), ec);  // first boot ever
  // Ours now (fd leaked on purpose), or unopenable — an unwritable sessions_dir is not a conflict
  // and gets reported by the first append; only a real lock conflict may divert the path.
  if (open_locked(path) != kHeldByOther) return path;

  // An explicit `--resume <id>` asked for THIS session; quietly handing back a DIFFERENT one is
  // the surprising outcome, and inconsistent with the `--resume <absent-id>` MalConfig right next
  // to it in resolve_session_path — an explicit request that cannot be honoured should fail loudly.
  // The default boot path and bare `--resume` ("newest, whatever that is") express no preference,
  // so they divert instead.
  if (on_held == OnHeld::Fail)
    throw MalConfig("session is open in another running hades: " + path);

  // Taken by a LIVE hades (a dead one released its lock, so a plain restart still rejoins today's
  // file). Divert to a private `-N` sibling. Re-checking the lock each iteration is load-bearing,
  // not paranoia: two hades booting together both see the same free suffix before either has
  // created it, and the loser must take the next one instead of silently sharing.
  const std::string dir = p.parent_path().empty() ? std::string(".") : p.parent_path().string();
  std::string alt = path;
  for (int tries = 0; tries < 16; ++tries) {
    const std::string cand = unique_fresh_path(dir, p.stem().string());
    if (cand == path) break;  // suffix space exhausted (10k files) — nothing free to divert to
    if (open_locked(cand) >= 0) {
      alt = cand;
      break;
    }
  }
  // Always say it: a session silently splitting in two is exactly the kind of divergence that is
  // impossible to diagnose after the fact, so name both paths.
  if (alt == path)
    std::cerr << "hades: session file " << path << " is held by another running hades and no free"
              << " sibling remains; sharing it (appends WILL interleave)\n";
  else
    std::cerr << "hades: session file " << path << " is held by another running hades; using "
              << alt << " instead\n";
  return alt;
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

// ── session_turns: extract per-turn "U:…\nA:…" units from a session jsonl (moved 2026-07-16 from embedding_memory.cpp so tool binaries can compile session.cpp standalone) ──────────────
namespace hades {
// Safe role read: a non-string "role" (corrupt/external line) must NOT throw type_error (the
// function's never-throws contract) — treat anything but a string role as "" (no match).
static std::string role_of(const nlohmann::json& m) {
  auto it = m.find("role");
  return (it != m.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}
std::vector<SessionTurn> extract_session_turns(const std::string& session_file) {
  std::vector<SessionTurn> turns;
  const auto msgs = read_session_jsonl(session_file);
  const std::string base = std::filesystem::path(session_file).filename().string();
  std::size_t idx = 0;
  for (std::size_t i = 0; i < msgs.size(); ++i) {
    if (role_of(msgs[i]) != "user") continue;
    if (!msgs[i].contains("content") || !msgs[i]["content"].is_string()) continue;
    const std::string user = msgs[i]["content"].get<std::string>();
    // find the NEXT assistant message with string content (fold tool / tool_call assistants)
    std::string answer;
    for (std::size_t j = i + 1; j < msgs.size(); ++j) {
      if (role_of(msgs[j]) == "user") break;             // next turn started, no answer
      if (role_of(msgs[j]) == "assistant" && msgs[j].contains("content") && msgs[j]["content"].is_string()) {
        answer = msgs[j]["content"].get<std::string>(); break;
      }
    }
    if (answer.empty()) continue;                                  // unanswered user -> drop
    turns.push_back({"session:" + base + "#" + std::to_string(idx), "U: " + user + "\nA: " + answer});
    ++idx;
  }
  return turns;
}
}  // namespace hades
