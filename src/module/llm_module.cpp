// src/module/llm_module.cpp — Blackboard bridge from LLM_REQUEST to Provider
//
// Subscribes to LLM_REQUEST on the Blackboard; on each entry unmarshals the
// messages/tools JSON, calls Provider::complete() (OpenAICompatProvider by
// default, injected in tests), and posts LLM_RESPONSE plus a cumulative
// BUDGET_SPENT_USD back to the Blackboard. Reads endpoint/model/api_key_env
// from the Manifest block in on_start(); throws MalConfig if the API key env
// var is absent.

#include "hades/module/llm_module.h"
#include "hades/blackboard.h"
#include "hades/executor.h"
#include "hades/launcher.h"   // MalConfig
#include "hades/config.h"
#include "hades/llm/openai_compat_provider.h"
#include "hades/llm/http.h"
#include <cstdlib>
namespace hades {

LLMModule::LLMModule(std::unique_ptr<Provider> p) : provider_(std::move(p)) {}

void LLMModule::on_start(const Block& cfg, Blackboard&) {
  set_pos_double_on_string(
      cfg.kv.count("price_per_mtok") ? cfg.kv.at("price_per_mtok") : "0",
      price_per_mtok_);
  if (provider_) return;  // injected (tests)
  std::string ep    = cfg.kv.count("endpoint")    ? cfg.kv.at("endpoint")    : "";
  std::string model = cfg.kv.count("model")       ? cfg.kv.at("model")       : "";
  std::string env   = cfg.kv.count("api_key_env") ? cfg.kv.at("api_key_env") : "HADES_API_KEY";
  const char* key   = std::getenv(env.c_str());
  if (!key) throw MalConfig("LLM api key env var not set: " + env);
  provider_ = std::make_unique<OpenAICompatProvider>(ep, key, model, cpr_http());
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
}

}  // namespace hades
