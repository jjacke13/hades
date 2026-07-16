// src/apps/auto_extract/extract.cpp — pure auto-extract helpers (see the header)
#include "hades/extract/extract.h"
#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
namespace hades {
namespace {
constexpr std::size_t kDigestSideCap = 2000;   // bytes per digest side
constexpr std::size_t kFactCap       = 500;    // bytes per saved fact

std::string trim(std::string s) {
  auto ns = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), ns));
  s.erase(std::find_if(s.rbegin(), s.rend(), ns).base(), s.end());
  return s;
}
}  // namespace

std::vector<std::string> parse_extract_reply(const std::string& reply, std::size_t max_facts) {
  std::vector<std::string> out;
  std::string r = trim(reply);
  // Strip a ``` / ```json fence if the whole reply is one fenced block.
  if (r.rfind("```", 0) == 0) {
    const std::size_t nl = r.find('\n');
    const std::size_t close = r.rfind("```");
    if (nl != std::string::npos && close != std::string::npos && close > nl)
      r = trim(r.substr(nl + 1, close - nl - 1));
  }
  if (r.empty()) return out;
  {  // case-insensitive NONE
    std::string low = r;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (low == "none") return out;
  }
  const auto j = nlohmann::json::parse(r, nullptr, false);
  if (!j.is_array()) return out;                 // fail-closed: array or nothing
  for (const auto& item : j) {
    if (out.size() >= max_facts) break;
    if (!item.is_string()) continue;
    std::string t = item.get<std::string>();
    for (char& c : t)
      if (c == '\n' || c == '\r') c = ' ';       // one fact = one store line
    t = trim(t);
    if (t.empty()) continue;
    if (t.size() > kFactCap) t.resize(kFactCap);
    out.push_back(std::move(t));
  }
  return out;
}

std::string build_extract_digest(const std::string& user, const std::string& assistant) {
  return "U: " + user.substr(0, kDigestSideCap) + "\nA: " + assistant.substr(0, kDigestSideCap);
}

bool is_turn_artifact(const std::string& a) {
  for (const char* p : {"[blocked", "[declined", "[stopped", "[timed out", "[new session]"})
    if (a.rfind(p, 0) == 0) return true;
  return false;
}
}  // namespace hades
