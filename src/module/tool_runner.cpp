// src/module/tool_runner.cpp — TOOL_REQUEST executor via subprocess/MCP
//
// Subscribes to TOOL_REQUEST on the Blackboard; dispatches native tools as
// isolated subprocesses (run_subprocess, one-JSON-line protocol) and MCP
// servers via mcp_adapter. Posts TOOL_RESULT with ok/content back to the
// Blackboard; ToolRegistry is warmed once in on_start() to cache tool specs.

#include "hades/module/tool_runner.h"
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/tool/subprocess.h"
#include "hades/tool/mcp_adapter.h"
#include <sstream>
namespace hades {

static std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> v;
  std::istringstream is(s);
  std::string w;
  while (is >> w) v.push_back(w);
  return v;
}

void ToolRunner::on_start(const Block& cfg, Blackboard&) {
  if (cfg.kv.count("timeout"))
    set_pos_double_on_string(cfg.kv.at("timeout"), timeout_s_);
  // Build the describe/spec cache ONCE here, so we never re-spawn a tool's
  // `describe` subprocess on each TOOL_REQUEST.
  reg_.warm(timeout_s_);
}

void ToolRunner::on_attach(Blackboard& bb) {
  bb_ = &bb;
  // SAFETY: `this`/bb_ are non-owning. ToolRunner lifetime is managed by the
  // Launcher and outlives this subscription (modules are cleared before the
  // Blackboard is destroyed).
  bb.subscribe("TOOL_REQUEST", [this](const Entry& e) {
    // Guard all external (blackboard) JSON access.
    std::string id, name;
    nlohmann::json args = nlohmann::json::object();
    if (e.value.is_object()) {
      id = e.value.value("id", "");
      name = e.value.value("tool", "");
      if (e.value.contains("args") && e.value["args"].is_object())
        args = e.value["args"];
    }

    const ToolEntry* te = reg_.find_by_tool_name(name);
    nlohmann::json content;
    bool ok = false;

    // Per-tool override (Tool block timeout_s, e.g. ask_agent's long peer-call window);
    // 0 -> the runner-wide default.
    const double timeout = (te && te->timeout_s > 0.0) ? te->timeout_s : timeout_s_;

    if (!te) {
      content = {{"error", "unknown tool: " + name}};
    } else if (te->kind == "native") {
      nlohmann::json call{{"call", name}, {"args", args}};
      auto r = run_subprocess(split_ws(te->command), call.dump(), timeout);
      if (r.timed_out) {
        content = {{"error", "tool timed out: " + name}};
      } else {
        auto j = nlohmann::json::parse(r.out, nullptr, false);  // guarded
        if (!j.is_object()) {
          content = {{"error", "bad tool output"}};
        } else {
          ok = j.value("ok", false);
          content = j.contains("result") ? j["result"] : nlohmann::json::object();
        }
      }
    } else {  // mcp
      content = mcp_call(te->command, name, args, timeout);
      ok = content.is_object() && !content.contains("error");
    }

    bb_->post("TOOL_RESULT", {{"id", id}, {"ok", ok}, {"content", content}},
              "tool_runner", id);
  });
}

}  // namespace hades
