// app/hades_main.cpp — hades binary entrypoint: parse manifest, build agent, run REPL
//
// Parses the Manifest from argv[1], resolves and redacts the LLM api key in the
// Eventlog before any post() is logged (so the secret never appears in session.log),
// then calls build_agent() and runs ChatModule::run_repl(). Bad config (missing
// Session block, unset api key env var) exits with code 1 via MalConfig; missing
// argv exits 2.
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/eventlog.h"
#include "hades/launcher.h"  // MalConfig
#include "hades/serve_config.h"
using namespace hades;

namespace {

std::string read_file(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw MalConfig("cannot open manifest: " + path);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

// Resolve the api key from the Session block so we can redact it in the eventlog
// BEFORE any post() is logged. Mirrors the resolution build_agent() performs;
// throwing here keeps the failure visible at the top level.
std::string resolve_api_key(const Manifest& m) {
  auto session = m.session();
  if (!session) throw MalConfig("manifest has no Session block");
  const std::string env =
      session->kv.count("api_key_env") ? session->kv.at("api_key_env") : "HADES_API_KEY";
  const char* key = std::getenv(env.c_str());
  if (!key) throw MalConfig("LLM api key env var not set: " + env);
  return key;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: hades <manifest> [--serve [port]]\n";
    return 2;
  }
  // Optional `--serve [port]`: run the HTTP front-end (web UI + JSON API) instead of the
  // stdin REPL. The port is optional here (falls back to the Serve block / default 8080).
  bool serve = false;
  int cli_port = 0;
  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--serve") {
      serve = true;
      if (i + 1 < argc && std::atoi(argv[i + 1]) > 0) cli_port = std::atoi(argv[++i]);
    }
  }
  try {
    const Manifest manifest = parse_manifest(read_file(argv[1]));

    // Resolve + redact the key BEFORE constructing the blackboard, so the secret
    // can never appear unredacted in session.log.
    const std::string key = resolve_api_key(manifest);
    Eventlog eventlog("session.log");
    eventlog.add_redaction(key);

    // LOAD-BEARING declaration order: `bb` BEFORE `agent`, so at scope exit `agent`
    // (and the Executor it owns, its last member) is destroyed FIRST and `bb` LAST.
    // The Executor's dtor joins its workers while the modules AND the Blackboard are
    // still alive — a worker posts BUDGET_SPENT_USD onto the bus AFTER LLM_RESPONSE,
    // so a turn's run_until can return before the worker has finished touching
    // llm/bb. Net teardown: Executor joins workers -> modules -> Blackboard. Do NOT
    // reorder these two lines.
    Blackboard bb(&eventlog);
    Agent agent = build_agent(bb, manifest);  // owns every module for the session

    if (!agent.arbiter || !agent.llm) {
      std::cerr << "hades: manifest Module roster is missing `llm` and/or `arbiter` "
                   "— the agent cannot take a turn\n";
      return 1;
    }

    if (serve) {
      if (!agent.serve) { std::cerr << "hades: no `serve` module in the manifest Module roster\n"; return 1; }
      const ServeConfig cfg = resolve_serve_config(manifest, cli_port);
      agent.serve->listen(cfg.host, cfg.port, cfg.webroot);  // blocks until killed
    } else {
      if (!agent.chat) { std::cerr << "hades: no `chat` module in the manifest Module roster\n"; return 1; }
      agent.chat->run_repl(std::cin, std::cout);
    }
    // Agent's RAII teardown (reverse-declared) releases the modules; tool
    // subprocesses are short-lived and reaped synchronously per call.
    return 0;
  } catch (const MalConfig& e) {
    std::cerr << "hades: configuration error: " << e.what() << "\n";
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "hades: error: " << e.what() << "\n";
    return 1;
  }
}
