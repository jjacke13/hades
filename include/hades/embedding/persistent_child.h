// include/hades/embedding/persistent_child.h — a long-lived child process spoken to over stdin/stdout,
// ONE request line -> ONE response line, with a wall-clock timeout. Unlike run_subprocess (one-shot),
// the pipes stay open across calls so a warm model is loaded once. NOT thread-safe — the caller
// serializes (the embedding provider holds a mutex).
#pragma once
#include <string>
#include <vector>
namespace hades {
class PersistentChild {
public:
  PersistentChild(std::vector<std::string> argv, double timeout_s);
  ~PersistentChild();
  PersistentChild(const PersistentChild&) = delete;
  PersistentChild& operator=(const PersistentChild&) = delete;
  // Send one line (a "\n" is appended) and read one reply line. ok=false on spawn failure, write
  // failure, timeout, or EOF (the child is then marked dead; the next call respawns). Never throws.
  struct Reply { bool ok; std::string line; std::string err; };
  Reply request(const std::string& line);
private:
  bool ensure_started_();   // (re)spawn if not alive; false on spawn failure
  void stop_();             // close pipes, SIGKILL + reap if alive
  std::vector<std::string> argv_;
  double timeout_s_;
  int in_fd_ = -1;          // write to child stdin
  int out_fd_ = -1;         // read from child stdout
  int pid_ = -1;
  std::string rbuf_;        // carry-over bytes read past one '\n'
  bool alive_ = false;
};
}  // namespace hades
