#include "hades/llm/http.h"
#include <cpr/cpr.h>
namespace hades {
HttpClient cpr_http(double timeout_s){
  return [timeout_s](const std::string& url,
                     const std::vector<std::pair<std::string,std::string>>& headers,
                     const std::string& body)->HttpResponse {
    cpr::Header h; for(auto& kv: headers) h[kv.first]=kv.second;
    auto r=cpr::Post(cpr::Url{url}, h, cpr::Body{body},
                     cpr::Timeout{static_cast<int32_t>(timeout_s*1000)});
    return {r.status_code, r.text};
  };
}
}  // namespace hades
