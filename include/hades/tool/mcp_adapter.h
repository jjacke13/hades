// include/hades/tool/mcp_adapter.h — MCP stdio client for ToolRunner
//
// mcp_call() spawns an MCP server by command, performs the JSON-RPC
// initialize handshake, issues a single tools/call, and returns its result.
// ToolRunner delegates here for ToolEntry entries with kind == "mcp".

#pragma once
#include <string>
#include <nlohmann/json.hpp>
namespace hades {
// Minimal real MCP stdio client. Spawns `command`, performs the JSON-RPC
// handshake (initialize + notifications/initialized) then a single tools/call,
// and returns its `result`. On timeout / no-result / malformed reply, returns
// {"error": "..."} (never throws). MCP tool *discovery* is out of scope (MVP).
nlohmann::json mcp_call(const std::string& command, const std::string& tool,
                        const nlohmann::json& args, double timeout_s = 30.0);
}  // namespace hades
