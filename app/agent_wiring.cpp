// app/agent_wiring.cpp — build_agent implementation: wire modules onto the Blackboard
//
// The Manifest overload routes through the Launcher (pAntler): the `Module =` roster
// decides which modules exist; factories just construct, and wire_agent() drives the
// per-module config/attach guarding each step on the module's presence (members may be
// null when the roster omits them). The LLM self-builds its provider from the Session
// block in on_start. wire_agent preserves the exact wiring order: ToolRunner is warmed
// first so ToolRegistry::ensure_warm() runs native describe probes once; the Arbiter
// then receives the resulting ToolSpecs and any Objective instances; the MemoryModule
// attaches before the Arbiter so RETRIEVED_MEMORY is posted before start_turn() reads it.
// The test overload constructs all six modules directly (injecting a scripted Provider).

#include "app/agent_wiring.h"
#include <cstdlib>
#include "hades/blackboard.h"
#include "hades/launcher.h"  // Launcher + MalConfig
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

// Transfer the first module of `type` out of the Launcher and downcast it to its
// concrete type. The factory registered under `type` constructs exactly that type,
// and take() matches on Module::type() == the factory key, so the static_cast is
// sound. Returns nullptr when the roster omitted the module (take() yields nullptr).
template <class T>
std::unique_ptr<T> take_as(Launcher& L, const std::string& type) {
  std::unique_ptr<Module> m = L.take(type);
  return std::unique_ptr<T>(static_cast<T*>(m.release()));  // factory key == module type(); safe
}

// Shared wiring for both public overloads. Operates on an ALREADY-POPULATED Agent
// whose members may be null (the Manifest path leaves out modules absent from the
// `Module =` roster; the test path constructs all six). Each module's config/attach
// is guarded on its presence; the wiring ORDER is enforced by this call sequence,
// not by the roster order: tools (warm) -> llm -> memory (attach) -> arbiter -> chat
// -> serve. `session` feeds the LLM module its config (price_per_mtok, and on the
// Manifest path endpoint/model/api_key_env for the self-built provider).
void wire_agent(Agent& a,
                Blackboard& bb,
                const Block& session,
                const std::vector<Block>& tools,
                const std::vector<Block>& objectives,
                const Block& memory,
                std::string model) {
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

  // pin_fact writes the core-memory file the Arbiter folds in each turn; without a
  // configured memory_file the two would target different files (silent drift), so
  // require it explicitly when the pin_fact tool is present.
  bool has_pin_fact = false;
  for (const auto& t : tools) if (t.name == "pin_fact") has_pin_fact = true;
  if (has_pin_fact && core_path.empty())
    throw MalConfig("pin_fact tool requires a memory_file in the Session block");

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
  if (a.tools) {
    for (const auto& t : tools_resolved) a.tools->add_tool(t);
    a.tools->on_start(Block{}, bb);
    a.tools->on_attach(bb);
  }

  // 2) LLM: on_start reads price_per_mtok from the Session block and either keeps the
  //    injected provider (test path) or self-builds an OpenAICompatProvider from the
  //    Session block's endpoint/model/api_key_env (Manifest path).
  if (a.llm) {
    a.llm->on_start(session, bb);
    a.llm->on_attach(bb);
  }

  // 2b) MemoryModule BEFORE the Arbiter: on a USER_MESSAGE the pump dispatches in
  //     registration order, so the module posts RETRIEVED_MEMORY (latest-value updated
  //     synchronously) before the Arbiter's start_turn() reads it the same turn.
  if (a.memory) {
    a.memory->on_start(memory, bb);
    a.memory->on_attach(bb);
  }

  // 3) Arbiter: now that the registry is warm, hand it the tool specs (empty when the
  //    ToolRunner is absent), model, and objectives, then attach it to the event loop.
  if (a.arbiter) {
    a.arbiter->set_tools(a.tools ? a.tools->registry().specs() : std::vector<ToolSpec>{});
    a.arbiter->set_model(std::move(model));
    a.arbiter->set_system_prompt(assemble_system_prompt(session));  // SOUL/USER/MEMORY (empty Block -> "")
    a.arbiter->set_memory_path(core_path);  // live core memory (memory_file), re-read each turn
    for (const auto& ob : objectives)
      if (auto o = make_objective(ob)) a.arbiter->add_objective(std::move(o));
    a.arbiter->on_attach(bb);
  }

  // 4) Chat last: it is the user-facing surface and only needs attach (its REPL
  //    is driven by the caller).
  if (a.chat) a.chat->on_attach(bb);

  // 5) HTTP front-end: attach its capture subscriptions; only drives the agent if
  //    the binary calls serve->listen() (--serve mode). Inert otherwise.
  if (a.serve) a.serve->on_attach(bb);
}

}  // namespace

Agent build_agent(Blackboard& bb,
                  std::unique_ptr<Provider> llm,
                  const std::vector<Block>& tools,
                  const std::vector<Block>& objectives,
                  std::string model,
                  const Block& memory,
                  const Block& session) {
  // Test path: construct ALL six modules directly (inject the scripted Provider into
  // the LLMModule), then run the shared wiring. Every member is present, so the null
  // guards in wire_agent are no-ops and behavior matches the pre-Launcher graph.
  Agent a;
  a.llm     = std::make_unique<LLMModule>(std::move(llm));
  a.tools   = std::make_unique<ToolRunner>();
  a.arbiter = std::make_unique<Arbiter>();
  a.memory  = std::make_unique<MemoryModule>();
  a.chat    = std::make_unique<ChatModule>();
  a.serve   = std::make_unique<HttpServerModule>();
  wire_agent(a, bb, session, tools, objectives, memory, std::move(model));
  return a;
}

Agent build_agent(Blackboard& bb, const Manifest& m) {
  auto session = m.session();
  if (!session) throw MalConfig("manifest has no Session block");
  const Block& s = *session;
  const std::string model = s.kv.count("model") ? s.kv.at("model") : "";

  // pAntler: the Module= roster decides which modules exist. Factories just construct;
  // the LLM self-builds its provider from the Session block in on_start (existing path).
  Launcher launcher(bb);
  launcher.register_factory("llm",         []{ return std::make_unique<LLMModule>(); });
  launcher.register_factory("tool_runner", []{ return std::make_unique<ToolRunner>(); });
  launcher.register_factory("memory",      []{ return std::make_unique<MemoryModule>(); });
  launcher.register_factory("arbiter",     []{ return std::make_unique<Arbiter>(); });
  launcher.register_factory("chat",        []{ return std::make_unique<ChatModule>(); });
  launcher.register_factory("serve",       []{ return std::make_unique<HttpServerModule>(); });
  launcher.instantiate(m);   // MalConfig on unknown Module type

  Agent a;
  a.llm     = take_as<LLMModule>(launcher, "llm");
  a.tools   = take_as<ToolRunner>(launcher, "tool_runner");
  a.memory  = take_as<MemoryModule>(launcher, "memory");
  a.arbiter = take_as<Arbiter>(launcher, "arbiter");
  a.chat    = take_as<ChatModule>(launcher, "chat");
  a.serve   = take_as<HttpServerModule>(launcher, "serve");

  const auto mem_blocks = m.of("Memory");
  const Block memory = mem_blocks.empty() ? Block{} : mem_blocks.front();
  wire_agent(a, bb, s, m.of("Tool"), m.of("Objective"), memory, model);
  return a;
}

}  // namespace hades
