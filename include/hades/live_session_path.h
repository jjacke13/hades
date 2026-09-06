// include/hades/live_session_path.h — the live session file, followed across daily rotation
//
// A session is a DAY (see session_id.h), so the path of the LIVE session file moves while the
// process runs: the Arbiter posts SESSION_ROTATED {from,to,path} at the daily rollover and on
// `/new`. Two modules cache that path and read it from a NON-pump thread — the embedding
// module's index worker (which excludes the live, mid-write session from the corpus) and the
// HTTP front-end's GET /history handler (httplib thread) — so both need the same two things: a
// mutex, and the "an empty path means nothing rotated anywhere useful" rule. One holder, so the
// rule cannot drift between them.
#pragma once
#include <mutex>
#include <string>
#include <utility>
#include <nlohmann/json.hpp>
namespace hades {

class LiveSessionPath {
public:
  void set(std::string p) {
    std::lock_guard<std::mutex> lk(mu_);
    p_ = std::move(p);
  }
  std::string get() const {
    std::lock_guard<std::mutex> lk(mu_);
    return p_;
  }
  // Adopt the path from a SESSION_ROTATED payload. An absent/non-string/EMPTY path is IGNORED:
  // an Arbiter with no sessions_dir rotates to nowhere and posts {to:"",path:""}, and assigning
  // that would silently disable the exclusion (index the live file / render an empty transcript)
  // instead of leaving the last known-good path in place.
  void on_rotated(const nlohmann::json& v) {
    if (!v.is_object()) return;
    const auto it = v.find("path");
    if (it == v.end() || !it->is_string()) return;
    std::string p = it->get<std::string>();
    if (p.empty()) return;
    set(std::move(p));
  }

private:
  mutable std::mutex mu_;
  std::string p_;
};

}  // namespace hades
