#include "hades/embedding/persistent_child.h"
#include <cerrno>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
namespace hades {
namespace { double mono() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; } }

PersistentChild::PersistentChild(std::vector<std::string> argv, double timeout_s)
  : argv_(std::move(argv)), timeout_s_(timeout_s) {}
PersistentChild::~PersistentChild() { stop_(); }

bool PersistentChild::ensure_started_() {
  ::signal(SIGPIPE, SIG_IGN);                // writing to a dead warm child must not kill us (EPIPE, not SIGPIPE)
  if (alive_) return true;
  int ip[2], op[2];
  if (pipe(ip) != 0) return false;
  if (pipe(op) != 0) { close(ip[0]); close(ip[1]); return false; }
  pid_t pid = fork();
  if (pid < 0) { close(ip[0]); close(ip[1]); close(op[0]); close(op[1]); return false; }
  if (pid == 0) {                          // child
    dup2(ip[0], 0); dup2(op[1], 1);
    for (int fd : {ip[0], ip[1], op[0], op[1]}) close(fd);
    std::vector<char*> a;
    for (auto& s : argv_) a.push_back(const_cast<char*>(s.c_str()));
    a.push_back(nullptr);
    execvp(a[0], a.data());
    _exit(127);                            // exec failed
  }
  close(ip[0]); close(op[1]);
  in_fd_ = ip[1]; out_fd_ = op[0]; pid_ = pid; alive_ = true; rbuf_.clear();
  fcntl(in_fd_, F_SETFL, O_NONBLOCK);      // timeout-bounded writes (poll(POLLOUT)) — a stuck child can't deadlock past timeout_s_
  return true;
}

void PersistentChild::stop_() {
  if (in_fd_ >= 0) { close(in_fd_); in_fd_ = -1; }
  if (out_fd_ >= 0) { close(out_fd_); out_fd_ = -1; }
  if (pid_ > 0) {
    kill(pid_, SIGKILL);
    int st = 0, rc;
    do { rc = waitpid(pid_, &st, 0); } while (rc < 0 && errno == EINTR);  // EINTR-guarded reap (no zombie window)
    pid_ = -1;
  }
  alive_ = false;
}

PersistentChild::Reply PersistentChild::request(const std::string& line) {
  if (!ensure_started_()) return {false, "", "embedder spawn failed"};
  const double deadline = mono() + timeout_s_;   // ONE wall-clock budget shared across write + read
  // write request + newline, timeout-bounded via poll(POLLOUT) (in_fd_ is non-blocking)
  std::string msg = line; msg.push_back('\n');
  std::size_t off = 0;
  while (off < msg.size()) {
    double ms = (deadline - mono()) * 1000.0;
    if (ms <= 0) { stop_(); return {false, "", "embedder write timed out"}; }
    pollfd pfd{in_fd_, POLLOUT, 0};
    int n = poll(&pfd, 1, static_cast<int>(ms));
    if (n < 0) { if (errno == EINTR) continue; stop_(); return {false, "", "embedder poll error"}; }
    if (n == 0) { stop_(); return {false, "", "embedder write timed out"}; }
    ssize_t w = write(in_fd_, msg.data() + off, msg.size() - off);
    if (w > 0) { off += static_cast<std::size_t>(w); }
    else if (w < 0 && (errno == EINTR || errno == EAGAIN)) { continue; }   // not ready / interrupted -> poll again
    else { stop_(); return {false, "", "embedder write failed"}; }         // EPIPE etc.: child gone
  }
  // read until we have one full line (or timeout / EOF)
  for (;;) {
    if (auto nl = rbuf_.find('\n'); nl != std::string::npos) {
      std::string out = rbuf_.substr(0, nl);
      rbuf_.erase(0, nl + 1);
      return {true, out, ""};
    }
    double ms = (deadline - mono()) * 1000.0;
    if (ms <= 0) { stop_(); return {false, "", "embedder timed out"}; }
    pollfd pfd{out_fd_, POLLIN, 0};
    int n = poll(&pfd, 1, static_cast<int>(ms));
    if (n < 0) { if (errno == EINTR) continue; stop_(); return {false, "", "embedder poll error"}; }
    if (n == 0) { stop_(); return {false, "", "embedder timed out"}; }
    char buf[4096];
    ssize_t k = read(out_fd_, buf, sizeof buf);
    if (k <= 0) { stop_(); return {false, "", "embedder closed (EOF)"}; }  // child died
    rbuf_.append(buf, static_cast<std::size_t>(k));
  }
}
}  // namespace hades
