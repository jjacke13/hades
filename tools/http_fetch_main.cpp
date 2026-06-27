// tools/http_fetch_main.cpp — bundled http_fetch native tool binary
//
// Reads one JSON line ({"call":"describe"|"http_fetch","args":{url}}), does an
// HTTP GET via cpr, and returns status + body (truncated to 64 KB) as one JSON
// line. Spawned as a subprocess by ToolRunner; the hades one-JSON-line native
// tool protocol. Read-only web egress; guarded so a malformed request never throws.
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>
#include <cpr/cpr.h>

int main() {
  std::string line;
  std::getline(std::cin, line);
  auto in = nlohmann::json::parse(line, nullptr, false);

  nlohmann::json out;
  std::string call = in.is_object() ? in.value("call", "") : "";

  if (call == "describe") {
    out = {{"ok", true},
           {"result",
            {{"name", "http_fetch"},
             {"description", "HTTP GET a URL and return the status and response body (truncated to 64 KB)."},
             {"schema",
              {{"type", "object"},
               {"properties", {{"url", {{"type", "string"}}}}},
               {"required", {"url"}}}}}}};
  } else if (call == "http_fetch") {
    nlohmann::json args =
        (in.is_object() && in.contains("args") && in["args"].is_object())
            ? in["args"]
            : nlohmann::json::object();
    std::string url = args.value("url", "");
    if (url.empty()) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: url"}}}};
    } else {
      cpr::Response r = cpr::Get(cpr::Url{url}, cpr::Timeout{30000});
      std::string body = r.text;
      constexpr std::size_t kCap = 64 * 1024;
      bool truncated = body.size() > kCap;
      if (truncated) body.resize(kCap);
      out = {{"ok", r.status_code > 0},
             {"result",
              {{"status", static_cast<int>(r.status_code)},
               {"body", body},
               {"truncated", truncated}}}};
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }

  std::cout << out.dump() << std::endl;
  return 0;
}
