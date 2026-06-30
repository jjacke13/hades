// include/hades/module/chat_module.h — stdin/stdout REPL Module
//
// ChatModule attaches to the Blackboard and drives the human-facing loop:
// posts USER_MESSAGE from stdin, prints ASSISTANT_MESSAGE to stdout, and
// answers CONFIRM_REQUEST y/n. run_repl() blocks until EOF or /quit.

#pragma once
#include <iosfwd>
#include "hades/module.h"
namespace hades {
class Blackboard;
class ChatModule : public Module {
public:
  ChatModule() = default;
  ChatModule(const ChatModule&) = delete;
  ChatModule& operator=(const ChatModule&) = delete;
  std::string type() const override { return "chat"; }
  void on_attach(Blackboard& bb) override;
  void run_repl(std::istream& in, std::ostream& out);  // blocks until EOF or /quit
  // Test seam: override the run_until idle timeout so a unit test can force a fast turn
  // abandonment instead of waiting out the 180s production default. Any value > 0 takes
  // effect; production never calls this (keeps kTurnTimeoutS, see chat_module.cpp).
  void set_turn_timeout_s(double s) { turn_timeout_override_s_ = s; }
private:
  // Interactive REPL backed by GNU readline (line editing: arrows, history,
  // Ctrl-A/E, reverse i-search). Used only when stdin is a real TTY; otherwise
  // run_repl falls back to std::getline so piped/test input still works.
  void run_repl_readline();
  // Print one assistant-labelled line to out_ (same colored/plain path the
  // ASSISTANT_MESSAGE handler uses). Reused for the "[timed out]" abandonment notice
  // so a timed-out turn reads identically to a normal answer. No-op if out_ is null.
  void print_assistant_(const std::string& msg);
  // run_until returned false (idle timeout): the turn is abandoned. Post TURN_ABANDONED
  // (the Arbiter bumps its turn epoch on it, dropping any late worker response for this
  // turn) and pump once so that happens before the next prompt is read, then tell the
  // user. Used by both run_repl and run_repl_readline so the two paths behave identically.
  void abandon_turn_();

  Blackboard* bb_  = nullptr;
  std::ostream* out_ = nullptr;
  std::istream* in_  = nullptr;
  bool color_ = false;   // ANSI styling on; set in run_repl when stdout is a real TTY
  // Turn-completion latch: set true in the ASSISTANT_MESSAGE handler (the final
  // answer / [blocked] / [declined] / [stopped] all arrive as ASSISTANT_MESSAGE),
  // reset false before each posted USER_MESSAGE. run_until(turn_done_) lets the LLM
  // be offloaded to a worker without busy-pumping; inline turns set it during the
  // first pump so run_until returns at once.
  bool turn_done_ = false;
  // run_until idle-timeout override (seconds). 0 = use the production default
  // (kTurnTimeoutS in chat_module.cpp); set_turn_timeout_s gives tests a small value.
  double turn_timeout_override_s_ = 0.0;
};
}  // namespace hades
