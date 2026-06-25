#include "hades/module/llm_module.h"
#include "hades/blackboard.h"
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
  });
}

}  // namespace hades
