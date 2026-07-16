// src/apps/llm/llm.cpp — the LLM app: module + OpenAI-compatible provider + cpr transport
//
// Merged (2026-07-04 src reorg): module/llm_module (bus app, executor offload, budget
// metering) + llm/openai_compat_provider (request/response mapping) + llm/http_cpr
// (thin HTTP shell).

#include <cpr/cpr.h>
#include <cstdlib>
#include "hades/module/llm_module.h"
#include "hades/blackboard.h"
#include "hades/executor.h"
#include "hades/launcher.h"   // MalConfig
#include "hades/config.h"
#include "hades/llm/openai_compat_provider.h"
#include "hades/llm/http.h"

// ── LLMModule: LLM_REQUEST -> Provider -> LLM_RESPONSE + budget accrual (was src/module/llm_module.cpp) ──────────────
namespace hades {

LLMModule::LLMModule(std::unique_ptr<Provider> p) : provider_(std::move(p)) {}

void LLMModule::on_start(const Block& cfg, Blackboard&) {
  set_pos_double_on_string(
      cfg.kv.count("price_per_mtok") ? cfg.kv.at("price_per_mtok") : "0",
      price_per_mtok_);
  // Resolve the per-call cpr timeout BEFORE the provider build / api-key check, so an
  // injected-provider test can construct + on_start and assert resolved_llm_timeout_s()
  // without a real endpoint or env var. Default kDefaultLlmTimeoutS; a bad/zero value is
  // ignored by set_pos_double_on_string (keeps the default — same style as price_per_mtok).
  if (cfg.kv.count("llm_timeout_s"))
    set_pos_double_on_string(cfg.kv.at("llm_timeout_s"), llm_timeout_s_);
  if (provider_) return;  // injected (tests)
  std::string ep    = cfg.kv.count("endpoint")    ? cfg.kv.at("endpoint")    : "";
  std::string model = cfg.kv.count("model")       ? cfg.kv.at("model")       : "";
  std::string env   = cfg.kv.count("api_key_env") ? cfg.kv.at("api_key_env") : "HADES_API_KEY";
  const char* key   = std::getenv(env.c_str());
  if (!key) throw MalConfig("LLM api key env var not set: " + env);
  provider_ = std::make_unique<OpenAICompatProvider>(ep, key, model, cpr_http(llm_timeout_s_));
}

void LLMModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  // SAFETY: `this`/bb_ are non-owning. Teardown order (see app/hades_main.cpp):
  // `Blackboard bb` is declared BEFORE `Agent agent`, so the Agent — and every
  // module it owns — destructs FIRST and the Blackboard LAST. This subscription
  // lambda (which captures `this`) therefore OUTLIVES the LLMModule it points at;
  // the captured `this` dangles for the brief window before `bb` is destroyed.
  // That is safe only because nothing pumps the bus during teardown: the
  // Blackboard's dtor destroys the subscription lambdas, it never invokes them,
  // so the dangling `this` is never dereferenced.
  bb.subscribe("LLM_REQUEST", [this](const Entry& e) {
    // Build the LlmRequest synchronously on the pump thread (the only place that
    // reads the Entry / the Blackboard) BEFORE any offload, then capture it by
    // value so it outlives the async task — `e` does not.
    LlmRequest req;
    try {
      req.messages = e.value.value("messages", nlohmann::json::array())
                       .get<std::vector<nlohmann::json>>();
      req.model    = e.value.value("model", std::string{});
      req.epoch    = e.value.value("epoch", static_cast<std::uint64_t>(0));  // bus turn stamp; echoed back
      auto tools_val = e.value.contains("tools") && e.value["tools"].is_array()
                         ? e.value["tools"]
                         : nlohmann::json::array();
      for (const auto& t : tools_val) {
        req.tools.push_back({
            t.value("name", ""),
            t.value("description", ""),
            t.value("schema", nlohmann::json::object())
        });
      }
    } catch (const nlohmann::json::exception&) {
      // Malformed request: proceed with whatever was parsed so far
    }
    // The blocking provider call + result posting. Runs inline on the pump thread
    // by default, or on an Executor worker when one is set. bb->post() is
    // thread-safe, so the worker can post results straight back onto the bus.
    // CONCURRENCY: the closure captures EXACTLY two non-owning pointers by value —
    // `prov` (the Provider) and `bb` (the Blackboard) — and `this` is NOT captured,
    // so the capture list itself proves the worker can reach NO mutable module field
    // (no spent_, no price_per_mtok_). Budget is NOT accrued here; it is accrued by
    // the LLM_RESPONSE handler below, which runs only on the pump thread. So even if
    // two LLM workers ever overlap, spent_ has a single writer (the pump thread) and
    // there is no data race (post-a-delta: the worker just posts LLM_RESPONSE).
    Provider*   prov = provider_.get();   // non-owning; module/Executor teardown order keeps it alive
    Blackboard* bb   = bb_;               // non-owning; Blackboard outlives the joined Executor
    auto run = [prov, bb](const LlmRequest& req) {
      LlmResponse r = prov->complete(req);
      nlohmann::json out{
          {"text",              r.text},
          {"prompt_tokens",     r.prompt_tokens},
          {"completion_tokens", r.completion_tokens},
          {"stop_reason",       r.stop_reason},
          {"epoch",             req.epoch}   // echo the request's turn stamp on BOTH inline + worker paths
      };
      if (r.tool_call) out["tool_call"] = *r.tool_call;
      bb->post("LLM_RESPONSE", out, "llm");   // worker posts ONLY this — touches no module state
    };
    if (executor_) {
      // SAFETY: the worker still reads `prov` (= provider_.get(), owned by this
      // module) and calls `bb`->post(), so a front-end's run_until() observing
      // LLM_RESPONSE does NOT prove this task has returned. The Executor MUST be
      // destroyed (joined) BEFORE this module and the Blackboard — teardown order
      // is load-bearing.
      // (It no longer mutates spent_ — that moved to the pump-thread handler below.)
      // Move req into the task to avoid a redundant copy on submit (run takes
      // it by const&, so no further copy when invoked).
      executor_->submit([req = std::move(req), run]{ run(req); });
    } else {
      run(req);                                      // inline (default, unchanged)
    }
  });

  // Race-free budget accrual (post-a-delta). This handler runs ONLY on the pump
  // thread (bus contract: handlers fire only inside pump()), so spent_ has a
  // single writer regardless of how many LLM workers are in flight — no data
  // race even under overlapping offloaded calls. It reacts to the LLM_RESPONSE
  // the worker (or the inline path) just posted, reads the token counts, accrues
  // the cumulative spend, and posts BUDGET_SPENT_USD. Because LLM_RESPONSE is
  // posted first and this handler reacts to it, the observable order
  // (LLM_RESPONSE then BUDGET_SPENT_USD) is preserved — StayOnBudget still works.
  // ATTACH-ORDER DEPENDENCY: StayOnBudget's correctness relies on THIS LLM_RESPONSE
  // handler running before the Arbiter's within one dispatch — i.e. on the LLMModule
  // being attached BEFORE the Arbiter (as wired in app/agent_wiring.cpp). A future
  // reorder (Arbiter attached first) would lag the budget by one LLM call.
  bb.subscribe("LLM_RESPONSE", [this](const Entry& e) {
    const int p = e.value.value("prompt_tokens", 0);
    const int c = e.value.value("completion_tokens", 0);
    spent_ += (static_cast<double>(p) + c) / 1e6 * price_per_mtok_;
    bb_->post("BUDGET_SPENT_USD", spent_, "llm");
  });

  // Aux-call spend (auto-extract's background reviews): posted as a per-call DELTA by the
  // aux consumer's worker. Folded here — the pump thread stays the single writer of spent_,
  // and BUDGET_SPENT_USD stays the one cumulative number stay_on_budget reads. This closes
  // the "background LLM calls are unmetered" class the embedding path still has (documented).
  bb.subscribe("AUX_SPENT_USD", [this](const Entry& e) {
    if (!e.value.is_number()) return;
    spent_ += e.value.get<double>();
    bb_->post("BUDGET_SPENT_USD", spent_, "llm");
  });
}

}  // namespace hades

// ── OpenAICompatProvider: /chat/completions request+response mapping (was src/llm/openai_compat_provider.cpp) ──────────────
namespace hades {
OpenAICompatProvider::OpenAICompatProvider(std::string e, std::string k, std::string m, HttpClient h)
  : endpoint_(std::move(e)), key_(std::move(k)), model_(std::move(m)), http_(std::move(h)) {}
nlohmann::json OpenAICompatProvider::build_body(const LlmRequest& req) const {
  nlohmann::json b;
  b["model"] = req.model.empty() ? model_ : req.model;
  b["messages"] = req.messages;
  if(!req.tools.empty()){
    b["tools"]=nlohmann::json::array();
    for(const auto& t: req.tools)
      b["tools"].push_back({{"type","function"},
        {"function",{{"name",t.name},{"description",t.description},{"parameters",t.schema}}}});
    b["tool_choice"]="auto";
  }
  return b;
}
LlmResponse OpenAICompatProvider::complete(const LlmRequest& req){
  auto body=build_body(req).dump();
  auto resp=http_(endpoint_+"/chat/completions",
                  {{"Authorization","Bearer "+key_},{"Content-Type","application/json"}}, body);
  LlmResponse out;
  auto j=nlohmann::json::parse(resp.body, nullptr, false);
  if(j.is_discarded() || !j.contains("choices") || !j["choices"].is_array()
     || j["choices"].empty() || !j["choices"][0].contains("message")){
    out.stop_reason="parse_error"; return out;
  }
  const auto& choice=j["choices"][0];
  const auto& msg=choice["message"];
  if(msg.contains("content") && msg["content"].is_string()) out.text=msg["content"];
  if(msg.contains("tool_calls") && msg["tool_calls"].is_array() && !msg["tool_calls"].empty()){
    const auto& tc=msg["tool_calls"][0];
    if(tc.contains("function") && tc["function"].is_object()){
      const auto& fn=tc["function"];
      nlohmann::json args=nlohmann::json::parse(fn.value("arguments","{}"), nullptr, false);
      out.tool_call={{"id",tc.value("id","")},{"name",fn.value("name","")},
                     {"arguments", args.is_discarded()?nlohmann::json::object():args}};
    }
  }
  out.stop_reason=choice.value("finish_reason","");
  if(j.contains("usage") && j["usage"].is_object()){
    out.prompt_tokens=j["usage"].value("prompt_tokens",0);
    out.completion_tokens=j["usage"].value("completion_tokens",0);
  }
  return out;
}
}  // namespace hades

// ── cpr-backed HttpClient factory (was src/llm/http_cpr.cpp) ──────────────
namespace hades {
HttpClient cpr_http(double timeout_s){
  return [timeout_s](const std::string& url,
                     const std::vector<std::pair<std::string,std::string>>& headers,
                     const std::string& body)->HttpResponse {
    cpr::Header h; for(auto& kv: headers) h[kv.first]=kv.second;
    auto r=cpr::Post(cpr::Url{url}, h, cpr::Body{body},
                     cpr::Timeout{static_cast<int32_t>(timeout_s*1000)});
    return {r.status_code, r.text};
  };
}
}  // namespace hades
