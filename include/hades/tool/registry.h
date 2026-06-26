#pragma once
#include <map>
#include <string>
#include <vector>
#include "hades/config.h"
#include "hades/llm/provider.h"  // ToolSpec
namespace hades {

struct ToolEntry {
  std::string name;     // the Tool block name (config-side handle)
  std::string kind;     // "native" | "mcp"
  std::string command;  // argv string (split on whitespace at spawn time)
};

// Registry of configured tools. The describe/spec cache for native tools is
// built EXACTLY ONCE (see warm() / lazy ensure_warm) so we never re-spawn a
// tool's `describe` subprocess on every TOOL_REQUEST.
class ToolRegistry {
public:
  void add_from_block(const Block& tool);  // Tool = name { native=.. | mcp=.. }
  const std::vector<ToolEntry>& entries() const;

  // Lookup by the config-side block name.
  const ToolEntry* find(const std::string& name) const;

  // Lookup by the tool's SELF-REPORTED name (from `describe`), falling back to
  // the block name. This is what TOOL_REQUEST.tool is routed against.
  const ToolEntry* find_by_tool_name(const std::string& reported_name) const;

  // Run each native tool's `describe` ONCE and build the spec + name caches.
  // Idempotent: subsequent calls are no-ops.
  void warm(double timeout_s = 10.0);

  // Cached ToolSpecs for the arbiter. Triggers warm() once if not yet warmed.
  std::vector<ToolSpec> specs(double timeout_s = 10.0) const;

private:
  // Builds the mutable caches if not already warmed. const so specs()/find can
  // lazily trigger it; mutates only the mutable cache members below.
  void ensure_warm(double timeout_s) const;

  std::vector<ToolEntry> tools_;

  mutable bool warmed_ = false;
  mutable std::vector<ToolSpec> specs_;
  // reported-name (or block name) -> entry. Pointers reference tools_ elements;
  // valid because all tools are added before warming and no entry is added
  // afterward (warmed_ guards rebuild).
  mutable std::map<std::string, const ToolEntry*> by_tool_name_;
};

}  // namespace hades
