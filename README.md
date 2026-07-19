# hades

A self-hosted AI-agent harness in C++20 that ports the [MOOS-IvP](https://oceanai.mit.edu/moos-ivp/) marine-robotics architecture to a software, LLM-driven agent. One plain-text **manifest** declares one **agent**: its model, its tools, its goals, its memory, its chat surfaces, its schedule, its peers. The process is just the vehicle the agent boots in.

> Working on this repo as an AI agent? Jump to [For AI agents](#for-ai-agents).

---

## For humans

### The idea: the agent is the center

Most harnesses are **app-centric**: an application embeds an LLM, and "the agent" is a request that lives for one conversation. hades is **agent-centric**, the way MOOS-IvP is vehicle-centric. In [MOOS-IvP](https://oceanai.mit.edu/moos-ivp/), a `.moos` mission file defines a *vehicle community* — a MOOSDB bus, a roster of apps, a helm with competing behaviors — and the vehicle is the deployed unit, not any single program. hades keeps that shape:

- A **`.hades` manifest** is the mission file. It rosters the modules (`Module =` lines), the tools, the objectives, the front-ends, the heartbeats, the peers. The binary reads it and *becomes* that agent.
- A central **Blackboard** (the MOOSDB) is the only way modules talk — pub/sub, no module calls another.
- An **Arbiter** (the helm) runs the turn loop: the LLM *proposes*, competing **Objectives** (the behaviors — budget, capability policy, destruction guard) *gate* every action before it executes.
- The agent's **identity persists outside the process**: core memory, archival memory, skills library, session history are files. Kill the process, boot the same manifest on another machine with those dirs — same agent.
- Chat surfaces are **peripherals, not the app**: the REPL, the web UI, Telegram, and SimpleX are just rostered modules that feed the same one conversation, serialized through one gate.
- More agents = more manifests. A **Bridge** module (the `pShare` analog) lets them discover each other's capabilities and delegate — a fleet, not a monolith.

### What it does

- **4 chat front-ends** — terminal REPL, browser web UI, Telegram bot, SimpleX Chat — all driving one gated conversation; voice notes in/out (STT/TTS provider seams).
- **20 native tools** (files, shell, git, http, grep/glob/edit, memory, skills, scheduling, peer delegation), each an isolated subprocess — plus any **MCP server** (stdio or Streamable HTTP): its tools are discovered at boot and announced to the model as `<block>__<tool>`.
- **Capability gate** on every action: allow / confirm / hard-veto bands from manifest scopes (path allowlists, command prefixes, SSRF-hardened net gate, per-MCP-tool allowlist). Unattended turns get only the unconfirmed power set.
- **Three memory layers** — always-on core facts (bounded, self-consolidating), archival keyword recall, opt-in semantic embeddings over memories *and* past sessions — plus a **skills library** the agent authors itself at runtime.
- **Autonomy** — cron heartbeats, reactive `when =` watches over bus state, and self-scheduled tasks: the agent plants its own future work, still fully gated.
- **Multi-agent** — authenticated agent↔agent bridge with capability cards, typed shares, trust labels, and loop protection.
- **Ops** — append-only event log with a replay CLI (`hades-scope`), session resume, ~9 MB RSS, and a fully static musl aarch64 build that runs on a bare Raspberry Pi Zero 2 W.

### Quick start

```bash
export HADES_API_KEY=<key>                                 # any OpenAI-compatible endpoint
nix develop --command cmake -S . -B build -G Ninja
nix develop --command cmake --build build
nix develop --command ctest --test-dir build               # the whole suite, ~7 s
./build/hades manifests/dev.hades                          # terminal REPL
./build/hades manifests/dev.hades --serve                  # web UI → http://localhost:8080/
nix build .#hades-aarch64-static                           # static Pi deploy dir
```

No Nix? It's a plain CMake project with five permissively-licensed deps — per-distro and macOS
recipes in [`docs/building.md`](docs/building.md).

A minimal manifest:

```
Session
{
  provider    = openai_compat
  endpoint    = https://api.ppq.ai/v1
  model       = gpt-5.5
  api_key_env = HADES_API_KEY
  memory_file = memory/facts.md
}

Module = llm
Module = tool_runner
Module = chat
Module = arbiter

Tool = fs_read { native = ./build/hades-fs-read }
Tool = shell   { native = ./build/hades-shell }

Objective = capability_policy
{
  fs_read_allow = ./workspace
  fs_deny       = .env .ssh
}
Objective = stay_on_budget { hard_cap_usd = 1.0 }
```

Every key is documented in [`docs/manifest-reference.md`](docs/manifest-reference.md); the design is in [`docs/architecture.md`](docs/architecture.md) (Mermaid diagrams, MOOS-IvP mapping table).

### Customizing your agent

Three channels, no code changes. **Persona**: `Session.system_prompt_file` (the agent's standing character and rules) and `Session.user_file` (who you are, how you like things done) take any path — the shipped `prompts/soul.md` is a default, not a requirement. **Core memory**: a small always-in-context facts file the agent edits itself (`core_memory` tool) — teach it something once, it stays taught. **Skills**: reusable instruction packs in the skills dir (`use_skill`/`save_skill`) — the agent loads them on demand and writes new ones from what it learns. Persona is yours to author; the other two the agent curates at runtime.

### Security posture (short version)

Secrets are env-var-only — never in the manifest, redacted in the event log. Tools run as subprocesses with timeouts. A built-in capability table is the authority (a tool cannot grant itself permission); writes, shell, and unknown tools confirm by default; denied paths and private-network fetches are hard-vetoed; turns with no human present (heartbeat, peer) auto-deny anything confirm-band. The manifest parser fails loud on malformed input — the binary refuses to boot rather than mis-parse a security scope.

### Status

Personal project, in active development, moving toward a public release. 740/740 tests green under ASan+UBSan and TSan. License: [MIT](LICENSE) — the whole linked dependency stack is permissive (MIT/BSD/Apache-2.0/curl).

---

## For AI agents

You are likely a coding agent working *on* this repo — or a curious agent reading a harness built for agents like you. Orientation:

**Mental model.** hades is [MOOS-IvP](https://oceanai.mit.edu/moos-ivp/) for a software agent — if you know MOOS, you know this codebase: Blackboard = MOOSDB, Module = MOOS app, Arbiter = pHelmIvP, Objective = behavior, Manifest = `.moos` mission file, Launcher = pAntler, Bridge = pShare, Eventlog + `hades-scope` = `.alog` + `alogview`. The full mapping table is in [`docs/architecture.md`](docs/architecture.md) §8. The central design fact: **the agent is the unit, defined by its manifest** — modules coordinate only through Blackboard posts, and no tool call executes un-gated.

**Read first, in order:**
1. `CLAUDE.md` — the living project memory: every shipped feature's design notes, gotchas, and invariants. Authoritative over your assumptions.
2. [`docs/manifest-reference.md`](docs/manifest-reference.md) — every manifest key, default, and footgun.
3. [`docs/architecture.md`](docs/architecture.md) — component diagram, turn sequence, bus keys, invariants.

**Layout.** `src/core/` (blackboard, executor, launcher, config, subprocess) · `src/apps/<name>/` — one dir per Module, MOOS-style · `src/behaviors/` — the Objectives · `tools/*_main.cpp` — one file per native tool binary · `app/agent_wiring.cpp` — where the manifest becomes an agent · `tests/` — GoogleTest, one file per unit.

**Build and test — only inside the Nix dev shell:**

```bash
nix develop --command cmake --build build
nix develop --command ctest --test-dir build --output-on-failure
```

Both must be green before any commit. There is a second TSan lane (`build-tsan/`) for anything touching threads.

**Invariants you must not break:**
- Subscriber handlers run **only on the pump thread**; `post()` is thread-safe, dispatch is not concurrent. Blocking work goes through the Executor or a module-owned thread that marshals back via `post()`.
- `Agent` member order is **teardown order** — thread-owning modules join in their destructors while everything they touch is still alive. Do not reorder members.
- The `capability_of` table in `src/behaviors/capability_policy.cpp` is the **authority** for what a tool may do — never derive capability from a tool's self-description.
- The manifest parser is **one key = value per line**; packed lines fail loud at boot by design.
- Secrets never appear in manifests, logs, or error strings; every exactly-one-of LLM-facing tool argument treats the empty string as absent.
- Boot fails loud (`MalConfig` before side effects); runtime fails soft (degrade, never crash a turn).

**Process.** Features go spec → plan → TDD on a feature branch, merged ff to `main` (no remote — never push). Specs and plans live in `docs/superpowers/`.
