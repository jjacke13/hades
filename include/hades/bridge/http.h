// include/hades/bridge/http.h — outbound bridge HTTP seam (real impl: cpr; tests: fake)
//
// The BridgeModule's share push talks ONLY to this interface, so the push logic is testable
// without a network (TelegramApi precedent). post_json returns {status, body}; status 0 means
// a transport-level failure (connect refused / timeout). Never throws.
#pragma once
#include <string>
#include <utility>
namespace hades {
struct BridgeHttp {
  virtual ~BridgeHttp() = default;
  virtual std::pair<int, std::string> post_json(const std::string& url, const std::string& body,
                                                const std::string& secret, double timeout_s) = 0;
  // GET seam for discovery (pull a peer's /card). Same contract: status 0 = transport failure,
  // never throws.
  virtual std::pair<int, std::string> get_json(const std::string& url, const std::string& secret,
                                               double timeout_s) = 0;
};

class CprBridgeHttp : public BridgeHttp {
 public:
  std::pair<int, std::string> post_json(const std::string& url, const std::string& body,
                                        const std::string& secret, double timeout_s) override;
  std::pair<int, std::string> get_json(const std::string& url, const std::string& secret,
                                       double timeout_s) override;
};
}  // namespace hades
