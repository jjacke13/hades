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
private:
  // Interactive REPL backed by GNU readline (line editing: arrows, history,
  // Ctrl-A/E, reverse i-search). Used only when stdin is a real TTY; otherwise
  // run_repl falls back to std::getline so piped/test input still works.
  void run_repl_readline();

  Blackboard* bb_  = nullptr;
  std::ostream* out_ = nullptr;
  std::istream* in_  = nullptr;
  bool color_ = false;   // ANSI styling on; set in run_repl when stdout is a real TTY
};
}  // namespace hades
