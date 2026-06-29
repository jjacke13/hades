// app/agent_wiring.h — build_agent: assemble the full hades module graph
//
// Declares the Agent holder struct (unique_ptrs to LLMModule, ToolRunner, Arbiter,
// ChatModule — alive for the full session) and two build_agent() overloads: one for
// tests (injected Provider + explicit Block lists) and one for the real binary (builds
// the module graph via the Launcher from the Manifest's `Module =` roster; the
// LLMModule then self-builds its provider from the Session block in on_start).

#pragma once
#include <memory>
#include <string>
#include <vector>
#include "hades/config.h"
#include "hades/executor.h"
#include "hades/llm/provider.h"
#include "hades/module/llm_module.h"
#include "hades/module/tool_runner.h"
#include "hades/module/chat_module.h"
#include "hades/module/http_server_module.h"
#include "hades/module/memory_module.h"
#include "hades/arbiter.h"
namespace hades {
class Blackboard;

// Agent: owns every module of the live graph for the duration of a session.
// The blackboard's subscriptions are non-owning, so this holder MUST outlive
// the blackboard's pump() activity. Modules are declared in reverse teardown
// order (last-declared is destroyed first); the holder is move-only.
struct Agent {
  std::unique_ptr<LLMModule>    llm;
  std::unique_ptr<ToolRunner>   tools;
  std::unique_ptr<Arbiter>      arbiter;
  std::unique_ptr<MemoryModule> memory;
  std::unique_ptr<ChatModule>   chat;
  // Optional HTTP front-end; always built/attached, but only drives the agent when
  // the binary runs in `--serve` mode (otherwise the stdin REPL drives it).
  std::unique_ptr<HttpServerModule> serve;
  // Worker pool that offloads the blocking LLM call off the single-threaded bus.
  // Set on the LLMModule (set_executor) only on the Manifest path; the test
  // overload leaves it null -> inline LLM -> existing tests unchanged.
  //
  // LOAD-BEARING: declared LAST so it is destroyed FIRST (members destruct in
  // reverse declaration order). Its joining dtor runs while `llm` and every other
  // module are still alive — a worker reads the LLMModule's `provider_` and calls
  // `bb->post` (LLM_RESPONSE), so a front-end's run_until() returning on the final
  // ASSISTANT_MESSAGE does NOT prove the worker has returned. Joining the Executor
  // first closes that use-after-free window. (Budget is NOT touched by the worker —
  // spent_ accrues on the pump thread in the LLM_RESPONSE handler.)
  // (hades_main also declares the Blackboard BEFORE the Agent, so the workers
  // join before the bus dies too — see the comment there.)
  std::unique_ptr<Executor> executor;
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
                  std::string model,
                  const Block& memory = Block{},
                  const Block& session = Block{});

// Convenience overload for the real binary: builds the module graph via the
// Launcher from the manifest's `Module =` roster (throwing MalConfig on an unknown
// module type) and wires the Tool/Objective blocks it carries. The LLMModule
// self-builds its OpenAI-compatible provider from the Session block (endpoint/
// model/api_key_env via the named env var) in its on_start.
Agent build_agent(Blackboard& bb, const Manifest& m);

}  // namespace hades
