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
  Blackboard* bb_  = nullptr;
  std::ostream* out_ = nullptr;
  std::istream* in_  = nullptr;
};
}  // namespace hades
