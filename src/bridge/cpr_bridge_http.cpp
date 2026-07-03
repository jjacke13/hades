// src/bridge/cpr_bridge_http.cpp — thin cpr shell for outbound bridge requests
#include "hades/bridge/http.h"
#include <exception>
#include <cpr/cpr.h>
namespace hades {
std::pair<int, std::string> CprBridgeHttp::post_json(const std::string& url,
                                                     const std::string& body,
                                                     const std::string& secret,
                                                     double timeout_s) {
  try {
    cpr::Response r = cpr::Post(
        cpr::Url{url}, cpr::Body{body},
        cpr::Header{{"Content-Type", "application/json"}, {"X-Hades-Bridge", secret}},
        cpr::Timeout{static_cast<long>(timeout_s * 1000)}, cpr::Redirect{false});
    return {static_cast<int>(r.status_code), r.text};
  } catch (const std::exception&) {
    return {0, ""};   // transport failure — the caller degrades (never throws)
  }
}
}  // namespace hades
