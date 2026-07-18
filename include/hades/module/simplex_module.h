// include/hades/module/simplex_module.h — SimpleX Chat front-end app (comms-interface analogue)
//
// Reads events from a local simplex-chat daemon (via the SimplexApi seam) on its own thread and
// drives whole turns through the shared TurnGate, exactly like the Telegram front-end: lock ->
// post USER_MESSAGE -> run_until(reply|confirm) -> send_text (split at 4000). Confirms are TEXT
// y/N (SimpleX has no inline keyboards): the next message from the SAME contact answers an
// outstanding confirm. Security: allow_contacts (ids and/or exact display names) is REQUIRED
// (MalConfig without it); non-allowed senders are silently dropped; contact requests are only
// auto-accepted when auto_accept=true (name-spoof risk documented in the manifest reference).
// The thread is started EXPLICITLY (start(), from hades_main) — never by on_attach — and is
// stop+joined in the dtor (telegram precedent).
#pragma once
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include "hades/module.h"
#include "hades/simplex/api.h"
#include "hades/turn_gate.h"
namespace hades {
class Blackboard;

class SimplexModule : public Module {
 public:
  SimplexModule() = default;
  explicit SimplexModule(std::unique_ptr<SimplexApi> api);   // test injection (skips the WS api)
  ~SimplexModule() override;                                 // stop + join the event thread
  std::string type() const override { return "simplex"; }
  void on_start(const Block& cfg, Blackboard& bb) override;
  void on_attach(Blackboard& bb) override;

  void set_turn_gate(TurnGate* g) { gate_ = g; }
  void set_turn_timeout_s(double s) { turn_timeout_override_s_ = s; }

  void start();          // spawn the daemon child (if `command` set) + the event loop (hades_main)
  void wait();           // join the event thread (simplex-only roster blocks here)
  int daemon_pid() const { return daemon_pid_; }   // 0 = no daemon spawned (test seam)
  // One next_event dispatch (the loop body; public as the test seam). Returns false on
  // Closed/Error — the loop then backs off and reconnects.
  bool step_once();

 private:
  void run_loop_();
  void handle_event_(const SxEvent& ev);
  void handle_text_(const SxEvent& ev);
  void drive_turn_(long long contact_id, const nlohmann::json& post_value, const char* key);
  void send_reply_(long long contact_id, const std::string& text);
  void drain_notifies_();                        // event thread only (sends over the one socket)
  bool allowed_(const SxEvent& ev) const;
  std::mutex& turn_mu_() { return gate_ ? gate_->mu : local_gate_.mu; }
  double effective_timeout_() const;

  std::unique_ptr<SimplexApi> api_;
  Blackboard* bb_ = nullptr;
  TurnGate* gate_ = nullptr;
  TurnGate local_gate_;
  std::set<long long> allow_ids_;
  std::set<std::string> allow_names_;
  std::map<std::string, long long> known_ids_;   // display name -> contact id (learned from events)
  bool auto_accept_ = false;
  std::string notify_contact_;                   // id-or-name; "" = no notify delivery
  // NOTIFY_USER texts queued by the subscriber (which runs on the POSTING thread — e.g. the
  // heartbeat timer) and drained on the event thread, which alone may touch api_/known_ids_.
  std::mutex notify_mu_;
  std::vector<std::string> notify_queue_;
  double connect_timeout_s_ = 10.0;
  double poll_timeout_s_ = 25.0;                 // internal next_event wait per loop pass
  double turn_timeout_override_s_ = 0.0;

  // Turn-capture state (event thread only, under the gate while a turn runs).
  bool my_turn_ = false;
  bool got_reply_ = false;
  std::string last_reply_;
  nlohmann::json pending_confirm_;
  std::string outstanding_confirm_id_;           // confirm prompt sent, awaiting the y/N text
  long long outstanding_contact_id_ = 0;

  // Optional auto-started daemon (`Simplex.command`): spawned by start() BEFORE the event
  // thread (the reconnect backoff absorbs the daemon's boot time), SIGTERM+reaped by the dtor
  // AFTER the thread joins. Empty argv = external daemon (default, unchanged).
  void spawn_daemon_();
  void stop_daemon_();
  std::vector<std::string> daemon_argv_;
  int daemon_pid_ = 0;

  std::thread ev_thread_;
  std::atomic<bool> stop_{false};
  std::condition_variable stop_cv_;
  std::mutex stop_mu_;
};
}  // namespace hades
