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

void ChatModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  bb.subscribe("ASSISTANT_MESSAGE", [this](const Entry& e) {
    if (out_ && e.value.is_string()) (*out_) << "assistant> " << e.value.get<std::string>() << "\n";
  });
  bb.subscribe("CONFIRM_REQUEST", [this](const Entry& e) {
    if (!in_) return;  // no interactive stream -> cannot answer; skip
    if (out_) (*out_) << "confirm> " << e.value.value("prompt", "") << " [y/N]: ";
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
  struct Guard { ChatModule* self; ~Guard(){ self->in_=nullptr; self->out_=nullptr; } } guard{this};

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
    bb_->post("USER_MESSAGE", line, "chat");
    bb_->pump();
  }
}

void ChatModule::run_repl_readline() {
  while (true) {
    char* raw = readline("user> ");
    if (!raw) {                       // Ctrl-D / EOF
      if (out_) (*out_) << "\n";
      break;
    }
    std::string line(raw);
    if (*raw) add_history(raw);       // non-empty lines recallable via up-arrow
    std::free(raw);
    if (line == "/quit") break;
    if (line.empty()) continue;
    bb_->post("USER_MESSAGE", line, "chat");
    bb_->pump();                       // assistant output prints in cooked mode, after readline returns
  }
}

}  // namespace hades
