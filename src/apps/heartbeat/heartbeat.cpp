// src/apps/heartbeat/heartbeat.cpp — HeartbeatModule: cron timer -> gated self-turns -> notify/drop
#include "hades/module/heartbeat_module.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include "hades/blackboard.h"
#include "hades/heartbeat/cron.h"
#include "hades/heartbeat/cron_store.h"
#include "hades/heartbeat/when.h"
#include "hades/timeouts.h"   // kDefaultTurnIdleTimeoutS
namespace hades {
namespace {
std::string trim(std::string s) {
  auto ns = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), ns));
  s.erase(std::find_if(s.rbegin(), s.rend(), ns).base(), s.end());
  return s;
}
}  // namespace

double HeartbeatModule::effective_timeout_() const {
  return turn_timeout_override_s_ > 0.0 ? turn_timeout_override_s_ : kDefaultTurnIdleTimeoutS;
}

void HeartbeatModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  // Capture the reply of a tick we drive (my_turn_), symmetric to the front-ends.
  bb.subscribe("ASSISTANT_MESSAGE", [this](const Entry& e) {
    if (!my_turn_ || !e.value.is_string()) return;
    last_reply_ = e.value.get<std::string>();
    got_reply_ = true;
  });
  // Track an outstanding confirm ACROSS all front-ends (un-gated) so a tick skips while a human's
  // async confirm is pending; then, if it's OUR tick, auto-deny (no human present, mirror Bridge).
  bb.subscribe("CONFIRM_REQUEST", [this](const Entry& e) {
    confirm_outstanding_ = true;                       // ANY confirm (human or ours) blocks a tick
    if (!my_turn_ || !e.value.is_object()) return;
    denied_confirm_ = true;
    auto id = e.value.find("id");
    bb_->post("CONFIRM_RESPONSE",
              {{"id", (id != e.value.end() && id->is_string()) ? id->get<std::string>() : ""},
               {"approved", false}},
              "heartbeat");
  });
  // Any confirm resolved (human answer / our auto-deny) -> a tick may fire again.
  bb.subscribe("CONFIRM_RESPONSE", [this](const Entry&) { confirm_outstanding_ = false; });
  // An abandoned turn clears its dangling confirm, else the heartbeat would skip forever.
  bb.subscribe("TURN_ABANDONED", [this](const Entry&) { confirm_outstanding_ = false; });
}

void HeartbeatModule::tick(const std::tm& now) {
  // A stamp unique to this wall-clock minute (year+yday+hour+min); dedups double-wakes in a minute.
  const long long minute =
      (static_cast<long long>(now.tm_year) * 100000000LL) + (now.tm_yday * 10000LL) +
      (now.tm_hour * 100LL) + now.tm_min;
  std::tm now_copy = now;
  const long long now_epoch = static_cast<long long>(std::mktime(&now_copy));
  if (!cron_store_.empty()) reload_dynamic_();          // pick up adds/cancels within one scan
  for (auto& e : entries_) {
    if (e.when.empty()) maybe_fire_(e, now, minute, now_epoch, /*dynamic=*/false);
    else                maybe_fire_when_(e, now_epoch, /*dynamic=*/false);
  }
  for (auto& e : dynamic_) {
    if (e.when.empty()) maybe_fire_(e, now, minute, now_epoch, /*dynamic=*/true);
    else                maybe_fire_when_(e, now_epoch, /*dynamic=*/true);
  }
}

void HeartbeatModule::maybe_fire_(HeartbeatEntry& e, const std::tm& now, long long minute,
                                  long long now_epoch, bool dynamic) {
  if (e.last_fired_minute == minute) return;             // already handled this minute (rescans)
  const bool match = e.one_shot ? (e.fire_epoch != 0 && now_epoch >= e.fire_epoch)
                                : cron_matches(e.schedule, now);
  if (!match) return;
  e.last_fired_minute = minute;                          // consume the minute (fire OR skip-if-busy)
  if (dynamic) last_fired_by_id_[e.id] = minute;         // carry the stamp across the next reload
  bool ran = false;
  try {
    ran = fire_(e);
  } catch (...) {
    bb_->post("HEARTBEAT_ERROR", e.name + " tick threw", "heartbeat");
  }
  if (dynamic && e.one_shot && ran && !cron_store_.empty()) {  // ONLY tombstone a one-shot that ACTUALLY ran
    std::ofstream f(cron_store_, std::ios::app);
    if (f) {
      f << done_record(e.id) << "\n";
      last_fired_by_id_.erase(e.id);                     // only forget the stamp if the done record persisted
    }
  }
}

void HeartbeatModule::maybe_fire_when_(HeartbeatEntry& e, long long now_epoch, bool dynamic) {
  // No minute-stamp gate here: edge detection + cooldown are the dedup for reactive entries.
  auto cond = parse_when(e.when);
  if (!cond) return;                                     // validated upstream; tolerate anyway
  auto entry = bb_->get(cond->key);
  const nlohmann::json* v = entry ? &entry->value : nullptr;

  bool edge = false;
  std::string new_dump = e.when_last_dump;
  bool new_true = e.when_was_true;
  if (cond->op == WhenCond::Op::Changes) {
    if (!v) { /* absent: hold state, nothing to compare */ }
    else {
      new_dump = v->dump();
      if (!e.when_armed) { e.when_armed = true; e.when_last_dump = new_dump; }   // arm, no fire
      else edge = (new_dump != e.when_last_dump);
    }
  } else {
    new_true = when_holds(*cond, v);
    edge = new_true && !e.when_was_true;
    if (!edge) e.when_was_true = new_true;               // re-arm on false; no-op while true
  }
  if (!edge) { sync_when_state_(e, dynamic); return; }

  // Cooldown: absorb the edge (advance state, no fire, no queueing).
  if (e.when_last_fire_epoch != 0 && now_epoch < e.when_last_fire_epoch + e.cooldown_s) {
    e.when_last_dump = new_dump;
    e.when_was_true = new_true;
    sync_when_state_(e, dynamic);
    return;
  }

  bool ran = false;
  try {
    ran = fire_(e);
  } catch (...) {
    bb_->post("HEARTBEAT_ERROR", e.name + " tick threw", "heartbeat");
    ran = true;                                          // a throw mid-turn: don't re-fire the same edge
  }
  if (ran) {                                             // consume the edge ONLY if the turn was driven
    e.when_last_dump = new_dump;
    e.when_was_true = new_true;
    e.when_last_fire_epoch = now_epoch;
  }
  sync_when_state_(e, dynamic);                          // busy-skip: state untouched -> retry next tick
}

void HeartbeatModule::sync_when_state_(const HeartbeatEntry& e, bool dynamic) {
  if (!dynamic || e.id.empty()) return;                  // static entries keep state in-place
  when_state_by_id_[e.id] = {e.when_armed, e.when_last_dump, e.when_was_true,
                             e.when_last_fire_epoch};
}

void HeartbeatModule::reload_dynamic_() {
  std::ifstream f(cron_store_);
  if (!f) { dynamic_.clear(); return; }
  std::stringstream ss; ss << f.rdbuf();
  std::vector<HeartbeatEntry> rebuilt;
  for (const auto& t : fold_cron_store(ss.str())) {
    HeartbeatEntry e;
    e.name = t.name; e.id = t.id; e.prompt = t.prompt; e.notify = t.notify;
    e.one_shot = (t.kind == "once");
    e.schedule = t.schedule; e.fire_epoch = t.fire_epoch;
    e.when = t.when; e.cooldown_s = t.cooldown_s;
    auto it = last_fired_by_id_.find(t.id);
    e.last_fired_minute = (it != last_fired_by_id_.end()) ? it->second : -1;
    if (auto ws = when_state_by_id_.find(t.id); ws != when_state_by_id_.end()) {
      e.when_armed = ws->second.armed;
      e.when_last_dump = ws->second.last_dump;
      e.when_was_true = ws->second.was_true;
      e.when_last_fire_epoch = ws->second.last_fire_epoch;
    }
    rebuilt.push_back(std::move(e));
  }
  // Prune dedup + when state for ids no longer active (a cancelled task folds out but left them).
  std::set<std::string> active_ids;
  for (const auto& e : rebuilt) active_ids.insert(e.id);
  for (auto it = last_fired_by_id_.begin(); it != last_fired_by_id_.end(); )
    it = active_ids.count(it->first) ? std::next(it) : last_fired_by_id_.erase(it);
  for (auto it = when_state_by_id_.begin(); it != when_state_by_id_.end(); )
    it = active_ids.count(it->first) ? std::next(it) : when_state_by_id_.erase(it);
  dynamic_ = std::move(rebuilt);
}

void HeartbeatModule::load_and_compact_() {
  if (cron_store_.empty()) return;
  std::ifstream f(cron_store_);
  if (!f) return;
  std::stringstream ss; ss << f.rdbuf();
  const std::string compacted = compact_cron_store(ss.str());
  std::ofstream out(cron_store_, std::ios::trunc);       // sole writer at boot
  if (out) out << compacted;
}

bool HeartbeatModule::fire_(HeartbeatEntry& e) {
  std::unique_lock<std::mutex> lk(turn_mu_(), std::try_to_lock);
  if (!lk.owns_lock()) {                              // a human/peer turn holds the gate -> skip (retry next tick)
    bb_->post("HEARTBEAT_SKIPPED", e.name, "heartbeat");
    return false;
  }
  if (confirm_outstanding_) {                         // async confirm freed the gate but pending_ set
    bb_->post("HEARTBEAT_SKIPPED", e.name + " (confirm pending)", "heartbeat");
    return false;
  }
  my_turn_ = true;
  struct Reset { bool& f; ~Reset() { f = false; } } reset{my_turn_};
  got_reply_ = false;
  last_reply_.clear();
  denied_confirm_ = false;
  bb_->post("TURN_ORIGIN", "heartbeat:" + e.name, "heartbeat");
  bb_->post("USER_MESSAGE", "(scheduled heartbeat \"" + e.name + "\") " + e.prompt, "heartbeat");
  const bool done = bb_->run_until([this] { return got_reply_; }, effective_timeout_());
  if (!done) {
    bb_->post("TURN_ABANDONED", nlohmann::json::object(), "heartbeat");
    bb_->pump();
    bb_->post("HEARTBEAT_ERROR", e.name + " turn timed out", "heartbeat");
    return true;   // the turn WAS driven -> a one-shot must NOT retry/re-run on timeout
  }
  if (e.notify) {
    std::string r = trim(last_reply_);
    if (!r.empty() && r != "SILENT") {                 // append the note AFTER the SILENT check
      if (denied_confirm_) r += "\n[note: a confirm-band action was auto-denied — no human present]";
      bb_->post("NOTIFY_USER", {{"text", r}, {"from", "heartbeat:" + e.name}}, "heartbeat");
      bb_->pump();   // dispatch to the notify sink on THIS thread while we still hold the gate
    }
  }
  return true;
}

void HeartbeatModule::start() {
  if (timer_thread_.joinable()) return;              // idempotent
  load_and_compact_();                               // drop tombstones/superseded on boot
  timer_thread_ = std::thread([this] {
    std::unique_lock<std::mutex> lk(timer_mu_);
    while (!timer_stop_) {
      if (timer_cv_.wait_for(lk, std::chrono::seconds(30), [this] { return timer_stop_; })) break;
      lk.unlock();
      try {
        std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        tick(local);
      } catch (...) { /* never let a tick escape the thread */ }
      lk.lock();
    }
  });
}

void HeartbeatModule::wait() {
  if (timer_thread_.joinable()) timer_thread_.join();
}

HeartbeatModule::~HeartbeatModule() {
  {
    std::lock_guard<std::mutex> lk(timer_mu_);
    timer_stop_ = true;
  }
  timer_cv_.notify_all();
  if (timer_thread_.joinable()) timer_thread_.join();
}
}  // namespace hades
