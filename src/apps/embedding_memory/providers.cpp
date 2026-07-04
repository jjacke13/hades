// src/apps/embedding_memory/providers.cpp — embedding providers: OpenAI-compat HTTP + warm subprocess
//
// Merged (2026-07-04 src reorg): embedding/http_embedding_provider (/embeddings POST) +
// embedding/subprocess_embedding_provider (one-JSON-line warm child) + embedding/
// persistent_child (the long-lived child-process plumbing).

#include <cerrno>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include "hades/embedding/http_embedding_provider.h"
#include "hades/embedding/persistent_child.h"
#include "hades/embedding/subprocess_embedding_provider.h"

// ── HttpEmbeddingProvider: OpenAI-compatible /embeddings POST (was src/embedding/http_embedding_provider.cpp) ──────────────
//
// embed() serialises {model, input:[texts]} to a JSON body, dispatches it via the injected
// HttpClient, and parses the {"data":[{"embedding":[...]}]} response into EmbedResult.vectors
// (one per input, in order). Every external value is type/bounds-checked before use; ANY failure
// (http throw, non-2xx, unparseable/wrong-shape json, non-number value, count/dim mismatch) sets
// out.error and returns — NEVER throws.
namespace hades {
HttpEmbeddingProvider::HttpEmbeddingProvider(std::string e, std::string k, std::string m, HttpClient h)
  : endpoint_(std::move(e)), key_(std::move(k)), model_(std::move(m)), http_(std::move(h)) {}

EmbedResult HttpEmbeddingProvider::embed(const std::vector<std::string>& texts) {
  EmbedResult out;
  out.model = model_;
  nlohmann::json body{{"model", model_}, {"input", texts}};
  HttpResponse resp;
  try {
    resp = http_(endpoint_ + "/embeddings",
                 {{"Authorization", "Bearer " + key_}, {"Content-Type", "application/json"}},
                 body.dump());
  } catch (...) { out.error = "embedding http call threw"; return out; }
  if (resp.status < 200 || resp.status >= 300) { out.error = "embedding http status " + std::to_string(resp.status); return out; }
  auto j = nlohmann::json::parse(resp.body, nullptr, false);
  if (j.is_discarded() || !j.contains("data") || !j["data"].is_array()) { out.error = "embedding response not parseable"; return out; }
  for (const auto& d : j["data"]) {
    if (!d.is_object() || !d.contains("embedding") || !d["embedding"].is_array()) { return EmbedResult{{}, model_, 0, "embedding item malformed"}; }
    std::vector<float> v;
    for (const auto& x : d["embedding"]) { if (!x.is_number()) { return EmbedResult{{}, model_, 0, "embedding value not number"}; } v.push_back(x.get<float>()); }
    out.vectors.push_back(std::move(v));
  }
  if (out.vectors.size() != texts.size()) { return EmbedResult{{}, model_, 0, "embedding count mismatch"}; }
  out.dim = out.vectors.empty() ? 0 : static_cast<int>(out.vectors.front().size());
  for (const auto& v : out.vectors) if (static_cast<int>(v.size()) != out.dim) { return EmbedResult{{}, model_, 0, "embedding dim inconsistent"}; }
  return out;
}
}  // namespace hades

// ── SubprocessEmbeddingProvider: one-JSON-line warm child (was src/embedding/subprocess_embedding_provider.cpp) ──────────────
namespace hades {
SubprocessEmbeddingProvider::SubprocessEmbeddingProvider(std::vector<std::string> argv, double timeout_s)
  : child_(std::move(argv), timeout_s) {}

std::string SubprocessEmbeddingProvider::model() const {
  std::lock_guard<std::mutex> lk(mu_);          // model_ is written under mu_ by embed() — avoid a torn read
  return model_;
}

EmbedResult SubprocessEmbeddingProvider::embed(const std::vector<std::string>& texts) {
  std::lock_guard<std::mutex> lk(mu_);          // serialize the shared warm child
  EmbedResult out;
  nlohmann::json req{{"texts", texts}};
  // UTF-8-replace dump: query/corpus text is user/LLM-sourced and may contain invalid UTF-8;
  // a plain dump() would THROW (uncaught -> the fail-soft contract broken). Replace instead.
  auto rep = child_.request(req.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
  if (!rep.ok) { out.error = rep.err; return out; }
  // Wrap the WHOLE parse->validate->extract in try/catch: nlohmann's value()/get<>() throw a
  // type_error (302) on a present-but-wrong-typed field (e.g. {"error":{...}}, {"model":5},
  // {"dim":"x"}) — parse(...,false) does NOT catch that (valid JSON, throw at extraction).
  // Any such throw becomes a soft error so embed() upholds its "never throws" contract.
  try {
    auto j = nlohmann::json::parse(rep.line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { out.error = "embedder reply not json object"; return out; }
    if (j.contains("error")) { out.error = "embedder error: " + j.value("error", std::string{"?"}); return out; }
    if (!j.contains("embeddings") || !j["embeddings"].is_array()) { out.error = "embedder reply missing embeddings"; return out; }
    out.model = j.value("model", std::string{});
    out.dim = j.value("dim", 0);
    for (const auto& row : j["embeddings"]) {
      if (!row.is_array()) { return EmbedResult{{}, out.model, 0, "embedder row not array"}; }
      std::vector<float> v;
      for (const auto& x : row) { if (!x.is_number()) { return EmbedResult{{}, out.model, 0, "embedder value not number"}; } v.push_back(x.get<float>()); }
      out.vectors.push_back(std::move(v));
    }
    if (out.vectors.size() != texts.size()) { return EmbedResult{{}, out.model, 0, "embedder count mismatch"}; }
    if (out.dim <= 0 && !out.vectors.empty()) out.dim = static_cast<int>(out.vectors.front().size());
    for (const auto& v : out.vectors) if (static_cast<int>(v.size()) != out.dim) { return EmbedResult{{}, out.model, 0, "embedder dim inconsistent"}; }
    if (!out.model.empty()) model_ = out.model;
    return out;
  } catch (const std::exception& e) {
    return EmbedResult{{}, "", 0, std::string("embedder reply parse error: ") + e.what()};
  }
}
}  // namespace hades

// ── PersistentChild: the long-lived child-process plumbing (was src/embedding/persistent_child.cpp) ──────────────
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
