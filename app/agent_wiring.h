#pragma once
#include <memory>
#include <string>
#include <vector>
#include "hades/config.h"
#include "hades/llm/provider.h"
#include "hades/module/llm_module.h"
#include "hades/module/tool_runner.h"
#include "hades/module/chat_module.h"
#include "hades/arbiter.h"
namespace hades {
class Blackboard;

// Agent: owns every module of the live graph for the duration of a session.
// The blackboard's subscriptions are non-owning, so this holder MUST outlive
// the blackboard's pump() activity. Modules are declared in reverse teardown
// order (last-declared is destroyed first); the holder is move-only.
struct Agent {
  std::unique_ptr<LLMModule>  llm;
  std::unique_ptr<ToolRunner> tools;
  std::unique_ptr<Arbiter>    arbiter;
  std::unique_ptr<ChatModule> chat;
};

// Build the full agent graph onto `bb`, injecting `llm` as the provider. Each
// Tool block is added to the ToolRunner, which is started/attached FIRST so its
// registry is warmed (every native `describe` runs once); only then are the
// resulting ToolSpecs handed to the Arbiter so the LLM sees the tool schemas.
// `objectives` maps Objective blocks (stay_on_budget / avoid_destructive) onto
// the Arbiter. Used directly by tests (scripted Provider + injected tool path).
Agent build_agent(Blackboard& bb,
                  std::unique_ptr<Provider> llm,
                  const std::vector<Block>& tools,
                  const std::vector<Block>& objectives,
                  std::string model);

// Convenience overload for the real binary: builds the live OpenAI-compatible
// provider from the manifest's Session block (endpoint/model/api_key_env via
// the named env var) and wires the Tool/Objective blocks it carries. Throws
// MalConfig on a missing Session or unset api key env var.
Agent build_agent(Blackboard& bb, const Manifest& m);

}  // namespace hades
