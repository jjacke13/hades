#include "hades/tool/mcp_adapter.h"
#include "hades/tool/subprocess.h"
#include <sstream>
namespace hades {

static std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> v;
  std::istringstream is(s);
  std::string w;
  while (is >> w) v.push_back(w);
  return v;
}

nlohmann::json mcp_call(const std::string& command, const std::string& tool,
                        const nlohmann::json& args, double timeout_s) {
  if (command.empty()) return {{"error", "mcp: empty command"}};

  // Newline-delimited JSON-RPC fed on the server's stdin.
  std::ostringstream in;
  in << nlohmann::json{{"jsonrpc", "2.0"},
                       {"id", 1},
                       {"method", "initialize"},
                       {"params",
                        {{"protocolVersion", "2024-11-05"},
                         {"capabilities", nlohmann::json::object()},
                         {"clientInfo", {{"name", "hades"}, {"version", "0.1.0"}}}}}}
            .dump()
     << "\n";
  in << nlohmann::json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump()
     << "\n";
  in << nlohmann::json{{"jsonrpc", "2.0"},
                       {"id", 2},
                       {"method", "tools/call"},
                       {"params", {{"name", tool}, {"arguments", args}}}}
            .dump()
     << "\n";

  auto r = run_subprocess(split_ws(command), in.str(), timeout_s);
  if (r.timed_out) return {{"error", "mcp server timed out"}};

  // Scan stdout lines for the JSON-RPC reply to our tools/call (id == 2).
  // Every JSON access is guarded so a malformed server line can never throw.
  std::istringstream out(r.out);
  std::string line;
  while (std::getline(out, line)) {
    if (line.empty()) continue;
    auto j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_object() && j.contains("id") && j["id"] == 2 && j.contains("result"))
      return j["result"];
  }
  return {{"error", "mcp: no result for tools/call"}};
}

}  // namespace hades
