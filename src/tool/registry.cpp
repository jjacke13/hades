// src/tool/registry.cpp — ToolRegistry: manifest-loaded tool table + lazy warm
//
// Implements add_from_block() (native / mcp entries from the Manifest), find(),
// find_by_tool_name(), and ensure_warm(): on first warm, each native tool is probed
// via run_subprocess with {"call":"describe"} to collect ToolSpec schemas that
// build_agent() hands to the Arbiter so the LLM sees tool definitions. MCP tool
// discovery is deferred (MVP); entries route by block name.

#include "hades/tool/registry.h"
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

void ToolRegistry::add_from_block(const Block& b) {
  ToolEntry e;
  e.name = b.name;
  if (b.kv.count("native")) { e.kind = "native"; e.command = b.kv.at("native"); }
  else if (b.kv.count("mcp")) { e.kind = "mcp"; e.command = b.kv.at("mcp"); }
  else return;                         // unchanged behavior: a block with neither is ignored
  if (b.kv.count("timeout_s"))
    set_pos_double_on_string(b.kv.at("timeout_s"), e.timeout_s);
  tools_.push_back(std::move(e));
}

const std::vector<ToolEntry>& ToolRegistry::entries() const { return tools_; }

const ToolEntry* ToolRegistry::find(const std::string& n) const {
  for (const auto& t : tools_)
    if (t.name == n) return &t;
  return nullptr;
}

void ToolRegistry::ensure_warm(double timeout_s) const {
  if (warmed_) return;
  warmed_ = true;
  specs_.clear();
  by_tool_name_.clear();
  for (const auto& t : tools_) {
    if (t.kind != "native") {
      // MCP tool discovery is deferred (MVP). Route by the block name so a
      // TOOL_REQUEST naming the block still reaches this entry.
      by_tool_name_.emplace(t.name, &t);
      continue;
    }
    auto r = run_subprocess(split_ws(t.command), R"({"call":"describe"})", timeout_s);
    auto j = nlohmann::json::parse(r.out, nullptr, false);  // guarded: no throw
    std::string reported = t.name;  // fall back to block name
    if (!r.timed_out && j.is_object() && j.value("ok", false) &&
        j.contains("result") && j["result"].is_object()) {
      const auto& res = j["result"];
      reported = res.value("name", t.name);
      specs_.push_back({reported, res.value("description", ""),
                        res.value("schema", nlohmann::json::object())});
    }
    by_tool_name_.emplace(reported, &t);
  }
}

void ToolRegistry::warm(double timeout_s) { ensure_warm(timeout_s); }

std::vector<ToolSpec> ToolRegistry::specs(double timeout_s) const {
  ensure_warm(timeout_s);
  return specs_;
}

const ToolEntry* ToolRegistry::find_by_tool_name(const std::string& n) const {
  ensure_warm(10.0);  // no-op if already warmed (e.g. via ToolRunner::on_start)
  auto it = by_tool_name_.find(n);
  if (it != by_tool_name_.end()) return it->second;
  return find(n);  // fall back to block name
}

}  // namespace hades
