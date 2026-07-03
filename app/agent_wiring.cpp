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
#include <sstream>
#include "hades/blackboard.h"
#include "hades/launcher.h"  // Launcher + MalConfig
#include "hades/llm/http.h"
#include "hades/llm/openai_compat_provider.h"
#include "hades/objective/avoid_destructive.h"
#include "hades/objective/capability_policy.h"
#include "hades/objective/peer_loop_guard.h"
#include "hades/objective/stay_on_budget.h"
#include "hades/bridge/protocol.h"  // valid_peer_name
#include "hades/prompt.h"  // assemble_system_prompt
#include "hades/skills/scan.h"  // resolve_skills_dir
#include "hades/module/memory_module.h"
#include "hades/timeouts.h"  // kDefaultLlmTimeoutS / kDefaultTurnIdleTimeoutS
namespace hades {
namespace {

// Split a manifest value into a whitespace-separated list. This is the one place where
// whitespace is an INTENDED list separator (CapabilityPolicy path/host scopes carry several
// entries per key); tool store paths deliberately forbid whitespace (see wire_agent).
std::vector<std::string> split_ws_list(const std::string& v) {
  std::vector<std::string> out;
  std::istringstream is(v);
  std::string w;
  while (is >> w) out.push_back(w);
  return out;
}

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
  if (b.name == "capability_policy") {
    // Path/host scopes are whitespace-separated lists; the two bools are set-on-string
    // (default true in CapabilityScope, so an omitted key keeps the safe default).
    CapabilityScope sc;
    if (b.kv.count("fs_read_allow"))  sc.fs_read_allow  = split_ws_list(b.kv.at("fs_read_allow"));
    if (b.kv.count("fs_deny"))        sc.fs_deny        = split_ws_list(b.kv.at("fs_deny"));
    if (b.kv.count("net_deny_hosts")) sc.net_deny_hosts = split_ws_list(b.kv.at("net_deny_hosts"));
    if (b.kv.count("block_private_net"))
      set_bool_on_string(b.kv.at("block_private_net"), sc.block_private_net);
    if (b.kv.count("confirm_unscoped"))
      set_bool_on_string(b.kv.at("confirm_unscoped"), sc.confirm_unscoped);
    return std::make_unique<CapabilityPolicy>(std::move(sc));
  }
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
                std::string model,
                const Block& embedding = Block{},
                const std::string& session_path = "",
                const Block& skills_cfg = Block{},
                const Block& telegram_cfg = Block{},
                const Block& bridge_cfg = Block{},
                const std::vector<Block>& peer_blocks = {}) {
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

  // Skills dir: same single-source-of-truth pattern as the memory paths — the module scans the
  // SAME dir the tools' argv points at. Resolved once here; whitespace would split the argv.
  const std::string skills_dir = resolve_skills_dir(skills_cfg);
  reject_ws(skills_dir, "skills dir");

  // Bridge identity + peer roster. The Bridge BLOCK is the agent's bridge identity (name/
  // secret/timeout) — needed by the ask_agent tool even without the listener module; the
  // bridge MODULE is only the inbound listener. Fail fast on a mis-wiring, BEFORE any
  // on_start side effects.
  std::map<std::string, std::string> peers;
  for (const auto& p : peer_blocks) {
    if (!valid_peer_name(p.name))
      throw MalConfig("Peer block has an invalid name: " + p.name);
    if (!p.kv.count("url") || p.kv.at("url").empty())
      throw MalConfig("Peer " + p.name + " requires a url");
    reject_ws(p.kv.at("url"), "peer url");
    if (!peers.emplace(p.name, p.kv.at("url")).second)
      throw MalConfig("duplicate Peer block: " + p.name);
  }
  const std::string bridge_name =
      bridge_cfg.kv.count("name") ? bridge_cfg.kv.at("name") : "";
  const std::string bridge_secret_env =
      bridge_cfg.kv.count("secret_env") ? bridge_cfg.kv.at("secret_env") : "HADES_BRIDGE_SECRET";
  double ask_timeout_s = kDefaultAskTimeoutS;
  if (bridge_cfg.kv.count("ask_timeout_s"))
    set_pos_double_on_string(bridge_cfg.kv.at("ask_timeout_s"), ask_timeout_s);
  bool has_ask_agent = false;
  for (const auto& t : tools) if (t.name == "ask_agent") has_ask_agent = true;
  if (has_ask_agent) {
    if (peers.empty())
      throw MalConfig("ask_agent tool requires at least one Peer block (nobody to call)");
    if (!valid_peer_name(bridge_name))
      throw MalConfig("ask_agent tool requires Bridge { name } (the agent's own peer name)");
  }

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
    else if ((t.name == "use_skill" || t.name == "save_skill") && t.kv.count("native"))
      t.kv["native"] = t.kv["native"] + " " + skills_dir;
    else if (t.name == "ask_agent" && t.kv.count("native")) {
      // argv: <own_name> <secret_env> <timeout_s> <name=url>... (peers sorted — std::map).
      auto fmt = [](double d) { std::ostringstream o; o << d; return o.str(); };
      std::string argv_tail = " " + bridge_name + " " + bridge_secret_env + " " +
                              fmt(ask_timeout_s);
      for (const auto& [pname, purl] : peers) argv_tail += " " + pname + "=" + purl;
      t.kv["native"] = t.kv["native"] + argv_tail;
      // ToolRunner cap ABOVE the tool's inner HTTP timeout: the tool reports its own timeout
      // error instead of being killed mid-write (single source: Bridge.ask_timeout_s).
      t.kv["timeout_s"] = fmt(ask_timeout_s + 10);
    }
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

  // 2c) EmbeddingMemoryModule (optional, opt-in via the `Module = embedding_memory` roster)
  //     ALSO before the Arbiter, for the same reason as MemoryModule: it must post
  //     RETRIEVED_MEMORY_SEMANTIC on the same pump before start_turn() reads it. BOTH the
  //     live-session path AND the executor must be set BEFORE on_attach, because on_attach is
  //     what submits the index worker: set_live_session_path writes the member the worker reads
  //     (live_session_path_), and the Executor queue's synchronization makes that write
  //     happen-before the worker dequeue — so the worker never races the main thread on that
  //     string (the Task-10 data race) and never excludes nothing on a resume. The executor
  //     (live path; created before wire_agent) lets the corpus index run OFF the bus. On the
  //     test overload a.embedding is null, a.executor null, session_path "" -> this is inert.
  if (a.embedding) {
    a.embedding->on_start(embedding, bb);
    a.embedding->set_live_session_path(session_path);  // BEFORE on_attach -> happens-before the worker
    if (a.executor) a.embedding->set_executor(a.executor.get());
    a.embedding->on_attach(bb);  // subscribes USER_MESSAGE before the Arbiter does; submits the worker
  }

  // 2d) SkillsModule: posts SKILLS_ANNOUNCE at attach. The Arbiter reads it via get()
  //     (latest-value updates on post, before any pump), so ordering vs the Arbiter is not
  //     load-bearing — kept before it for consistency with the other posting modules.
  if (a.skills) {
    a.skills->on_start(skills_cfg, bb);
    a.skills->on_attach(bb);
  }

  // 3) Arbiter: now that the registry is warm, hand it the tool specs (empty when the
  //    ToolRunner is absent), model, and objectives, then attach it to the event loop.
  if (a.arbiter) {
    a.arbiter->set_tools(a.tools ? a.tools->registry().specs() : std::vector<ToolSpec>{});
    a.arbiter->set_model(std::move(model));
    a.arbiter->set_system_prompt(assemble_system_prompt(session));  // SOUL/USER/MEMORY (empty Block -> "")
    a.arbiter->set_memory_path(core_path);  // live core memory (memory_file), re-read each turn
    // The bridge brings its OWN safety behavior (not manifest-optional): a peer-driven turn
    // must never ask_agent onward — the A<->B mutual-wait deadlock. Registered FIRST so its
    // hard veto short-circuits before any manifest objective.
    if (a.bridge) a.arbiter->add_objective(std::make_unique<PeerLoopGuard>());
    for (const auto& ob : objectives)
      if (auto o = make_objective(ob)) a.arbiter->add_objective(std::move(o));
    a.arbiter->on_attach(bb);
  }

  // 4) Chat: the user-facing surface. Inject the shared TurnGate BEFORE on_attach so its
  //    REPL serializes turns against the other front-ends. Its REPL is driven by the caller.
  if (a.chat) {
    a.chat->set_turn_gate(a.gate.get());
    a.chat->on_attach(bb);
  }

  // 5) HTTP front-end: same shared gate BEFORE on_attach; only drives the agent if the
  //    binary calls serve->listen() (--serve mode). Inert otherwise.
  if (a.serve) {
    a.serve->set_turn_gate(a.gate.get());
    a.serve->on_attach(bb);
  }

  // 5b) Bridge: inbound peer front-end + outbound share push. Gate BEFORE on_attach (it
  //     drives whole turns like the other front-ends); peers BEFORE on_attach (the allowlist
  //     must exist before any request can arrive — the listener itself starts later, in
  //     hades_main); executor for the share-push offload. on_start throws MalConfig on a
  //     missing name / unset secret env.
  if (a.bridge) {
    a.bridge->set_turn_gate(a.gate.get());
    a.bridge->on_start(bridge_cfg, bb);
    a.bridge->set_peers(peers);
    if (a.executor) a.bridge->set_executor(a.executor.get());
    a.bridge->on_attach(bb);
  }

  // 6) Telegram front-end: config (MalConfig on missing allow_users / token env) + captures.
  //    The poll thread is NOT started here — hades_main calls start_polling() explicitly, so
  //    tests and non-interactive builds never spawn a surprise thread.
  if (a.telegram) {
    a.telegram->set_turn_gate(a.gate.get());
    a.telegram->on_start(telegram_cfg, bb);
    a.telegram->on_attach(bb);
  }
}

}  // namespace

void enforce_manifest(const Manifest& m) {
  auto fatal = fatal_warnings(m);
  if (fatal.empty()) return;
  std::string msg = "corrupt manifest — multiple key=value pairs packed on one physical line "
                    "(split each onto its own line in a { } block):";
  for (const auto& w : fatal) msg += "\n  - " + w;
  throw MalConfig(msg);
}

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
  // Test path never builds a.embedding -> session_path "" makes set_live_session_path a no-op.
  wire_agent(a, bb, session, tools, objectives, memory, std::move(model), Block{}, "");
  return a;
}

Agent build_agent(Blackboard& bb, const Manifest& m, const std::string& session_path) {
  enforce_manifest(m);   // refuse to build from a manifest with packed multi-kv lines
  auto session = m.session();
  if (!session) throw MalConfig("manifest has no Session block");
  const Block& s = *session;
  const std::string model = s.kv.count("model") ? s.kv.at("model") : "";

  // Manifest-configurable think-time limits (Session block; defaults from timeouts.h).
  // Fail loud BEFORE building anything heavy if the load-bearing invariant is violated:
  // turn_idle_timeout_s MUST stay > llm_timeout_s so a slow-but-alive LLM call posts back
  // (resetting the idle deadline) before run_until abandons the turn. (Live path only —
  // the test build_agent overload never runs this, so existing inline tests are unchanged.)
  // NOTE: this resolves llm_timeout_s purely for the invariant guard below; LLMModule::on_start
  // ALSO resolves it from this same Session block (for the cpr per-call timeout). The duplicate
  // read is intentional — the LLM self-builds its provider from the Session block, and both sites
  // use the same key + default + validator, so they compute an identical value. Do not "dedupe".
  double llm_timeout_s = kDefaultLlmTimeoutS;
  if (s.kv.count("llm_timeout_s"))
    set_pos_double_on_string(s.kv.at("llm_timeout_s"), llm_timeout_s);
  double turn_idle_timeout_s = kDefaultTurnIdleTimeoutS;
  if (s.kv.count("turn_idle_timeout_s"))
    set_pos_double_on_string(s.kv.at("turn_idle_timeout_s"), turn_idle_timeout_s);
  if (turn_idle_timeout_s <= llm_timeout_s) {
    // Format without std::to_string's trailing zeros (900.000000 -> "900", 1.5 -> "1.5").
    auto fmt = [](double d) { std::ostringstream o; o << d; return o.str(); };
    throw MalConfig("turn_idle_timeout_s (" + fmt(turn_idle_timeout_s) +
                    ") must be greater than llm_timeout_s (" + fmt(llm_timeout_s) +
                    ") — a slow LLM call would be abandoned mid-flight");
  }

  // pAntler: the Module= roster decides which modules exist. Factories just construct;
  // the LLM self-builds its provider from the Session block in on_start (existing path).
  Launcher launcher(bb);
  launcher.register_factory("llm",         []{ return std::make_unique<LLMModule>(); });
  launcher.register_factory("tool_runner", []{ return std::make_unique<ToolRunner>(); });
  launcher.register_factory("memory",      []{ return std::make_unique<MemoryModule>(); });
  launcher.register_factory("embedding_memory",
                            []{ return std::make_unique<EmbeddingMemoryModule>(); });
  launcher.register_factory("skills",      []{ return std::make_unique<SkillsModule>(); });
  launcher.register_factory("arbiter",     []{ return std::make_unique<Arbiter>(); });
  launcher.register_factory("chat",        []{ return std::make_unique<ChatModule>(); });
  launcher.register_factory("serve",       []{ return std::make_unique<HttpServerModule>(); });
  launcher.register_factory("telegram",    []{ return std::make_unique<TelegramModule>(); });
  launcher.register_factory("bridge",      []{ return std::make_unique<BridgeModule>(); });
  launcher.instantiate(m);   // MalConfig on unknown Module type

  Agent a;
  a.llm     = take_as<LLMModule>(launcher, "llm");
  a.tools   = take_as<ToolRunner>(launcher, "tool_runner");
  a.memory  = take_as<MemoryModule>(launcher, "memory");
  a.embedding = take_as<EmbeddingMemoryModule>(launcher, "embedding_memory");
  a.skills  = take_as<SkillsModule>(launcher, "skills");
  a.arbiter = take_as<Arbiter>(launcher, "arbiter");
  a.chat    = take_as<ChatModule>(launcher, "chat");
  a.serve   = take_as<HttpServerModule>(launcher, "serve");
  a.telegram = take_as<TelegramModule>(launcher, "telegram");
  a.bridge  = take_as<BridgeModule>(launcher, "bridge");

  const auto mem_blocks = m.of("Memory");
  const Block memory = mem_blocks.empty() ? Block{} : mem_blocks.front();
  const auto embed_blocks = m.of("Embedding");
  const Block embedding = embed_blocks.empty() ? Block{} : embed_blocks.front();
  const auto skills_blocks = m.of("Skills");
  const Block skills_cfg = skills_blocks.empty() ? Block{} : skills_blocks.front();
  const auto tg_blocks = m.of("Telegram");
  const Block telegram_cfg = tg_blocks.empty() ? Block{} : tg_blocks.front();
  const auto bridge_blocks = m.of("Bridge");
  const Block bridge_cfg = bridge_blocks.empty() ? Block{} : bridge_blocks.front();
  const auto peer_blocks = m.of("Peer");

  // Live path only: own a small worker pool and offload the blocking LLM call onto it so
  // the bus stays responsive (front-ends drive turns via run_until). Created BEFORE
  // wire_agent because the optional EmbeddingMemoryModule submits its corpus index to this
  // executor when it attaches INSIDE wire_agent (set_executor must precede its on_attach) —
  // so the index runs off the pump thread. The TEST overload deliberately leaves a.executor
  // null -> the LLM runs inline + a.embedding is null -> the whole existing suite is
  // unchanged. `a.executor` is the Agent's LAST member, so it is joined before the
  // modules/Blackboard tear down regardless of this construction order (see agent_wiring.h /
  // hades_main).
  constexpr unsigned kExecutorThreads = 2;
  a.executor = std::make_unique<Executor>(kExecutorThreads);
  if (a.llm) a.llm->set_executor(a.executor.get());

  // Pass the resolved live-session path through so wire_agent sets it on the embedding module
  // BEFORE on_attach submits the index worker (race-free exclusion — see wire_agent's 2c block).
  wire_agent(a, bb, s, m.of("Tool"), m.of("Objective"), memory, model, embedding, session_path,
             skills_cfg, telegram_cfg, bridge_cfg, peer_blocks);

  // Apply the resolved idle ceiling to whichever front-end(s) the roster built (the LLM
  // resolved its own llm_timeout_s from the same Session block in on_start). Null-guarded:
  // a roster may omit chat and/or serve. Tests still set small values through the same seam.
  if (a.chat)  a.chat->set_turn_timeout_s(turn_idle_timeout_s);
  if (a.serve) a.serve->set_collect_timeout_s(turn_idle_timeout_s);
  if (a.telegram) a.telegram->set_turn_timeout_s(turn_idle_timeout_s);
  if (a.bridge) a.bridge->set_turn_timeout_s(turn_idle_timeout_s);
  return a;
}

}  // namespace hades
