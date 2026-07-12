// include/hades/tool/mcp_adapter.h — MCP client (stdio + Streamable HTTP) for ToolRunner
//
// Transport-agnostic MCP exchange over a configured server entry: kind "mcp" = one-shot stdio
// (spawn command, newline-delimited JSON-RPC), kind "mcp_http" = Streamable HTTP (cpr POSTs,
// optional Bearer from entry.api_key_env, Mcp-Session-Id lifecycle, SSE-or-JSON responses).
// mcp_list -> the server's tools/list result ({"tools":[...]}); mcp_call -> one tools/call
// result. Both never throw; timeout / transport / malformed-reply -> {"error": "..."}.

#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include "hades/tool/registry.h"   // ToolEntry (the transport descriptor)
namespace hades {
nlohmann::json mcp_list(const ToolEntry& server, double timeout_s = 30.0);
nlohmann::json mcp_call(const ToolEntry& server, const std::string& tool,
                        const nlohmann::json& args, double timeout_s = 30.0);
}  // namespace hades
