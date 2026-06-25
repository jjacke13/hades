#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>
namespace hades {
struct HttpResponse { long status; std::string body; };
using HttpClient = std::function<HttpResponse(
    const std::string& url,
    const std::vector<std::pair<std::string,std::string>>& headers,
    const std::string& body)>;
HttpClient cpr_http(double timeout_s = 120.0);   // default impl uses cpr::Post
}  // namespace hades
