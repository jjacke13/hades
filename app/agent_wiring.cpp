// app/agent_wiring.cpp — build_agent implementation: wire modules onto the Blackboard
//
// Instantiates LLMModule, ToolRunner, Arbiter, and ChatModule in dependency order:
// ToolRunner is started first so ToolRegistry::ensure_warm() runs native describe
// probes exactly once; Arbiter then receives the resulting ToolSpecs and any Objective
// instances (StayOnBudget, AvoidDestructive) before attaching to the Blackboard.
// The Manifest overload resolves the api key and builds an OpenAICompatProvider via cpr_http().

#include "app/agent_wiring.h"
#include <cstdlib>
#include "hades/blackboard.h"
#include "hades/launcher.h"  // MalConfig
#include "hades/llm/http.h"
#include "hades/llm/openai_compat_provider.h"
#include "hades/objective/avoid_destructive.h"
#include "hades/objective/stay_on_budget.h"
#include "hades/prompt.h"  // assemble_system_prompt
#include "hades/module/memory_module.h"
namespace hades {
namespace {

// Map one Objective block onto a concrete Objective. Unknown types are skipped
// (the manifest parser already collected a warning); we never throw here so a
// stray block can't take down the whole graph.
std::unique_ptr<Objective> make_objective(const Block& b) {
  if (b.name == "stay_on_budget") {
    double cap = 0.0;
    set_pos_double_on_string(b.kv.count("hard_cap_usd") ? b.kv.at("hard_cap_usd") : "0", cap);
    return std::make_unique<StayOnBudget>(cap);
  }
  if (b.name == "avoid_destructive") return std::make_unique<AvoidDestructive>();
  return nullptr;
}

// Shared wiring for both public overloads. `session` feeds the LLM module its
// price_per_mtok (empty Block => 0); `llm_provider` is the injected backend.
Agent build_agent_impl(Blackboard& bb,
                       const Block& session,
                       std::unique_ptr<Provider> llm_provider,
                       const std::vector<Block>& tools,
                       const std::vector<Block>& objectives,
                       const Block& memory,
                       std::string model) {
  Agent a;
  a.llm     = std::make_unique<LLMModule>(std::move(llm_provider));
  a.tools   = std::make_unique<ToolRunner>();
  a.arbiter = std::make_unique<Arbiter>();
  a.memory  = std::make_unique<MemoryModule>();
  a.chat    = std::make_unique<ChatModule>();
  a.serve   = std::make_unique<HttpServerModule>();

  // Single source of truth: each memory tool writes the same file its reader uses.
  //   save_memory -> archival store (Memory block `store`), read by MemoryModule.
  //   pin_fact    -> core file (Session `memory_file`),     read live by the Arbiter.
  // Append each configured path to the matching tool's command; copy-then-modify (never
  // touch the caller's blocks). A path is whitespace-split downstream, so reject whitespace.
  auto reject_ws = [](const std::string& p, const char* what) {
    for (char c : p)
      if (std::isspace(static_cast<unsigned char>(c)))
        throw MalConfig(std::string(what) + " path must not contain whitespace: " + p);
  };
  const std::string store_path =
      memory.kv.count("store") ? memory.kv.at("store") : ".hades/memory.jsonl";
  const std::string core_path = session.kv.count("memory_file") ? session.kv.at("memory_file") : "";
  reject_ws(store_path, "memory store");
  if (!core_path.empty()) reject_ws(core_path, "memory_file");
  std::vector<Block> tools_resolved;
  tools_resolved.reserve(tools.size());
  for (Block t : tools) {
    if (t.name == "save_memory" && t.kv.count("native"))
      t.kv["native"] = t.kv["native"] + " " + store_path;
    else if (t.name == "pin_fact" && t.kv.count("native") && !core_path.empty())
      t.kv["native"] = t.kv["native"] + " " + core_path;
    tools_resolved.push_back(std::move(t));
  }

  // 1) ToolRunner first: load every Tool block, then on_start WARMS the registry
  //    (each native `describe` runs exactly once) so the schemas exist before the
  //    Arbiter pulls them.
  for (const auto& t : tools_resolved) a.tools->add_tool(t);
  a.tools->on_start(Block{}, bb);
  a.tools->on_attach(bb);

  // 2) LLM: on_start reads price_per_mtok from the Session block and keeps the
  //    injected provider.
  a.llm->on_start(session, bb);
  a.llm->on_attach(bb);

  // 2b) MemoryModule BEFORE the Arbiter: on a USER_MESSAGE the pump dispatches in
  //     registration order, so the module posts RETRIEVED_MEMORY (latest-value updated
  //     synchronously) before the Arbiter's start_turn() reads it the same turn.
  a.memory->on_start(memory, bb);
  a.memory->on_attach(bb);

  // 3) Arbiter: now that the registry is warm, hand it the tool specs, model,
  //    and objectives, then attach it to the event loop.
  a.arbiter->set_tools(a.tools->registry().specs());
  a.arbiter->set_model(std::move(model));
  a.arbiter->set_system_prompt(assemble_system_prompt(session));  // SOUL/USER/MEMORY (empty Block -> "")
  a.arbiter->set_memory_path(core_path);  // live core memory (memory_file), re-read each turn
  for (const auto& ob : objectives)
    if (auto o = make_objective(ob)) a.arbiter->add_objective(std::move(o));
  a.arbiter->on_attach(bb);

  // 4) Chat last: it is the user-facing surface and only needs attach (its REPL
  //    is driven by the caller).
  a.chat->on_attach(bb);

  // 5) HTTP front-end: attach its capture subscriptions; only drives the agent if
  //    the binary calls serve->listen() (--serve mode). Inert otherwise.
  a.serve->on_attach(bb);
  return a;
}

}  // namespace

Agent build_agent(Blackboard& bb,
                  std::unique_ptr<Provider> llm,
                  const std::vector<Block>& tools,
                  const std::vector<Block>& objectives,
                  std::string model,
                  const Block& memory,
                  const Block& session) {
  return build_agent_impl(bb, session, std::move(llm), tools, objectives, memory, std::move(model));
}

Agent build_agent(Blackboard& bb, const Manifest& m) {
  auto session = m.session();
  if (!session) throw MalConfig("manifest has no Session block");
  const Block& s = *session;

  const std::string endpoint = s.kv.count("endpoint") ? s.kv.at("endpoint") : "";
  const std::string model    = s.kv.count("model")    ? s.kv.at("model")    : "";
  const std::string env      = s.kv.count("api_key_env") ? s.kv.at("api_key_env") : "HADES_API_KEY";
  const char* key = std::getenv(env.c_str());
  if (!key) throw MalConfig("LLM api key env var not set: " + env);

  const auto mem_blocks = m.of("Memory");
  const Block memory = mem_blocks.empty() ? Block{} : mem_blocks.front();

  auto provider = std::make_unique<OpenAICompatProvider>(endpoint, key, model, cpr_http());
  return build_agent_impl(bb, s, std::move(provider), m.of("Tool"), m.of("Objective"), memory, model);
}

}  // namespace hades
