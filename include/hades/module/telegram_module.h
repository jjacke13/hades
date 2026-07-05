// include/hades/module/telegram_module.h — Telegram front-end app (comms-interface analogue)
//
// Long-polls the Bot API on its own thread and drives whole turns through the shared TurnGate,
// exactly like the REPL/HTTP front-ends: lock -> post USER_MESSAGE -> run_until(reply|confirm)
// -> sendMessage (split at 4096). Confirm-gated actions become an inline-keyboard message
// ([Approve]/[Deny] -> callback_query -> CONFIRM_RESPONSE). Security: allow_users (numeric ids)
// is REQUIRED (MalConfig without it) and non-allowed senders are silently dropped; the bot
// token comes from an env var (token_env) and never appears in the manifest. The startup
// backlog is drained AND DISCARDED so commands queued while the agent was down never replay.
// v1 is private-chat-only: group-chat messages are dropped (replies would be group-readable).
// The poll thread is started EXPLICITLY (start_polling, from hades_main) — never by on_attach —
// and is stop+joined in the dtor (before the Blackboard dies; embedding-timer precedent).
#pragma once
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <nlohmann/json.hpp>
#include "hades/module.h"
#include "hades/telegram/api.h"
#include "hades/turn_gate.h"
namespace hades {
class Blackboard;
class SttProvider;

class TelegramModule : public Module {
 public:
  TelegramModule() = default;
  explicit TelegramModule(std::unique_ptr<TelegramApi> api);   // test injection (skips token env)
  ~TelegramModule() override;                                  // stop + join the poll thread
  std::string type() const override { return "telegram"; }
  void on_start(const Block& cfg, Blackboard& bb) override;
  void on_attach(Blackboard& bb) override;

  // Shared whole-turn serializer (null -> module-local fallback).
  void set_turn_gate(TurnGate* g) { gate_ = g; }
  // run_until idle ceiling (0 -> default kDefaultTurnIdleTimeoutS); wiring sets the manifest value.
  void set_turn_timeout_s(double s) { turn_timeout_override_s_ = s; }
  // Optional STT provider (source-agnostic seam). Null -> voice updates get an "isn't enabled"
  // nudge. Injected by wire_agent from the Stt block; owned by the Agent (outlives the poll thread).
  void set_stt(SttProvider* s) { stt_ = s; }

  void start_polling();   // spawn the poll loop (called by hades_main when rostered)
  void wait();            // join the poll thread (telegram-only roster blocks here; Ctrl-C exits)
  // Process ONE get_updates batch synchronously (the loop body; public as the test seam).
  // First call drains-and-discards the startup backlog. Returns false on an api/parse error
  // (the loop backs off 5s before retrying).
  bool poll_once();

 private:
  void run_loop_();
  void drive_turn_(long long chat_id, const nlohmann::json& post_value, const char* key);
  void handle_text_(const TgUpdate& u);
  void handle_callback_(const TgUpdate& u);
  void handle_voice_(const TgUpdate& u);
  void send_reply_(long long chat_id, const std::string& text);
  std::mutex& turn_mu_() { return gate_ ? gate_->mu : local_gate_.mu; }
  double effective_timeout_() const;

  std::unique_ptr<TelegramApi> api_;
  SttProvider* stt_ = nullptr;   // non-owning; Agent owns it, declared before telegram (teardown)
  Blackboard* bb_ = nullptr;
  TurnGate* gate_ = nullptr;
  TurnGate local_gate_;
  std::set<long long> allow_;            // REQUIRED numeric user ids
  double poll_timeout_s_ = 50.0;
  double turn_timeout_override_s_ = 0.0;
  long long offset_ = 0;                 // next update id to fetch (in-memory, v1)
  bool drained_ = false;                 // startup backlog discarded yet?

  // Turn-capture state (poll thread only, under the gate while a turn runs).
  bool my_turn_ = false;                 // this module is driving the current turn
  bool got_reply_ = false;
  std::string last_reply_;
  nlohmann::json pending_confirm_;       // confirm captured during MY turn (null otherwise)
  std::string outstanding_confirm_id_;   // confirm sent to Telegram, awaiting callback
  long long outstanding_chat_id_ = 0;

  std::thread poll_thread_;
  std::atomic<bool> stop_{false};
  std::condition_variable stop_cv_;
  std::mutex stop_mu_;
};
}  // namespace hades
