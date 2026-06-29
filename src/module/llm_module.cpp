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
  // SAFETY: `this`/bb_ are non-owning. LLMModule lifetime is managed by Launcher
  // and outlives the Blackboard's subscription (modules are cleared before the
  // Blackboard is destroyed).
  bb.subscribe("LLM_REQUEST", [this](const Entry& e) {
    // Build the LlmRequest synchronously on the pump thread (the only place that
    // reads the Entry / the Blackboard) BEFORE any offload, then capture it by
    // value so it outlives the async task — `e` does not.
    LlmRequest req;
    try {
      req.messages = e.value.value("messages", nlohmann::json::array())
                       .get<std::vector<nlohmann::json>>();
      req.model    = e.value.value("model", std::string{});
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
    // by default, or on an Executor worker when one is set. bb_->post() is
    // thread-safe, so the worker can post results straight back onto the bus.
    // CONCURRENCY: spent_ is mutated here (on the worker when offloaded). v1 has
    // at most ONE in-flight LLM call per agent (turns are serial), so there is no
    // data race. If overlapping LLM calls are ever introduced, spent_ must become
    // an atomic or switch to a post-a-delta scheme.
    auto run = [this](const LlmRequest& req) {
      LlmResponse r = provider_->complete(req);
      nlohmann::json out{
          {"text",              r.text},
          {"prompt_tokens",     r.prompt_tokens},
          {"completion_tokens", r.completion_tokens},
          {"stop_reason",       r.stop_reason}
      };
      if (r.tool_call) out["tool_call"] = *r.tool_call;
      bb_->post("LLM_RESPONSE", out, "llm");
      spent_ += (static_cast<double>(r.prompt_tokens) + r.completion_tokens) / 1e6 * price_per_mtok_;
      bb_->post("BUDGET_SPENT_USD", spent_, "llm");
    };
    if (executor_) {
      // SAFETY: the worker mutates spent_ and touches bb_ AFTER posting
      // LLM_RESPONSE, so a front-end's run_until() observing LLM_RESPONSE does
      // NOT prove this task has returned. The Executor MUST be destroyed (joined)
      // BEFORE this module and the Blackboard — teardown order is load-bearing.
      // Move req into the task to avoid a redundant copy on submit (run takes
      // it by const&, so no further copy when invoked).
      executor_->submit([req = std::move(req), run]{ run(req); });
    } else {
      run(req);                                      // inline (default, unchanged)
    }
  });
}

}  // namespace hades
