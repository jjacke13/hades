#include "hades/module/chat_module.h"
#include "hades/blackboard.h"
#include <istream>
#include <ostream>
#include <string>
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

}  // namespace hades
