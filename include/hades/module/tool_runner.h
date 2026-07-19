// include/hades/module/tool_runner.h — Module: TOOL_REQUEST -> TOOL_RESULT
//
// ToolRunner subscribes to TOOL_REQUEST on the Blackboard, dispatches to a
// native subprocess or MCP server via ToolRegistry, and posts TOOL_RESULT.
// on_start() warms the registry (one `describe` per native tool); the Arbiter
// reads specs() from the embedded ToolRegistry to build LLM_REQUEST payloads.

#pragma once
#include "hades/module.h"
#include "hades/tool/registry.h"
namespace hades {
class Blackboard;
class Executor;

// ToolRunner: consumes TOOL_REQUEST {id, tool, args}, runs the named tool
// (native subprocess or MCP stdio), and posts TOOL_RESULT {id, ok, content}.
// Tool blocks are loaded via add_tool() before on_start(); on_start() warms the
// registry (each native `describe` runs exactly once, not per request).
class ToolRunner : public Module {
public:
  std::string type() const override { return "tool_runner"; }
  void on_start(const Block& cfg, Blackboard& bb) override;
  void on_attach(Blackboard& bb) override;
  ToolRegistry& registry() { return reg_; }  // arbiter pulls specs() from here
  void add_tool(const Block& b) { reg_.add_from_block(b); }

  // Opt-in offload (the LLMModule::set_executor pattern): when set, tool execution runs on
  // a worker and posts TOOL_RESULT back; null (default) -> inline on the pump thread, so
  // the test path is byte-identical. Non-owning: the Executor is joined before modules die.
  void set_executor(Executor* ex) { executor_ = ex; }

private:
  ToolRegistry reg_;
  Blackboard* bb_ = nullptr;
  double timeout_s_ = 30.0;
  Executor* executor_ = nullptr;  // non-owning; see set_executor
};

}  // namespace hades
