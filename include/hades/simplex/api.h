// include/hades/simplex/api.h — SimpleX daemon seam (real impl: WsSimplexApi; tests: scripted fake)
//
// The SimplexModule talks ONLY to this interface (TelegramApi precedent), so its allowlist/
// turn/confirm logic is testable without a daemon. Events are already-parsed SxEvents; commands
// return false on failure (fail-soft — the module logs and carries on). next_event returns
// queued events first (events can arrive while a command round-trip is waiting for its resp).
#pragma once
#include <memory>
#include <string>
#include <vector>
namespace hades {

struct SxEvent {
  enum class Kind { None, Text, ContactRequest, Connected };
  Kind kind = Kind::None;
  long long contact_id = 0;     // Text / Connected
  std::string display_name;     // all kinds (the sender's local display name)
  long long request_id = 0;     // ContactRequest
  std::string text;             // Text
};

enum class SxStatus { Event, Timeout, Closed, Error };

class SimplexApi {
 public:
  virtual ~SimplexApi() = default;
  virtual SxStatus next_event(double timeout_s, SxEvent& out) = 0;
  virtual bool send_text(long long contact_id, const std::string& text) = 0;
  virtual bool accept_request(long long request_id) = 0;
  virtual bool reconnect() = 0;   // (re)establish the connection; module backs off between tries
};

// Pure, tolerant event parse for one daemon frame: {"resp":{...}} (an optional {"Right":...}
// Either-wrapper is unwrapped). Yields Text (direct+directRcv+rcvMsgContent+text only),
// ContactRequest, Connected; everything else -> {}. Never throws.
std::vector<SxEvent> parse_simplex_events(const std::string& frame_json);

// The real seam impl (WsSimplexApi over WsClient) is file-local in simplex.cpp; this factory
// is its only exposure (the module self-builds it in on_start; tests script a FakeApi instead).
std::unique_ptr<SimplexApi> make_ws_simplex_api(std::string host, int port,
                                                double connect_timeout_s);

}  // namespace hades
