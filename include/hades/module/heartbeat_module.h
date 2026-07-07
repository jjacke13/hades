// include/hades/module/heartbeat_module.h — cron self-trigger app (the autonomy leg)
//
// Owns a timer thread that wakes ~every 30s and fires a self-turn for each Heartbeat entry whose
// cron matches the machine-local minute (dedup once per minute). A tick is a NORMAL gated turn
// through the shared TurnGate: it try_locks the gate (skip-if-busy), posts TURN_ORIGIN=
// "heartbeat:<name>" + a USER_MESSAGE (the entry prompt), run_until()s the reply, then per the
// entry's notify flag forwards the reply to NOTIFY_USER (unless empty/"SILENT") or drops it. No
// human is present, so a confirm-band action is auto-denied. tick(std::tm) is the test seam (no
// clock/thread); start() spawns the thread (hades_main only); the dtor stop+joins it.
#pragma once
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "hades/module.h"
#include "hades/turn_gate.h"
namespace hades {
class Blackboard;

struct HeartbeatEntry {
  std::string name;
  std::string schedule;              // 5-field cron
  std::string prompt;                // resolved (inline or from prompt_file) at wiring
  bool notify = false;
  long long last_fired_minute = -1;  // per-entry minute-stamp dedup
};

class HeartbeatModule : public Module {
 public:
  std::string type() const override { return "heartbeat"; }
  void on_attach(Blackboard& bb) override;
  void add_entry(HeartbeatEntry e) { entries_.push_back(std::move(e)); }
  void set_turn_gate(TurnGate* g) { gate_ = g; }
  void set_turn_timeout_s(double s) { turn_timeout_override_s_ = s; }

  void tick(const std::tm& now);     // TEST SEAM: fire due entries for this wall-clock minute
  void start();                       // spawn the timer thread (hades_main; idempotent)
  void wait();                        // join the timer thread (heartbeat-only roster keep-alive)
  ~HeartbeatModule() override;        // stop + join

 private:
  void fire_(HeartbeatEntry& e);
  std::mutex& turn_mu_() { return gate_ ? gate_->mu : local_gate_.mu; }
  double effective_timeout_() const;

  std::vector<HeartbeatEntry> entries_;
  Blackboard* bb_ = nullptr;
  TurnGate* gate_ = nullptr;
  TurnGate local_gate_;
  double turn_timeout_override_s_ = 0.0;

  // Turn-capture state (timer thread only, under the gate while a tick runs).
  bool my_turn_ = false;
  bool got_reply_ = false;
  bool denied_confirm_ = false;
  std::string last_reply_;

  std::thread timer_thread_;
  std::mutex timer_mu_;
  std::condition_variable timer_cv_;
  bool timer_stop_ = false;
};
}  // namespace hades
