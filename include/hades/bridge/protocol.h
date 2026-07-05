// include/hades/bridge/protocol.h — pure bridge wire-protocol helpers (build/parse/validate)
//
// The agent↔agent bridge protocol (spec 2026-07-03): versioned JSON over HTTP with an
// X-Hades-Bridge shared-secret header. build_* produce the exact request bodies; parse_*
// are tolerant, never throw, and IGNORE unknown fields (additive forward compatibility).
// valid_peer_name is header-only so the standalone hades-ask-agent tool binary shares the
// exact same gate. peer_bus_key renders the fixed pShare-style rename on arrival:
// an inbound share lands as PEER.<from>.<key>, so a peer can never inject a local bus key.
#pragma once
#include <cstddef>
#include <string>
#include <nlohmann/json.hpp>
namespace hades {

inline constexpr int kBridgeProtocolV = 1;
inline constexpr std::size_t kMaxShareBytes = 64 * 1024;   // /share request-body cap

// Strict peer/agent-name gate: 1..64 chars of [A-Za-z0-9_-] (same charset as skill names —
// explicit ASCII ranges, locale-independent). Anything else (path chars, ':', whitespace,
// empty) is rejected: the name is embedded in bus keys and the "peer:<name>" TURN_ORIGIN.
inline bool valid_peer_name(const std::string& n) {
  if (n.empty() || n.size() > 64) return false;
  for (char c : n) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

// One parsed inbound request. ok=false + error covers BOTH transport-level garbage and
// field-level validation failures; callers branch only on ok.
struct BridgeMsg {
  bool ok = false;
  std::string error;      // set when !ok
  std::string from;       // validated peer name
  long long hops = 0;     // /ask only; >= 0
  std::string message;    // /ask only; non-empty
  std::string key;        // /share only; non-empty, no whitespace/control chars
  nlohmann::json value;   // /share only
  std::string type = "raw";   // /share only: "card" | "fact" | "raw" (default; absent -> raw)
};

nlohmann::json build_ask(const std::string& from, long long hops, const std::string& message);
nlohmann::json build_share(const std::string& from, const std::string& key,
                           const nlohmann::json& value, const std::string& type = "raw");
BridgeMsg parse_ask(const std::string& body);
BridgeMsg parse_share(const std::string& body);

// "PEER.<from>.<key>" — the fixed v1 rename-on-arrival for inbound shares.
std::string peer_bus_key(const std::string& from, const std::string& key);

}  // namespace hades
