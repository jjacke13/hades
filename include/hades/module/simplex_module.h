// include/hades/module/simplex_module.h — SimpleX Chat front-end app (comms-interface analogue)
//
// Reads events from a local simplex-chat daemon (via the SimplexApi seam) on its own thread and
// drives whole turns through the shared TurnGate, exactly like the Telegram front-end: lock ->
// post USER_MESSAGE -> run_until(reply|confirm) -> send_text (split at 4000). Confirms are TEXT
// y/N (SimpleX has no inline keyboards): the next message from the SAME contact answers an
// outstanding confirm. Security: allow_contacts (ids and/or exact display names) is REQUIRED
// (MalConfig without it); non-allowed senders are silently dropped; contact requests are only
// auto-accepted when auto_accept=true (name-spoof risk documented in the manifest reference).
// A voice message is accepted explicitly (/freceive, size-capped, allowlisted senders only) and
// transcribed via the optional SttProvider into an ordinary turn; without a provider it is refused.
// The thread is started EXPLICITLY (start(), from hades_main) — never by on_attach — and is
// stop+joined in the dtor (telegram precedent).
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
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
class SttProvider;

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
  // Voice input (opt-in, mirrors TelegramModule): without a provider a voice message is refused
  // with a text reply and never accepted off the daemon. Injected by wire_agent from the Stt
  // block; owned by the Agent (declared before simplex, so it outlives the event thread).
  void set_stt(SttProvider* s) { stt_ = s; }
  void set_voice_max_bytes(long long n) { if (n > 0) voice_max_bytes_ = n; }
  // Read-back seams for the wiring tests: no event thread exists there (start() is never called
  // in a test, and wire_agent sets both before it could be), so these race with nothing.
  const SttProvider* stt() const { return stt_; }
  long long voice_max_bytes() const { return voice_max_bytes_; }

  void start();          // spawn the daemon child (if `command` set) + the event loop (hades_main)
  void wait();           // join the event thread (simplex-only roster blocks here)
  int daemon_pid() const { return daemon_pid_; }   // 0 = no daemon spawned (test seam)
  // One next_event dispatch (the loop body; public as the test seam). Returns false on
  // Closed/Error — the loop then backs off and reconnects.
  // LOAD-BEARING: this must stay a TEST seam. voice_dir_ok_ defaults true and voice_tmp_dir_
  // defaults empty, which is safe only because production always reaches step_once() through
  // start() (which sets both). Drive this loop without start() from real code and voice temp
  // files escape the per-process directory and outlive the process.
  bool step_once();

 private:
  void run_loop_();
  void handle_event_(const SxEvent& ev);
  void handle_text_(const SxEvent& ev);
  void handle_voice_(const SxEvent& ev);
  void handle_file_done_(const SxEvent& ev);
  void handle_file_failed_(const SxEvent& ev);
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
  // Voice transfers we accepted and are waiting on: fileId -> where we told the daemon to put
  // the bytes. EVENT THREAD ONLY (step_once and what it calls) — correct without a mutex
  // precisely because it never leaves that thread; do not touch it from anywhere else.
  // Bounded: a sender whose transfers never complete must not grow it without limit.
  struct PendingVoice { long long contact_id; std::string path; };
  std::map<long long, PendingVoice> pending_voice_;
  static constexpr std::size_t kMaxPendingVoice = 8;
  // Per-PROCESS temp directory for the audio (<tmp>/hades-sx-voice-<pid>), created by start()
  // and removed whole by the dtor. The per-entry remove() calls stay the fast path; the
  // directory reclaims what they cannot see — a transfer still in flight at shutdown, a
  // /freceive the daemon accepted before receive_file reported failure, an evicted entry.
  // Deliberately NOT a boot-time sweep of hades-sx-voice-*: several hades instances share a
  // machine in this deployment and a starting one would delete another's live downloads. Empty
  // (start() never called — tests drive step_once directly) -> the system temp dir is used
  // directly and the dtor removes nothing. A hard crash leaves one directory behind: accepted.
  std::filesystem::path voice_tmp_dir_;
  // Is voice storage usable? Set by start() — false first, true ONLY on a created directory —
  // so BOTH of its failure modes refuse voice with a reply: a create_directories error (dir set
  // but unusable) and a throwing temp_directory_path() (dir left EMPTY, which no path-based
  // check can tell apart from "start() never called"). Defaults true for that never-called
  // case: tests drive step_once() directly and fall back to the system temp dir, as before.
  bool voice_dir_ok_ = true;
  // file_id names the file; the offered name contributes ONLY a validated extension (the http
  // STT backend format-checks the uploaded basename). See voice_ext_from_name in the .cpp.
  std::string voice_temp_path_(long long file_id, const std::string& offered_name) const;
  SttProvider* stt_ = nullptr;                   // non-owning; null = voice input disabled
  long long voice_max_bytes_ = 10 * 1024 * 1024;

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
