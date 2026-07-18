// tools/http_fetch_main.cpp — bundled http_fetch native tool binary
//
// Reads one JSON line ({"call":"describe"|"http_fetch","args":{url,raw?}}), does an HTTP
// GET via cpr, and returns one JSON line. HTML responses (Content-Type, or body sniff when
// the header is absent) are converted to readable text by default — title first, links
// inline as "label (url)" — pass raw=true for the untouched body. Extraction runs on the
// FULL body, THEN the 64 KB cap applies to the output; non-HTML bodies pass through capped
// as before. Spawned as a subprocess by ToolRunner; read-only web egress; guarded so a
// malformed request never throws. Reply uses the UTF-8-replace dump: the page's bytes are
// untrusted and a cap cut can land mid-codepoint.
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>
#include <cpr/cpr.h>
#include "hades/tool/html_text.h"

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
             {"description",
              "HTTP GET a URL. HTML pages are converted to readable text with links shown "
              "as 'label (url)'; pass raw=true for the unconverted body. Response truncated "
              "to 64 KB."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"url", {{"type", "string"}}},
                 {"raw",
                  {{"type", "boolean"},
                   {"description", "return the unconverted response body"}}}}},
               {"required", {"url"}}}}}}};
  } else if (call == "http_fetch") {
    nlohmann::json args =
        (in.is_object() && in.contains("args") && in["args"].is_object())
            ? in["args"]
            : nlohmann::json::object();
    std::string url = args.value("url", "");
    // Non-boolean raw counts as absent (LLM-malformed args fail soft, never crash).
    const bool raw = args.contains("raw") && args["raw"].is_boolean() && args["raw"].get<bool>();
    if (url.empty()) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: url"}}}};
    } else {
      // Redirects are DISABLED so the CapabilityPolicy host-gate (which classifies only the
      // initial URL's host) cannot be bypassed by a 3xx Location to a private/loopback host
      // (redirect-SSRF). A 3xx now returns the redirect response unfollowed.
      cpr::Response r = cpr::Get(cpr::Url{url}, cpr::Timeout{30000}, cpr::Redirect{false});
      std::string content_type;
      if (auto it = r.header.find("Content-Type"); it != r.header.end())
        content_type = it->second;   // cpr header map is case-insensitive
      std::string body = r.text;
      bool extracted = false;
      if (!raw && hades::looks_like_html(content_type, body)) {
        body = hades::extract_html_text(body);   // full body first; cap applies to output
        extracted = true;
      }
      constexpr std::size_t kCap = 64 * 1024;
      bool truncated = body.size() > kCap;
      if (truncated) body.resize(kCap);
      out = {{"ok", r.status_code > 0},
             {"result",
              {{"status", static_cast<int>(r.status_code)},
               {"body", body},
               {"truncated", truncated},
               {"extracted", extracted}}}};
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }

  std::cout << out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)
            << std::endl;
  return 0;
}
