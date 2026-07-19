// include/hades/module/tool_runner.h — Module: TOOL_REQUEST -> TOOL_RESULT
//
// ToolRunner subscribes to TOOL_REQUEST on the Blackboard, dispatches to a
// native subprocess or MCP server via ToolRegistry, and posts TOOL_RESULT.
// on_start() warms the registry (one `describe` per native tool); the Arbiter
// reads specs() from the embedded ToolRegistry to build LLM_REQUEST payloads.

#pragma once
#include <chrono>
#include <cstdint>
#include <deque>
#include "hades/entry.h"
#include "hades/module.h"
#include "hades/tool/registry.h"
namespace hades {
class Blackboard;
class Executor;

// Byte-cap a string on a UTF-8 boundary: drop a trailing INCOMPLETE codepoint so the cut
// never yields invalid UTF-8 (the auto_extract trunc_utf8 pattern — count trailing
// continuation bytes, then drop the lead byte too when it announces more bytes than
// remain), then mark the cut. Header-inline so tests exercise the boundary walk directly.
inline std::string trunc_utf8_bytes(std::string s, std::size_t cap) {
  if (s.size() <= cap) return s;
  s.resize(cap);
  std::size_t cont = 0;  // trailing continuation bytes (10xxxxxx), at most 3
  while (cont < 3 && cont < s.size() &&
         (static_cast<unsigned char>(s[s.size() - 1 - cont]) & 0xC0) == 0x80)
    ++cont;
  if (cont < s.size()) {
    const unsigned char lead = static_cast<unsigned char>(s[s.size() - 1 - cont]);
    std::size_t need;                              // bytes the lead byte announces
    if      ((lead & 0x80) == 0x00) need = 1;      // 0xxxxxxx  ASCII
    else if ((lead & 0xE0) == 0xC0) need = 2;      // 110xxxxx
    else if ((lead & 0xF0) == 0xE0) need = 3;      // 1110xxxx
    else if ((lead & 0xF8) == 0xF0) need = 4;      // 11110xxx
    else                            need = 1;      // stray continuation as lead — treat as complete
    if (1 + cont < need) s.resize(s.size() - 1 - cont);  // incomplete tail — drop lead + its conts
  }
  return s + " (truncated)";
}

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

  // Background-task registry — mutated ONLY on the pump thread (the BG_DONE subscriber);
  // workers just post. In-memory, process-lifetime (documented v1: tasks die on restart).
  struct BgRunning { std::string tool; std::chrono::steady_clock::time_point started; };
  struct BgFinished { std::string task_id, tool, output; bool ok = false; };
  void on_bg_done_(const Entry& e);   // running -> finished ring -> post BG_TASKS
  void post_bg_tasks_();              // format + post the BG_TASKS latest-value block
  std::map<std::string, BgRunning> bg_running_;
  std::deque<BgFinished> bg_finished_;   // newest at back; capped at 5
  std::uint64_t bg_counter_ = 0;
  unsigned max_background_ = 4;
  double bg_timeout_s_ = 600.0;
};

}  // namespace hades
