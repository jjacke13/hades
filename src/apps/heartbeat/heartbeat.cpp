// src/apps/heartbeat/heartbeat.cpp — HeartbeatModule: cron timer -> gated self-turns -> notify/drop
#include "hades/module/heartbeat_module.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>
#include "hades/blackboard.h"
#include "hades/heartbeat/cron.h"
#include "hades/heartbeat/cron_store.h"
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
  for (auto& e : entries_) maybe_fire_(e, now, minute, now_epoch, /*dynamic=*/false);
  for (auto& e : dynamic_) maybe_fire_(e, now, minute, now_epoch, /*dynamic=*/true);
}

void HeartbeatModule::maybe_fire_(HeartbeatEntry& e, const std::tm& now, long long minute,
                                  long long now_epoch, bool dynamic) {
  if (e.last_fired_minute == minute) return;             // already handled this minute (rescans)
  const bool match = e.one_shot ? (e.fire_epoch != 0 && now_epoch >= e.fire_epoch)
                                : cron_matches(e.schedule, now);
  if (!match) return;
  e.last_fired_minute = minute;                          // consume the minute (fire OR skip-if-busy)
  if (dynamic) last_fired_by_id_[e.id] = minute;         // carry the stamp across the next reload
  try {
    fire_(e);
  } catch (...) {
    bb_->post("HEARTBEAT_ERROR", e.name + " tick threw", "heartbeat");
  }
  if (dynamic && e.one_shot && !cron_store_.empty()) {   // one-shot done -> tombstone, never re-fires
    std::ofstream f(cron_store_, std::ios::app);
    if (f) f << done_record(e.id) << "\n";
    last_fired_by_id_.erase(e.id);                       // gone after the next reload
  }
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
    auto it = last_fired_by_id_.find(t.id);
    e.last_fired_minute = (it != last_fired_by_id_.end()) ? it->second : -1;
    rebuilt.push_back(std::move(e));
  }
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

void HeartbeatModule::fire_(HeartbeatEntry& e) {
  std::unique_lock<std::mutex> lk(turn_mu_(), std::try_to_lock);
  if (!lk.owns_lock()) {                              // a human/peer turn holds the gate -> skip
    bb_->post("HEARTBEAT_SKIPPED", e.name, "heartbeat");
    return;
  }
  if (confirm_outstanding_) {                         // async confirm freed the gate but pending_ set
    bb_->post("HEARTBEAT_SKIPPED", e.name + " (confirm pending)", "heartbeat");
    return;
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
    return;
  }
  if (e.notify) {
    std::string r = trim(last_reply_);
    if (!r.empty() && r != "SILENT") {                 // append the note AFTER the SILENT check
      if (denied_confirm_) r += "\n[note: a confirm-band action was auto-denied — no human present]";
      bb_->post("NOTIFY_USER", {{"text", r}, {"from", "heartbeat:" + e.name}}, "heartbeat");
      bb_->pump();   // dispatch to the notify sink on THIS thread while we still hold the gate
    }
  }
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
