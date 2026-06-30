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
#include "hades/history_budget.h"  // kDefaultHistoryBudgetChars
#include "hades/launcher.h"  // MalConfig
#include "hades/serve_config.h"
#include "hades/session_id.h"  // make_session_id, resolve_session_path
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
    std::cerr << "usage: hades <manifest> [--serve [port]] [--resume [id]]\n";
    return 2;
  }
  // Optional `--serve [port]`: run the HTTP front-end (web UI + JSON API) instead of the
  // stdin REPL. The port is optional here (falls back to the Serve block / default 8080).
  // Optional `--resume [id]`: reload a prior session's conversation. With an id, resume that
  // named session (error if missing); without one, resume the newest. Composes with --serve.
  bool serve = false;
  int cli_port = 0;
  bool resume = false;
  std::string resume_id;
  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--serve") {
      serve = true;
      if (i + 1 < argc && std::atoi(argv[i + 1]) > 0) cli_port = std::atoi(argv[++i]);
    } else if (arg == "--resume") {
      resume = true;
      // Optional following non-flag token is the session id (mirror --serve [port]).
      if (i + 1 < argc && argv[i + 1][0] != '-') resume_id = argv[++i];
    }
  }
  try {
    const Manifest manifest = parse_manifest(read_file(argv[1]));
    for (const auto& w : manifest.warnings)
      std::cerr << "hades: manifest warning: " << w << "\n";
    // Enforce HERE (before resolve_api_key) is load-bearing, not redundant: build_agent()
    // also enforces, but only AFTER the key is resolved — so without this early call a
    // missing-key error would mask a corrupt-manifest error. Throws MalConfig (caught below).
    enforce_manifest(manifest);

    // Resolve + redact the key BEFORE constructing the blackboard, so the secret
    // can never appear unredacted in session.log.
    const std::string key = resolve_api_key(manifest);

    // Resolve the per-session conversation jsonl + the per-turn history budget from the Session
    // block, then pick the file path from the --resume flag. Done BEFORE building the agent so a
    // `--resume <missing-id>` fails fast (MalConfig -> caught below) before any heavy setup. The
    // resolved path + budget are wired into the Arbiter after build_agent (REPL and --serve both).
    auto session = manifest.session();  // resolve_api_key already verified it exists
    std::string sessions_dir = ".hades/sessions";
    double history_budget = kDefaultHistoryBudgetChars;
    if (session) {
      if (session->kv.count("sessions_dir")) sessions_dir = session->kv.at("sessions_dir");
      if (session->kv.count("history_budget_chars"))
        set_pos_double_on_string(session->kv.at("history_budget_chars"), history_budget);
    }
    const std::string new_id = make_session_id();
    const SessionResolution sr = resolve_session_path(sessions_dir, resume, resume_id, new_id);
    const std::string session_path = sr.path;
    // fresh_fallback is set ONLY when a resume found nothing to resume (explicit flag, not a
    // string compare — a new session's `-N` collision suffix no longer breaks this note).
    if (sr.fresh_fallback)
      std::cerr << "hades: no prior session in " << sessions_dir << "; starting fresh\n";

    Eventlog eventlog("session.log");
    eventlog.add_redaction(key);

    // LOAD-BEARING declaration order: `bb` BEFORE `agent`, so at scope exit `agent`
    // (and the Executor it owns, its last member) is destroyed FIRST and `bb` LAST.
    // The Executor's dtor joins its workers while the modules AND the Blackboard are
    // still alive — a worker reads the LLMModule's `provider_` and calls `bb->post`
    // (LLM_RESPONSE), so a turn's run_until can return before the worker has finished
    // touching llm/bb. (Budget is NOT touched by the worker — spent_ accrues on the
    // pump thread.) Net teardown: Executor joins workers -> modules -> Blackboard. Do
    // NOT reorder these two lines.
    Blackboard bb(&eventlog);
    // Pass `session_path` (resolved above) INTO build_agent so the optional embedding module's
    // live-session exclusion is set BEFORE its on_attach submits the index worker — the write
    // then happens-before the worker reads it (Executor-queue synchronization), closing the
    // Task-10 data race. Do NOT set it again after this call (the late write was the race).
    Agent agent = build_agent(bb, manifest, session_path);  // owns every module for the session

    if (!agent.arbiter || !agent.llm) {
      std::cerr << "hades: manifest Module roster is missing `llm` and/or `arbiter` "
                   "— the agent cannot take a turn\n";
      return 1;
    }

    // Wire session persistence into the Arbiter (both front-ends; resume composes with --serve).
    // Order: budget first, then the path, then load — load_history reads the path just set.
    agent.arbiter->set_history_budget_chars(history_budget);
    agent.arbiter->set_session_path(session_path);
    // NOTE: the embedding module's live-session exclusion (set_live_session_path) is wired INSIDE
    // build_agent, BEFORE the index worker is submitted (on_attach) — see the wire_agent 2c block.
    // It must NOT be set here: this late write would race the running worker (the Task-10 bug).
    // Also give the Arbiter the sessions dir so a `/new` mid-run rotates to a fresh file in the
    // same dir (no id-gen injection in prod -> defaults to make_session_id()).
    agent.arbiter->set_session_dir(sessions_dir);
    // The --serve front-end reads the same session jsonl for GET /history (resumed-transcript
    // render). Null-guarded: a REPL-only roster omits `serve`. Same resolved path as the Arbiter.
    if (agent.serve) agent.serve->set_session_path(session_path);
    if (resume) agent.arbiter->load_history();

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
