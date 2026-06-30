// include/hades/session_id.h — session-id generation + per-session jsonl path resolution
//
// make_session_id() stamps a launch id from the real local clock ("YYYYMMDD-HHMMSS");
// the binary may read the wall clock (only workflow scripts forbid it). resolve_session_path()
// turns the `--resume [id]` CLI + the Session block's sessions_dir into one jsonl path:
//   - new session       -> dir/<new_id>.jsonl  (first free `-N` suffix if that file exists)
//   - resume <id>        -> dir/<id>.jsonl   (throws MalConfig if that file is absent)
//   - resume (no id)     -> the lexical-newest dir/*.jsonl; fresh dir/<new_id>.jsonl if none.

#pragma once
#include <string>
namespace hades {

// Launch timestamp id, e.g. "20260630-221544" (local time). Collision-safe for human-paced launches.
std::string make_session_id();

// First NON-EXISTING path among dir/<id>.jsonl, dir/<id>-1.jsonl, dir/<id>-2.jsonl, … (capped),
// so two sessions resolving to the same `id` in the same wall-clock second never share one jsonl
// and interleave. Used by both the initial resolve_session_path and the Arbiter's `/new` rotation.
std::string unique_fresh_path(const std::string& dir, const std::string& id);

// Result of resolving the per-session jsonl path. `fresh_fallback` is TRUE only when the caller
// asked to resume but the directory was empty/missing, so a fresh path was substituted — the
// front-end uses it to print a "starting fresh" note (an explicit flag, NOT a string compare).
struct SessionResolution {
  std::string path;
  bool fresh_fallback = false;
};

// Resolve the conversation jsonl path. `new_id` seeds a fresh path (no resume, or resume with
// nothing to resume); a NEW session that collides with an existing same-second file gets the first
// free `dir/<new_id>-N.jsonl` so two launches never share one file. Throws MalConfig when
// `resume && !id.empty()` but the named file is absent.
SessionResolution resolve_session_path(const std::string& dir, bool resume,
                                       const std::string& id, const std::string& new_id);

}  // namespace hades
