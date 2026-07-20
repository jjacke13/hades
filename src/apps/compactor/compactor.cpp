// src/apps/compactor/compactor.cpp — CompactorModule (see the header)
#include "hades/module/compactor_module.h"
#include <cstdlib>
#include <string>
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/executor.h"
#include "hades/launcher.h"              // MalConfig
#include "hades/llm/http.h"
#include "hades/llm/openai_compat_provider.h"
#include "hades/module/tool_runner.h"    // trunc_utf8_bytes
namespace hades {
namespace {
constexpr const char* kSystemPrompt =
    "You maintain a rolling summary of an ongoing conversation between a user and their AI "
    "assistant. Merge the existing summary with the newly dropped turns into ONE updated "
    "summary. Keep: decisions made, open tasks, user preferences and corrections, key facts, "
    "and file/tool state. Drop: pleasantries, superseded attempts, and tool noise. Reply with "
    "ONLY the updated summary text.";

// The merge input: prior summary + the span digest, flattened to plain text.
std::string build_merge_input(const std::string& current, const nlohmann::json& span,
                              std::size_t char_limit) {
  std::string in = "Existing summary (may be empty):\n" + current + "\n\nNewly dropped turns:\n";
  for (const auto& m : span) {
    if (!m.is_object()) continue;
    in += m.value("role", "") + ": " + m.value("content", "") + "\n";
  }
  in += "\nUpdated summary (under " + std::to_string(char_limit) + " characters):";
  return in;
}
}  // namespace

void CompactorModule::on_start(const Block& cfg, Blackboard&) {
  if (cfg.kv.count("model")) model_ = cfg.kv.at("model");
  if (cfg.kv.count("price_per_mtok"))
    set_pos_double_on_string(cfg.kv.at("price_per_mtok"), price_per_mtok_);
  if (cfg.kv.count("summary_char_limit")) {
    try {
      const long n = std::stol(cfg.kv.at("summary_char_limit"));
      if (n > 0) summary_char_limit_ = static_cast<std::size_t>(n);
    } catch (...) { /* keep default */ }
  }
  if (provider_) return;  // injected (tests)
  double timeout_s = 60.0;
  if (cfg.kv.count("timeout_s")) set_pos_double_on_string(cfg.kv.at("timeout_s"), timeout_s);
  const std::string ep  = cfg.kv.count("endpoint") ? cfg.kv.at("endpoint") : "";
  const std::string env = cfg.kv.count("api_key_env") ? cfg.kv.at("api_key_env") : "HADES_API_KEY";
  const char* key = std::getenv(env.c_str());
  if (!key) throw MalConfig("compactor: api key env var not set: " + env);
  provider_ = std::make_unique<OpenAICompatProvider>(ep, key, model_, cpr_http(timeout_s));
}

void CompactorModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  bb.subscribe("COMPACT_REQUEST", [this](const Entry& e) {
    // Gate on the pump thread; the summarize itself runs on a worker (inline w/o executor).
    const auto& v = e.value;
    if (!v.is_object()) return;
    const std::string session = v.value("session", "");
    const std::size_t upto =
        static_cast<std::size_t>(v.value("upto", static_cast<std::uint64_t>(0)));
    if (upto == 0) return;
    if (!v.contains("span") || !v["span"].is_array() || v["span"].empty()) return;
    if (busy_.exchange(true)) return;   // one in flight (the Arbiter never double-sends)
    // Capture discipline (LLMModule/auto-extract precedent): non-owning provider/bus
    // pointers + plain values + the atomic busy flag. No pump-mutated field off-thread.
    Provider* prov = provider_.get();
    Blackboard* bus = bb_;
    std::atomic<bool>* busy = &busy_;
    const double price = price_per_mtok_;
    const std::size_t limit = summary_char_limit_;
    LlmRequest req;
    req.model = model_;
    req.messages = {
        nlohmann::json{{"role", "system"}, {"content", kSystemPrompt}},
        nlohmann::json{{"role", "user"},
                       {"content", build_merge_input(v.value("current_summary", ""),
                                                     v["span"], limit)}}};
    // ALWAYS-terminal worker (tool-offload BG_DONE lesson): success -> SESSION_SUMMARY,
    // any throw/empty reply -> COMPACT_FAILED. The Arbiter's pending flag depends on it.
    auto run = [prov, bus, busy, session, upto, limit, price](const LlmRequest& r) {
      bool ok = false;
      std::string text;
      try {
        const LlmResponse resp = prov->complete(r);
        text = trunc_utf8_bytes(resp.text, limit);
        ok = !text.empty();
        const double delta =
            (static_cast<double>(resp.prompt_tokens) + resp.completion_tokens) / 1e6 * price;
        if (delta > 0.0) bus->post("AUX_SPENT_USD", delta, "compactor");
      } catch (...) {
        ok = false;
      }
      if (ok)
        bus->post("SESSION_SUMMARY",
                  {{"session", session}, {"upto", static_cast<std::uint64_t>(upto)},
                   {"text", text}},
                  "compactor");
      else
        bus->post("COMPACT_FAILED",
                  {{"session", session}, {"upto", static_cast<std::uint64_t>(upto)}},
                  "compactor");
      busy->store(false);
    };
    if (executor_) {
      // Enqueue-throw would leak busy_=true AND leave the Arbiter pending — post the
      // terminal failure inline (we are on the pump thread; post() is safe).
      try {
        executor_->submit([req = std::move(req), run] { run(req); });
      } catch (...) {
        busy_.store(false);
        bb_->post("COMPACT_FAILED",
                  {{"session", session}, {"upto", static_cast<std::uint64_t>(upto)}},
                  "compactor");
      }
    } else {
      run(req);
    }
  });
}
}  // namespace hades
