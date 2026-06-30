// include/hades/session_id.h — session-id generation + per-session jsonl path resolution
//
// make_session_id() stamps a launch id from the real local clock ("YYYYMMDD-HHMMSS");
// the binary may read the wall clock (only workflow scripts forbid it). resolve_session_path()
// turns the `--resume [id]` CLI + the Session block's sessions_dir into one jsonl path:
//   - new session       -> dir/<new_id>.jsonl
//   - resume <id>        -> dir/<id>.jsonl   (throws MalConfig if that file is absent)
//   - resume (no id)     -> the lexical-newest dir/*.jsonl; fresh dir/<new_id>.jsonl if none.

#pragma once
#include <string>
namespace hades {

// Launch timestamp id, e.g. "20260630-221544" (local time). Collision-safe for human-paced launches.
std::string make_session_id();

// Resolve the conversation jsonl path. `new_id` is used when starting fresh (no resume, or resume
// with nothing to resume). Throws MalConfig when `resume && !id.empty()` but the named file is absent.
std::string resolve_session_path(const std::string& dir, bool resume,
                                 const std::string& id, const std::string& new_id);

}  // namespace hades
