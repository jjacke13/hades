// src/apps/tool_runner/tool_runner.cpp — the tool-execution app: module + registry + MCP
//
// Merged (2026-07-04 src reorg): module/tool_runner (TOOL_REQUEST->TOOL_RESULT, per-tool
// timeout override) + tool/registry (Tool blocks, describe/spec warm cache) + tool/
// mcp_adapter (MCP stdio call shim). Tools themselves are transient subprocesses
// (tools/*.cpp binaries) — actions, not apps; run_subprocess lives in core/subprocess.
//
// Offload (tool-offload phase 2, LLM-offload pattern): when set_executor() is given an
// Executor, the resolved tool runs on a worker (execute_tool core) and posts TOOL_RESULT
// back so the pump thread stays live mid-tool; null (default) -> inline, byte-identical.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <cpr/cpr.h>
#include "hades/module/tool_runner.h"
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/executor.h"
#include "hades/tool/subprocess.h"
#include "hades/tool/mcp_adapter.h"
#include "hades/tool/registry.h"
#include "hades/skills/scan.h"  // valid_skill_name — charset gate for discovered MCP tool names

// ── ToolRunner: TOOL_REQUEST -> subprocess/MCP -> TOOL_RESULT (was src/module/tool_runner.cpp) ──────────────
namespace hades {

static std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> v;
  std::istringstream is(s);
  std::string w;
  while (is >> w) v.push_back(w);
  return v;
}

void ToolRunner::on_start(const Block& cfg, Blackboard&) {
  if (cfg.kv.count("timeout"))
    set_pos_double_on_string(cfg.kv.at("timeout"), timeout_s_);
  if (cfg.kv.count("timeout_s"))
    set_pos_double_on_string(cfg.kv.at("timeout_s"), timeout_s_);   // Tools block key
  if (cfg.kv.count("background_timeout_s"))
    set_pos_double_on_string(cfg.kv.at("background_timeout_s"), bg_timeout_s_);
  if (cfg.kv.count("max_background")) {
    double v = 0.0;
    set_pos_double_on_string(cfg.kv.at("max_background"), v);
    if (v >= 1.0 && v <= 64.0) max_background_ = static_cast<unsigned>(v);  // bound the cast
  }
  // Build the describe/spec cache ONCE here, so we never re-spawn a tool's
  // `describe` subprocess on each TOOL_REQUEST.
  reg_.warm(timeout_s_);
}

// Pure execution core: run a RESOLVED tool (native subprocess or MCP call) and return
// {ok, content}. Thread-safe by construction — it reads ONLY its parameters (the ToolEntry
// is a value copy), so it behaves identically on the pump thread (inline) or on an
// Executor worker. All mutable-registry access (find_by_tool_name / mcp_real_name lazily
// warm caches) must happen BEFORE this, on the pump thread.
static std::pair<bool, nlohmann::json> execute_tool(const ToolEntry& te,
                                                    const std::string& name,
                                                    const std::string& real,
                                                    const nlohmann::json& args,
                                                    double timeout) {
  if (te.kind == "native") {
    nlohmann::json call{{"call", name}, {"args", args}};
    auto r = run_subprocess(split_ws(te.command), call.dump(), timeout);
    if (r.timed_out) return {false, {{"error", "tool timed out: " + name}}};
    auto j = nlohmann::json::parse(r.out, nullptr, false);  // guarded
    if (!j.is_object()) return {false, {{"error", "bad tool output"}}};
    return {j.value("ok", false),
            j.contains("result") ? j["result"] : nlohmann::json::object()};
  }
  nlohmann::json content = mcp_call(te, real.empty() ? name : real, args, timeout);
  return {content.is_object() && !content.contains("error"), content};
}

void ToolRunner::on_attach(Blackboard& bb) {
  bb_ = &bb;
  // SAFETY: `this`/bb_ are non-owning. ToolRunner lifetime is managed by the
  // Launcher and outlives this subscription (modules are cleared before the
  // Blackboard is destroyed).
  bb.subscribe("TOOL_REQUEST", [this](const Entry& e) {
    // Guard all external (blackboard) JSON access.
    std::string id, name;
    std::uint64_t epoch = 0;
    bool has_epoch = false;
    nlohmann::json args = nlohmann::json::object();
    if (e.value.is_object()) {
      id = e.value.value("id", "");
      name = e.value.value("tool", "");
      if (e.value.contains("args") && e.value["args"].is_object()) args = e.value["args"];
      if (e.value.contains("epoch") &&
          (e.value["epoch"].is_number_integer() || e.value["epoch"].is_number_unsigned())) {
        epoch = e.value["epoch"].get<std::uint64_t>();
        has_epoch = true;
      }
    }

    // Background opt-in: harness-owned — strip it in ALL paths so the tool binary / MCP
    // server never sees it. Non-bool value = absent (house rule).
    bool background = false;
    if (auto ba = args.find("background"); ba != args.end()) {
      if (ba->is_boolean()) background = ba->get<bool>();
      args.erase(ba);
    }

    // PUMP THREAD ONLY: find_by_tool_name/mcp_real_name lazily warm mutable caches. The
    // worker below receives value copies exclusively.
    const ToolEntry* te = reg_.find_by_tool_name(name);
    if (!te) {
      nlohmann::json out{{"id", id}, {"ok", false},
                         {"content", {{"error", "unknown tool: " + name}}}};
      if (has_epoch) out["epoch"] = epoch;
      bb_->post("TOOL_RESULT", out, "tool_runner", id);
      return;
    }
    // Per-tool override (Tool block timeout_s); 0 -> the runner-wide default.
    const double timeout = te->timeout_s > 0.0 ? te->timeout_s : timeout_s_;
    const ToolEntry entry = *te;                    // value copy: worker independent of registry
    const std::string real = reg_.mcp_real_name(name);

    if (background && executor_) {
      if (bg_running_.size() >= max_background_) {
        nlohmann::json out{
            {"id", id}, {"ok", false},
            {"content",
             {{"error", "too many background tasks (" + std::to_string(bg_running_.size()) +
                            " running, cap " + std::to_string(max_background_) +
                            ") — wait for one to finish"}}}};
        if (has_epoch) out["epoch"] = epoch;
        bb_->post("TOOL_RESULT", out, "tool_runner", id);
        return;
      }
      const std::string task_id = "bg-" + std::to_string(bg_counter_++);
      bg_running_.emplace(task_id, BgRunning{name, std::chrono::steady_clock::now()});
      // Builds need longer than the interactive per-tool cap; the invariant EXEMPTS this
      // timeout (no run_until waits on a background task).
      const double bg_timeout = std::max(timeout, bg_timeout_s_);
      Blackboard* bbg = bb_;
      executor_->submit([bbg, task_id, name, args, entry, real, bg_timeout] {
        auto [ok, content] = execute_tool(entry, name, real, args, bg_timeout);
        // Epoch-FREE by design: completion is cross-turn; the BG_TASKS fold delivers it.
        bbg->post("BG_DONE", {{"task_id", task_id}, {"ok", ok}, {"content", content}},
                  "tool_runner", task_id);
      });
      post_bg_tasks_();   // the running entry is visible to the very next start_turn
      nlohmann::json out{{"id", id}, {"ok", true},
                         {"content",
                          {{"started", true},
                           {"task_id", task_id},
                           {"note", "result will appear in the Background tasks block"}}}};
      if (has_epoch) out["epoch"] = epoch;
      bb_->post("TOOL_RESULT", out, "tool_runner", id);
      return;
    }

    // CONCURRENCY (LLMModule precedent): the closure captures value copies + ONE non-owning
    // pointer (`bb2`); `this` is NOT captured, so the worker can reach no mutable module
    // field. Teardown order joins the Executor before modules/Blackboard die.
    Blackboard* bb2 = bb_;
    auto run = [bb2, id, name, args, entry, real, timeout, epoch, has_epoch] {
      auto [ok, content] = execute_tool(entry, name, real, args, timeout);
      nlohmann::json out{{"id", id}, {"ok", ok}, {"content", content}};
      if (has_epoch) out["epoch"] = epoch;          // echo the turn stamp (absent in -> absent out)
      bb2->post("TOOL_RESULT", out, "tool_runner", id);
    };
    if (executor_) executor_->submit(run);
    else run();                                     // inline (default, unchanged)
  });
  bb.subscribe("BG_DONE", [this](const Entry& e) { on_bg_done_(e); });
}

// Pump-thread only (bus contract): fold a worker's completion into the registry and
// republish the fold block. Unknown/duplicate task_id -> ignore.
void ToolRunner::on_bg_done_(const Entry& e) {
  if (!e.value.is_object()) return;
  auto it = bg_running_.find(e.value.value("task_id", ""));
  if (it == bg_running_.end()) return;
  BgFinished f;
  f.task_id = it->first;
  f.tool = it->second.tool;
  f.ok = e.value.value("ok", false);
  const auto content =
      e.value.contains("content") ? e.value["content"] : nlohmann::json::object();
  f.output = trunc_utf8_bytes(
      content.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), 2000);
  bg_running_.erase(it);
  bg_finished_.push_back(std::move(f));
  while (bg_finished_.size() > 5) bg_finished_.pop_front();   // ring cap
  post_bg_tasks_();
}

void ToolRunner::post_bg_tasks_() {
  std::string block;
  if (!bg_running_.empty() || !bg_finished_.empty()) {
    block = "Background tasks (started earlier with background:true):";
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [tid, r] : bg_running_) {
      const auto secs =
          std::chrono::duration_cast<std::chrono::seconds>(now - r.started).count();
      block += "\n- " + tid + " · " + r.tool + " · running (" +
               std::to_string(secs) + "s)";
    }
    for (const auto& f : bg_finished_)
      block += "\n- " + f.task_id + " · " + f.tool +
               (f.ok ? " · finished ok: " : " · FAILED: ") + f.output;
  }
  bb_->post("BG_TASKS", block, "tool_runner");   // "" when the registry is empty
}

}  // namespace hades

// ── ToolRegistry: manifest tool table + describe/spec warm cache (duplicate split_ws dropped — kept in ToolRunner section above) (was src/tool/registry.cpp) ──────────────
namespace hades {

void ToolRegistry::add_from_block(const Block& b) {
  ToolEntry e;
  e.name = b.name;
  if (b.kv.count("native"))        { e.kind = "native";   e.command = b.kv.at("native"); }
  else if (b.kv.count("mcp"))      { e.kind = "mcp";      e.command = b.kv.at("mcp"); }
  else if (b.kv.count("mcp_url"))  { e.kind = "mcp_http"; e.command = b.kv.at("mcp_url"); }
  else return;                         // unchanged behavior: a block with none is ignored
  if (b.kv.count("api_key_env")) e.api_key_env = b.kv.at("api_key_env");
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
      // MCP discovery: one tools/list exchange per server. Each discovered tool announces as
      // <block>__<name> — the prefix guarantees a server can never shadow a native tool name,
      // and capability_of maps any "__" name to McpTool (mcp_allow-gated). tools/call needs
      // the server's OWN name, kept in mcp_real_names_ (never recovered by string-splitting).
      const double t_timeout = t.timeout_s > 0.0 ? t.timeout_s : timeout_s;
      auto listed = mcp_list(t, t_timeout);
      bool any = false;
      if (listed.is_object() && listed.contains("tools") && listed["tools"].is_array()) {
        for (const auto& disc : listed["tools"]) {
          if (!disc.is_object()) continue;
          const std::string real = disc.value("name", "");
          // Charset gate (same [A-Za-z0-9_-]{1,64} rule as skills + mcp block names): a
          // discovered name with provider-illegal chars ('/', space, ...) would 400 the
          // WHOLE tools array at the LLM API — skip it, keep the server's clean tools.
          if (!valid_skill_name(real)) continue;
          const std::string prefixed = t.name + "__" + real;
          // Dedup by announced name, first-wins across ALL THREE caches: an unconditional
          // spec push with first-wins emplace below would announce DUPLICATE function names
          // (providers 400 the whole tools array) while routing kept only the first.
          if (by_tool_name_.count(prefixed)) continue;
          specs_.push_back({prefixed, disc.value("description", ""),
                            disc.contains("inputSchema") && disc["inputSchema"].is_object()
                                ? disc["inputSchema"]
                                : nlohmann::json::object()});
          by_tool_name_.emplace(prefixed, &t);
          mcp_real_names_.emplace(prefixed, real);
          any = true;
        }
      }
      if (!any) {
        // Fail-soft: discovery failed or returned nothing -> keep the legacy call-by-block-
        // name path so a down server degrades to pre-discovery behavior; boot is never
        // blocked beyond this entry's timeout (stdio: one exchange; http: each of the
        // initialize/request/teardown POSTs is bounded by timeout_s -> worst case ~3x).
        std::fprintf(stderr, "[hades] mcp discovery failed for '%s': %s\n", t.name.c_str(),
                     listed.is_object() && listed.contains("error")
                         ? listed["error"].dump().c_str()
                         : "no tools");
        by_tool_name_.emplace(t.name, &t);
      }
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
  // Background opt-in (harness-owned): every announced tool accepts `background:true` —
  // the ToolRunner strips it before the subprocess/MCP call, so tool binaries and MCP
  // servers never see it. Injected here so native AND discovered-MCP specs get it.
  for (auto& sp : specs_) {
    if (!sp.schema.is_object()) sp.schema = nlohmann::json::object();
    if (!sp.schema.contains("properties") || !sp.schema["properties"].is_object())
      sp.schema["properties"] = nlohmann::json::object();
    sp.schema["properties"]["background"] =
        {{"type", "boolean"},
         {"description",
          "run in background: returns {started, task_id} immediately; the result appears "
          "in the 'Background tasks' block when done (default false)"}};
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

std::string ToolRegistry::mcp_real_name(const std::string& prefixed) const {
  auto it = mcp_real_names_.find(prefixed);
  return it != mcp_real_names_.end() ? it->second : std::string{};
}

}  // namespace hades

// ── MCP client: stdio + Streamable HTTP transports behind mcp_list/mcp_call ──────────────
namespace hades {
namespace {

// One-shot stdio conversation: initialize + initialized-notification + the request (id 2),
// newline-delimited on the spawned server's stdin; reply scanned off stdout.
std::string stdio_conversation(const nlohmann::json& request) {
  std::ostringstream in;
  in << nlohmann::json{{"jsonrpc", "2.0"},
                       {"id", 1},
                       {"method", "initialize"},
                       {"params",
                        {{"protocolVersion", "2024-11-05"},
                         {"capabilities", nlohmann::json::object()},
                         {"clientInfo", {{"name", "hades"}, {"version", "0.1.0"}}}}}}
            .dump()
     << "\n";
  in << nlohmann::json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump()
     << "\n";
  in << request.dump() << "\n";
  return in.str();
}

nlohmann::json stdio_exchange(const std::string& command, const nlohmann::json& request,
                              double timeout_s) {
  if (command.empty()) return {{"error", "mcp: empty command"}};
  auto r = run_subprocess(split_ws(command), stdio_conversation(request), timeout_s);
  if (r.timed_out) return {{"error", "mcp server timed out"}};
  // Scan stdout lines for the JSON-RPC reply to our request (id == 2). Every JSON access is
  // guarded so a malformed server line can never throw.
  std::istringstream out(r.out);
  std::string line;
  while (std::getline(out, line)) {
    if (line.empty()) continue;
    auto j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_object() && j.contains("id") && j["id"] == 2 && j.contains("result"))
      return j["result"];
  }
  return {{"error", "mcp: no result for request"}};
}

// Streamable HTTP transport (defined below, after exchange(), via this forward declaration).
nlohmann::json http_exchange(const ToolEntry& server, const nlohmann::json& request,
                             double timeout_s);

nlohmann::json exchange(const ToolEntry& server, const nlohmann::json& request,
                        double timeout_s) {
  if (server.kind == "mcp_http") return http_exchange(server, request, timeout_s);
  return stdio_exchange(server.command, request, timeout_s);
}

// Extract the id==2 JSON-RPC payload from an HTTP response body that is either plain JSON or
// a one-off SSE stream (a Streamable-HTTP server MAY answer any POST as text/event-stream —
// scan `data:` lines for the object with our id). Guarded parse: malformed lines are skipped.
nlohmann::json parse_http_rpc(const std::string& content_type, const std::string& body) {
  auto pick = [](const nlohmann::json& j) -> nlohmann::json {
    if (j.is_object() && j.contains("id") && j["id"] == 2) {
      if (j.contains("result")) return j["result"];
      if (j.contains("error") && j["error"].is_object())
        return nlohmann::json{
            {"error", "mcp: server error: " + j["error"].value("message", std::string{"unknown"})}};
    }
    return nlohmann::json();   // null = not the reply we asked for
  };
  if (content_type.find("text/event-stream") != std::string::npos) {
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.rfind("data:", 0) != 0) continue;
      std::string payload = line.substr(5);
      if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
      auto r = pick(nlohmann::json::parse(payload, nullptr, false));
      if (!r.is_null()) return r;
    }
    return {{"error", "mcp: no result in event stream"}};
  }
  auto r = pick(nlohmann::json::parse(body, nullptr, false));
  if (!r.is_null()) return r;
  return {{"error", "mcp: malformed http response"}};
}

// Streamable HTTP exchange: initialize (capture Mcp-Session-Id) -> initialized notification
// (best-effort) -> the request -> best-effort DELETE teardown. Bearer comes from the entry's
// api_key_env (env-only, never logged). Redirects OFF (http_fetch precedent). The url is
// OPERATOR-set in the manifest (not LLM-chosen), so no private-net gate applies here.
nlohmann::json http_exchange(const ToolEntry& server, const nlohmann::json& request,
                             double timeout_s) {
  const std::string& url = server.command;
  if (url.empty()) return {{"error", "mcp: empty url"}};
  std::string bearer;
  if (!server.api_key_env.empty())
    if (const char* v = std::getenv(server.api_key_env.c_str()); v && *v) bearer = v;
  auto headers = [&](const std::string& session) {
    cpr::Header h{{"Content-Type", "application/json"},
                  {"Accept", "application/json, text/event-stream"}};
    if (!bearer.empty()) h["Authorization"] = "Bearer " + bearer;
    if (!session.empty()) h["Mcp-Session-Id"] = session;
    return h;
  };
  const cpr::Timeout t{static_cast<long>(timeout_s * 1000)};

  nlohmann::json init{{"jsonrpc", "2.0"},
                      {"id", 1},
                      {"method", "initialize"},
                      {"params",
                       {{"protocolVersion", "2024-11-05"},
                        {"capabilities", nlohmann::json::object()},
                        {"clientInfo", {{"name", "hades"}, {"version", "0.1.0"}}}}}};
  auto r1 = cpr::Post(cpr::Url{url}, headers(""), cpr::Body{init.dump()}, t,
                      cpr::Redirect{false});
  if (r1.error.code != cpr::ErrorCode::OK)
    return {{"error", "mcp: http error: " + r1.error.message}};
  if (r1.status_code >= 400)
    return {{"error", "mcp: http " + std::to_string(r1.status_code) + " on initialize"}};
  std::string session;
  if (auto it = r1.header.find("Mcp-Session-Id"); it != r1.header.end()) session = it->second;

  cpr::Post(cpr::Url{url}, headers(session),
            cpr::Body{nlohmann::json{{"jsonrpc", "2.0"},
                                     {"method", "notifications/initialized"}}.dump()},
            t, cpr::Redirect{false});   // best-effort; spec says 202

  auto r3 = cpr::Post(cpr::Url{url}, headers(session), cpr::Body{request.dump()}, t,
                      cpr::Redirect{false});
  nlohmann::json out;
  if (r3.error.code != cpr::ErrorCode::OK)
    out = {{"error", "mcp: http error: " + r3.error.message}};
  else if (r3.status_code >= 400)
    out = {{"error", "mcp: http " + std::to_string(r3.status_code)}};
  else {
    std::string ct;
    if (auto it = r3.header.find("Content-Type"); it != r3.header.end()) ct = it->second;
    out = parse_http_rpc(ct, r3.text);
  }
  if (!session.empty())
    cpr::Delete(cpr::Url{url}, headers(session), t, cpr::Redirect{false});   // best-effort
  return out;
}

}  // namespace

nlohmann::json mcp_list(const ToolEntry& server, double timeout_s) {
  return exchange(server,
                  nlohmann::json{{"jsonrpc", "2.0"},
                                 {"id", 2},
                                 {"method", "tools/list"},
                                 {"params", nlohmann::json::object()}},
                  timeout_s);
}

nlohmann::json mcp_call(const ToolEntry& server, const std::string& tool,
                        const nlohmann::json& args, double timeout_s) {
  return exchange(server,
                  nlohmann::json{{"jsonrpc", "2.0"},
                                 {"id", 2},
                                 {"method", "tools/call"},
                                 {"params", {{"name", tool}, {"arguments", args}}}},
                  timeout_s);
}

}  // namespace hades
