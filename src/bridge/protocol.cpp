// src/bridge/protocol.cpp — bridge protocol build/parse (tolerant, never throws)
#include "hades/bridge/protocol.h"
namespace hades {
namespace {

// Common envelope checks: valid JSON object, v == kBridgeProtocolV, from is a valid peer
// name. Returns true and fills m.from on success; sets m.error otherwise.
bool parse_envelope(const nlohmann::json& j, BridgeMsg& m) {
  if (!j.is_object()) { m.error = "not a JSON object"; return false; }
  auto v = j.find("v");
  if (v == j.end() || !v->is_number_integer() ||
      v->get<long long>() != kBridgeProtocolV) {
    m.error = "unsupported protocol version";
    return false;
  }
  auto from = j.find("from");
  if (from == j.end() || !from->is_string() || !valid_peer_name(from->get<std::string>())) {
    m.error = "missing/invalid from";
    return false;
  }
  m.from = from->get<std::string>();
  return true;
}

bool valid_share_key(const std::string& k) {
  if (k.empty() || k.size() > 128) return false;
  for (char c : k)
    if (static_cast<unsigned char>(c) <= ' ' || static_cast<unsigned char>(c) >= 127)
      return false;   // no whitespace/control/non-ASCII in a bus key
  return true;
}

}  // namespace

nlohmann::json build_ask(const std::string& from, long long hops, const std::string& message) {
  return {{"v", kBridgeProtocolV}, {"from", from}, {"hops", hops}, {"message", message}};
}

nlohmann::json build_share(const std::string& from, const std::string& key,
                           const nlohmann::json& value) {
  return {{"v", kBridgeProtocolV}, {"from", from}, {"key", key}, {"value", value}};
}

BridgeMsg parse_ask(const std::string& body) {
  BridgeMsg m;
  auto j = nlohmann::json::parse(body, nullptr, false);
  if (j.is_discarded()) { m.error = "malformed JSON"; return m; }
  if (!parse_envelope(j, m)) return m;
  auto hops = j.find("hops");
  if (hops == j.end() || !hops->is_number_integer() || hops->get<long long>() < 0) {
    m.error = "missing/invalid hops";
    return m;
  }
  m.hops = hops->get<long long>();
  auto msg = j.find("message");
  if (msg == j.end() || !msg->is_string() || msg->get<std::string>().empty()) {
    m.error = "missing/invalid message";
    return m;
  }
  m.message = msg->get<std::string>();
  m.ok = true;
  return m;
}

BridgeMsg parse_share(const std::string& body) {
  BridgeMsg m;
  auto j = nlohmann::json::parse(body, nullptr, false);
  if (j.is_discarded()) { m.error = "malformed JSON"; return m; }
  if (!parse_envelope(j, m)) return m;
  auto key = j.find("key");
  if (key == j.end() || !key->is_string() || !valid_share_key(key->get<std::string>())) {
    m.error = "missing/invalid key";
    return m;
  }
  m.key = key->get<std::string>();
  auto val = j.find("value");
  if (val == j.end()) { m.error = "missing value"; return m; }
  m.value = *val;
  m.ok = true;
  return m;
}

std::string peer_bus_key(const std::string& from, const std::string& key) {
  return "PEER." + from + "." + key;
}
}  // namespace hades
