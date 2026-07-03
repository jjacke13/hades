// tools/ask_agent_main.cpp — bundled ask_agent native tool binary (outbound delegation)
//
// Reads one JSON line ({"call":"describe"|"ask_agent","args":{peer,message}}) and writes one
// JSON line. POSTs a v1 bridge /ask to the named peer and returns the peer's reply as the
// tool result — the receiving agent runs the request through its OWN Arbiter/objectives.
// Argv (appended by wiring; single source of truth): <own_name> <secret_env> <timeout_s>
// <name=url>... The shared secret is resolved from the NAMED env var here (never argv/
// manifest). hops is always 0 in v1 (multi-hop is a v2 wire seam); the PeerLoopGuard on the
// asking side is what actually prevents relay loops. Fail-closed: malformed input, unknown
// peer, missing secret, transport failure -> ok:false, never throws.
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <nlohmann/json.hpp>
#include <cpr/cpr.h>
#include "hades/bridge/protocol.h"

int main(int argc, char** argv) {
  const std::string own_name = argc > 1 ? argv[1] : "hades";
  const std::string secret_env = argc > 2 ? argv[2] : "HADES_BRIDGE_SECRET";
  double timeout_s = 180.0;
  if (argc > 3) {
    try { timeout_s = std::stod(argv[3]); } catch (const std::exception&) {}
    if (timeout_s <= 0) timeout_s = 180.0;
  }
  std::map<std::string, std::string> peers;   // name -> base url
  for (int i = 4; i < argc; ++i) {
    const std::string pair = argv[i];
    const auto eq = pair.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 >= pair.size()) continue;
    peers[pair.substr(0, eq)] = pair.substr(eq + 1);
  }

  std::string line;
  std::getline(std::cin, line);
  auto in = nlohmann::json::parse(line, nullptr, false);

  nlohmann::json out;
  std::string call;
  if (in.is_object() && in.contains("call") && in["call"].is_string())
    call = in["call"].get<std::string>();

  if (call == "describe") {
    std::string roster;
    for (const auto& [name, url] : peers) roster += (roster.empty() ? "" : ", ") + name;
    out = {{"ok", true},
           {"result",
            {{"name", "ask_agent"},
             {"description",
              "Ask a peer hades agent a question and get its answer. The peer runs your "
              "request through its own tools and safety gates. Known peers: " +
                  (roster.empty() ? std::string("(none configured)") : roster) +
                  ". peer: the peer's name; message: what you want it to do or answer."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"peer", {{"type", "string"}}}, {"message", {{"type", "string"}}}}},
               {"required", {"peer", "message"}}}}}}};
  } else if (call == "ask_agent") {
    nlohmann::json args = (in.is_object() && in.contains("args") && in["args"].is_object())
                              ? in["args"]
                              : nlohmann::json::object();
    auto str = [&](const char* k) {
      return args.contains(k) && args[k].is_string() ? args[k].get<std::string>()
                                                     : std::string{};
    };
    const std::string peer = str("peer");
    const std::string message = str("message");
    const char* secret = std::getenv(secret_env.c_str());
    if (peer.empty() || message.empty()) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: peer and message required"}}}};
    } else if (!peers.count(peer)) {
      out = {{"ok", false}, {"result", {{"error", "unknown peer: " + peer}}}};
    } else if (!secret || !*secret) {
      out = {{"ok", false},
             {"result", {{"error", "bridge secret env var not set: " + secret_env}}}};
    } else {
      const std::string body = hades::build_ask(own_name, 0, message).dump();
      cpr::Response r = cpr::Post(
          cpr::Url{peers.at(peer) + "/ask"}, cpr::Body{body},
          cpr::Header{{"Content-Type", "application/json"}, {"X-Hades-Bridge", secret}},
          cpr::Timeout{static_cast<long>(timeout_s * 1000)}, cpr::Redirect{false});
      auto resp = nlohmann::json::parse(r.text, nullptr, false);
      if (r.status_code == 200 && resp.is_object() && resp.value("ok", false)) {
        out = {{"ok", true},
               {"result", {{"peer", peer}, {"reply", resp.value("reply", "")}}}};
      } else if (r.status_code == 0) {
        out = {{"ok", false},
               {"result", {{"error", "peer unreachable: " + peer + " (connect/timeout)"}}}};
      } else {
        const std::string why =
            resp.is_object() ? resp.value("error", "") : std::string("bad response");
        out = {{"ok", false},
               {"result",
                {{"error", "peer " + peer + " refused (" + std::to_string(r.status_code) +
                               "): " + why}}}};
      }
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }
  std::cout << out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << std::endl;
  return 0;
}
