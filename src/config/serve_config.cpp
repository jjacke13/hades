// src/config/serve_config.cpp — Serve block -> ServeConfig with safe defaults
#include "hades/serve_config.h"
#include <string>
namespace hades {
namespace {
int parse_port(const std::string& s) {
  try {
    std::size_t i = 0;
    long v = std::stol(s, &i);
    if (i == s.size() && v > 0 && v < 65536) return static_cast<int>(v);
  } catch (...) { /* fall through */ }
  return 0;  // invalid -> caller keeps its default
}
}  // namespace

ServeConfig resolve_serve_config(const Manifest& m, int cli_port) {
  ServeConfig c{"127.0.0.1", 8080, "web"};
  auto blocks = m.of("Serve");
  if (!blocks.empty()) {
    const auto& kv = blocks.front().kv;
    if (auto it = kv.find("host");    it != kv.end() && !it->second.empty()) c.host = it->second;
    if (auto it = kv.find("webroot"); it != kv.end() && !it->second.empty()) c.webroot = it->second;
    if (auto it = kv.find("port");    it != kv.end()) {
      int p = parse_port(it->second);
      if (p > 0) c.port = p;
    }
  }
  if (cli_port > 0 && cli_port < 65536) c.port = cli_port;  // ignore out-of-range CLI port
  return c;
}
}  // namespace hades
