// include/hades/session_id.h — session-id generation + per-session jsonl path resolution
//
// A session is a DAY, not a process launch: current_session_id() stamps the logical date
// ("YYYY-MM-DD") of the current local clock, so a restart mid-day resolves to the SAME file and
// rejoins the morning's conversation. make_session_id() ("YYYYMMDD-HHMMSS") is the older
// launch-stamp id, kept for reading pre-upgrade sessions but no longer used to NAME one (`/new`
// now rotates to a same-day sibling of the logical date). The binary may read the wall
// clock (only workflow scripts forbid it). resolve_session_path() turns the `--resume [id]` CLI +
// the Session block's sessions_dir into one jsonl path:
//   - new session (boot)  -> dir/<new_id>.jsonl  (OnCollision decides what an existing file means)
//   - resume <id>         -> dir/<id>.jsonl   (throws MalConfig if that file is absent)
//   - resume (no id)      -> the most recently MODIFIED dir/*.jsonl; fresh dir/<new_id>.jsonl if none.

#pragma once
#include <ctime>
#include <string>
namespace hades {

// Local hour a session "day" begins. 4 = 04:00, so 03:59 still belongs to yesterday; 0 = plain
// calendar midnight. The `Session.day_cutoff_hour` manifest key defaults to this.
inline constexpr int kDefaultDayCutoffHour = 4;

// Parse a `Session.day_cutoff_hour` value. Anything that is not a whole number in [0,23] —
// unparseable, trailing junk ("4h"), negative, >23, or an absent key (pass "") — yields
// kDefaultDayCutoffHour. House rule: garbage never yields 0 (0 is a MEANING here, plain calendar
// midnight, so silently producing it would move every session boundary four hours).
int resolve_day_cutoff_hour(const std::string& raw);

// Logical session date for an instant, as "YYYY-MM-DD". A session "day" runs from `cutoff_hour`
// to the same hour next day, LOCAL time, so anything before the cutoff belongs to the previous day
// (03:59 with cutoff 4 is still yesterday). cutoff_hour 0 = plain calendar date. Out-of-range
// cutoffs are clamped to [0,23] here so a bad config can never shift the date wildly.
std::string logical_date(std::time_t t, int cutoff_hour);

// logical_date() of the current local clock — the session id at boot and at daily rollover.
std::string current_session_id(int cutoff_hour);

// Launch timestamp id, e.g. "20260630-221544" (local time). The pre-daily-session id shape; no
// production caller mints one any more (kept: it documents the shape resume/search still read).
std::string make_session_id();

// First NON-EXISTING path among dir/<id>.jsonl, dir/<id>-1.jsonl, dir/<id>-2.jsonl, … (capped),
// so two sessions resolving to the same `id` never share one jsonl and interleave. Called by
// resolve_session_path (its Suffix collision policy, and its nothing-to-resume fallback), by the
// Arbiter's `/new` rotation, and by lock_session_file when a live process already holds the file.
std::string unique_fresh_path(const std::string& dir, const std::string& id);

// What an EXISTING dir/<new_id>.jsonl means for a new (non-resume) session. The two boot-shaped
// callers want opposite things from the same collision, so it is a parameter, not a policy:
enum class OnCollision {
  Suffix,  // never share a file: take the first free `-N` (`/new`, and same-second launch ids)
  Reuse,   // rejoin it: the daily boot path, where colliding with today's file IS the feature
};

// Result of resolving the per-session jsonl path. `fresh_fallback` is TRUE only when the caller
// asked to resume but the directory was empty/missing, so a fresh path was substituted — the
// front-end uses it to print a "starting fresh" note (an explicit flag, NOT a string compare).
struct SessionResolution {
  std::string path;
  bool fresh_fallback = false;
};

// Resolve the conversation jsonl path. `new_id` seeds a fresh path (no resume, or resume with
// nothing to resume); `on_collision` decides whether an existing dir/<new_id>.jsonl is rejoined
// (daily boot) or suffixed (never-share-a-file). Throws MalConfig when `resume && !id.empty()`
// but the named file is absent.
SessionResolution resolve_session_path(const std::string& dir, bool resume,
                                       const std::string& id, const std::string& new_id,
                                       OnCollision on_collision = OnCollision::Suffix);

// What a HELD file means to the caller — same shape as OnCollision above: the two callers want
// opposite things from one lock conflict, so it is a parameter, not a policy.
enum class OnHeld {
  Divert,  // "any usable session will do": default boot + bare `--resume` -> take a free `-N`
  Fail,    // "THIS session or nothing": `--resume <id>` named it -> MalConfig
};

// Claim `path` for THIS process with an exclusive advisory lock (flock LOCK_EX|LOCK_NB) and return
// the path actually claimed. OnCollision::Reuse dropped the old guarantee that a resolved path was
// ours alone — two hades sharing a sessions_dir now resolve to the SAME dir/<today>.jsonl, and
// sharing it interleaves their appends (an assistant(tool_calls) and its `tool` result land apart,
// which load_history only repairs at the ENDS of a file -> a provider 400 on every turn) besides
// loading each agent's whole conversation into the other. So: lock it, and when a LIVE process
// already holds it, either divert to the first free `-N` sibling (saying so on stderr) or throw,
// per `on_held`. A DEAD process has released its lock, so the point of the feature — a restart
// rejoins today's file — is untouched. Throws MalConfig on a held file when `on_held == OnHeld::Fail`.
//
// An flock lives on the open file description, so "hold the lock" is spelled "keep the fd open"
// and "release it" is spelled "close the fd". `out` decides which happens: pass one and the fd is
// stored there (destroying/reassigning the handle releases the lock — how a rotation stops holding
// yesterday's CLOSED session hostage against someone else's `--resume <yesterday>`); pass nullptr
// and the fd is deliberately leaked, i.e. held until the process exits.
class SessionLock {
public:
  SessionLock() = default;
  explicit SessionLock(int fd) : fd_(fd) {}
  ~SessionLock() { reset(); }
  SessionLock(SessionLock&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
  SessionLock& operator=(SessionLock&& o) noexcept {
    if (this != &o) { reset(); fd_ = o.fd_; o.fd_ = -1; }
    return *this;
  }
  SessionLock(const SessionLock&) = delete;
  SessionLock& operator=(const SessionLock&) = delete;
  void reset();          // close the held fd (releasing the flock); no-op when empty
  bool held() const { return fd_ >= 0; }

private:
  int fd_ = -1;
};

std::string lock_session_file(const std::string& path, OnHeld on_held = OnHeld::Divert,
                              SessionLock* out = nullptr);

}  // namespace hades
