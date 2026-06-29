// src/module/chat_module.cpp — stdin/stdout REPL; USER_MESSAGE / CONFIRM I/O
//
// Implements ChatModule: run_repl() drives a line-by-line user prompt loop,
// posting USER_MESSAGE and calling Blackboard::pump() each turn. Subscribes
// to ASSISTANT_MESSAGE (prints to stdout) and CONFIRM_REQUEST (reads y/N from
// stdin, posts CONFIRM_RESPONSE) so the Arbiter's gating loop reaches the user.

#include "hades/module/chat_module.h"
#include "hades/blackboard.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <istream>
#include <ostream>
#include <string>
#include <unistd.h>
#include <readline/history.h>
#include <readline/readline.h>
namespace hades {

namespace {
// ANSI styling, emitted only when stdout is a real TTY (color_). Bold cyan for the
// user prompt, bold green for assistant output, bold yellow for the confirm prompt.
constexpr const char* kBoldCyan   = "\033[1;36m";
constexpr const char* kBoldGreen  = "\033[1;32m";
constexpr const char* kBoldYellow = "\033[1;33m";
constexpr const char* kReset      = "\033[0m";

// Generous IDLE ceiling for run_until — NOT a per-turn wall-clock cap. The timer
// resets on every bus event, so it fires only after this many seconds of NO bus
// activity (a genuinely hung/stalled worker). A turn may offload a (possibly slow)
// LLM call onto a worker, so the REPL waits on the bus instead of busy-pumping; a
// legitimately long but bus-active multi-step turn can exceed this wall-clock and is
// NOT killed. Only true silence trips it, and the loop moves on to the next prompt.
// Inline turns (no executor, e.g. the echo test) set turn_done_ during the first
// pump -> returns immediately.
constexpr double kTurnTimeoutS = 180.0;
}  // namespace

void ChatModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  bb.subscribe("ASSISTANT_MESSAGE", [this](const Entry& e) {
    // The turn's final message (answer / [blocked] / [declined] / [stopped]) — latch
    // turn completion so run_until() returns once the turn has resolved.
    turn_done_ = true;
    if (!out_ || !e.value.is_string()) return;
    const std::string msg = e.value.get<std::string>();
    // Interactive: blank line before + bold-green label + blank line after, so each
    // turn reads as its own block. Non-TTY (pipe/test): byte-identical to the original.
    if (color_)
      (*out_) << "\n" << kBoldGreen << "assistant> " << kReset << msg << "\n\n";
    else
      (*out_) << "assistant> " << msg << "\n";
  });
  bb.subscribe("CONFIRM_REQUEST", [this](const Entry& e) {
    if (!in_) return;  // no interactive stream -> cannot answer; skip
    const std::string prompt = e.value.value("prompt", "");
    if (out_) {
      if (color_)
        (*out_) << "\n" << kBoldYellow << "confirm> " << kReset << prompt << " [y/N]: ";
      else
        (*out_) << "confirm> " << prompt << " [y/N]: ";
    }
    std::string ans;
    std::getline(*in_, ans);
    bool yes = (ans == "y" || ans == "Y" || ans == "yes");
    bb_->post("CONFIRM_RESPONSE",
              {{"id", e.value.value("id", "")}, {"approved", yes}},
              "chat");
  });
}

void ChatModule::run_repl(std::istream& in, std::ostream& out) {
  in_  = &in;
  out_ = &out;
  struct Guard { ChatModule* self; ~Guard(){ self->in_=nullptr; self->out_=nullptr; self->color_=false; } } guard{this};

  // Color only when writing to a real terminal on stdout (not a pipe/file/test stream),
  // so piped output and tests stay free of ANSI escapes.
  color_ = (&out == &std::cout) && isatty(fileno(stdout));

  // Interactive terminal -> readline (arrows/history/editing). Anything else
  // (piped stdin, a test stringstream, a redirected file) -> plain getline so
  // the injected-stream seam keeps working and tests stay deterministic.
  if (&in == &std::cin && isatty(fileno(stdin))) {
    run_repl_readline();
    return;
  }

  std::string line;
  while (true) {
    out << "user> " << std::flush;
    if (!std::getline(in, line)) break;
    if (line == "/quit") break;
    if (line.empty()) continue;
    turn_done_ = false;
    bb_->post("USER_MESSAGE", line, "chat");
    // Drive the turn to its final ASSISTANT_MESSAGE; with an Executor the LLM runs on
    // a worker and run_until sleeps on the bus until it posts back (inline turns
    // complete during the first pump). The CONFIRM_REQUEST handler reads y/N inline
    // during a run_until pump and posts CONFIRM_RESPONSE, then the turn continues.
    bb_->run_until([this] { return turn_done_; }, kTurnTimeoutS);
  }
}

void ChatModule::run_repl_readline() {
  // readline computes the prompt's on-screen width to place the cursor, so the
  // non-printing ANSI bytes must be wrapped in \001..\002 (RL_PROMPT_{START,END}_IGNORE)
  // or line editing miscounts columns on wrap.
  const std::string prompt =
      color_ ? std::string("\001") + kBoldCyan + "\002" + "user> " + "\001" + kReset + "\002"
             : "user> ";
  while (true) {
    char* raw = readline(prompt.c_str());
    if (!raw) {                       // Ctrl-D / EOF
      if (out_) (*out_) << "\n";
      break;
    }
    std::string line(raw);
    if (*raw) add_history(raw);       // non-empty lines recallable via up-arrow
    std::free(raw);
    if (line == "/quit") break;
    if (line.empty()) continue;
    turn_done_ = false;
    bb_->post("USER_MESSAGE", line, "chat");
    // run_until drives the turn (offloaded LLM on a worker, or inline) to its final
    // ASSISTANT_MESSAGE; assistant output prints in cooked mode after readline returns.
    bb_->run_until([this] { return turn_done_; }, kTurnTimeoutS);
  }
}

}  // namespace hades
