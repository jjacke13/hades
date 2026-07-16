// src/apps/auto_extract/auto_extract.cpp — AutoExtractModule (see the header)
#include "hades/module/auto_extract_module.h"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/executor.h"
#include "hades/extract/extract.h"
#include "hades/launcher.h"  // MalConfig
#include "hades/llm/http.h"
#include "hades/llm/openai_compat_provider.h"
namespace hades {
namespace {
constexpr const char* kSystemPrompt =
    "You review one exchange between a user and their AI assistant. Extract durable facts "
    "worth remembering across sessions: user preferences, corrections the user made, and "
    "standing facts about the user or their environment. Ignore small talk, one-off task "
    "detail, and anything already obvious. Reply with ONLY a JSON array of short "
    "self-contained strings, or NONE if nothing qualifies.";

// build_extract_digest (T1) byte-truncates each side to this cap; mirrored here so we can cut on
// the same boundary and strip a split-codepoint tail BEFORE the digest reaches the request.
constexpr std::size_t kDigestSideCap = 2000;

// Truncate to at most kDigestSideCap bytes, then drop a trailing INCOMPLETE UTF-8 sequence so
// the result is always valid UTF-8. Cross-task Important (T1 review): the aux digest becomes an
// LlmRequest message content the real OpenAICompatProvider serialises with a STRICT dump() —
// which throws on invalid UTF-8, so a mid-codepoint cut would kill every aux call silently.
// (The sanitizer lives here, not in extract.cpp — T1 is reviewed/closed.)
std::string trunc_utf8(std::string s) {
  if (s.size() > kDigestSideCap) s.resize(kDigestSideCap);
  // Walk back over continuation bytes (10xxxxxx), at most 3 (UTF-8 is <=4 bytes/codepoint).
  std::size_t cont = 0;
  while (cont < 3 && cont < s.size() &&
         (static_cast<unsigned char>(s[s.size() - 1 - cont]) & 0xC0) == 0x80)
    ++cont;
  if (cont >= s.size()) return s;  // no lead byte reached (unreachable for valid input) — leave
  const unsigned char lead = static_cast<unsigned char>(s[s.size() - 1 - cont]);
  std::size_t need;                              // bytes the lead byte announces
  if      ((lead & 0x80) == 0x00) need = 1;      // 0xxxxxxx  ASCII
  else if ((lead & 0xE0) == 0xC0) need = 2;      // 110xxxxx
  else if ((lead & 0xF0) == 0xE0) need = 3;      // 1110xxxx
  else if ((lead & 0xF8) == 0xF0) need = 4;      // 11110xxx
  else                            need = 1;      // stray continuation as lead — treat as complete
  if (1 + cont < need) s.resize(s.size() - 1 - cont);  // incomplete tail — drop lead + its conts
  return s;
}

// Append accepted facts (exact-dup-skipped against the store) and return the count written.
// Runs on the WORKER; touches only its arguments. Same line-append discipline as the
// save_memory tool; the tolerant loaders skip a hypothetically torn line (spec-accepted).
std::size_t append_facts(const std::string& store, const std::vector<std::string>& facts) {
  std::set<std::string> existing;
  {
    std::ifstream f(store);
    std::string l;
    while (std::getline(f, l)) {
      auto j = nlohmann::json::parse(l, nullptr, false);
      if (j.is_object() && j.contains("text") && j["text"].is_string())
        existing.insert(j["text"].get<std::string>());
    }
  }
  std::ofstream f(store, std::ios::app);
  if (!f) return 0;
  std::size_t n = 0;
  const double ts = std::chrono::duration<double>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
  for (const auto& t : facts) {
    if (existing.count(t)) continue;
    // UTF-8-replace dump (house pattern): a fact is byte-capped at 500 (T1) and can split a
    // codepoint; a strict dump() would THROW on the tail and drop the whole worker's writes.
    f << nlohmann::json{{"text", t}, {"ts", ts}, {"src", "auto"}}.dump(
             -1, ' ', false, nlohmann::json::error_handler_t::replace)
      << "\n";
    ++n;
  }
  return n;
}
}  // namespace

void AutoExtractModule::on_start(const Block& cfg, Blackboard&) {
  if (cfg.kv.count("store") && !cfg.kv.at("store").empty()) store_ = cfg.kv.at("store");
  if (cfg.kv.count("model")) model_ = cfg.kv.at("model");
  if (cfg.kv.count("price_per_mtok"))
    set_pos_double_on_string(cfg.kv.at("price_per_mtok"), price_per_mtok_);
  if (cfg.kv.count("max_facts")) {
    try {
      const long n = std::stol(cfg.kv.at("max_facts"));
      if (n > 0) max_facts_ = static_cast<std::size_t>(n);
    } catch (...) { /* keep default */ }
  }
  if (provider_) return;  // injected (tests)
  double timeout_s = 60.0;
  if (cfg.kv.count("timeout_s")) set_pos_double_on_string(cfg.kv.at("timeout_s"), timeout_s);
  const std::string ep  = cfg.kv.count("endpoint") ? cfg.kv.at("endpoint") : "";
  const std::string env = cfg.kv.count("api_key_env") ? cfg.kv.at("api_key_env") : "HADES_API_KEY";
  const char* key = std::getenv(env.c_str());
  if (!key) throw MalConfig("auto_extract: api key env var not set: " + env);
  provider_ = std::make_unique<OpenAICompatProvider>(ep, key, model_, cpr_http(timeout_s));
}

void AutoExtractModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  bb.subscribe("TURN_ORIGIN", [this](const Entry& e) {
    if (e.value.is_string()) origin_ = e.value.get<std::string>();
  });
  bb.subscribe("USER_MESSAGE", [this](const Entry& e) {
    if (e.value.is_string()) last_user_ = e.value.get<std::string>();
  });
  bb.subscribe("ASSISTANT_MESSAGE", [this](const Entry& e) {
    // Gate on the pump thread; the review itself runs on a worker (or inline w/o executor).
    if (!e.value.is_string()) return;
    const std::string assistant = e.value.get<std::string>();
    if (origin_ != "human") return;                       // never harvest peer/heartbeat turns
    if (last_user_.empty() || assistant.empty()) return;
    if (is_turn_artifact(assistant)) return;
    if (busy_.exchange(true)) return;                     // one review in flight; skip, not queue
    // Capture discipline (LLMModule precedent): non-owning provider/bus pointers + plain
    // values. The worker's only `this`-reachable write is the ATOMIC busy_ (via `busy`);
    // no pump-mutated field (origin_/last_user_/config) is read or written off-thread.
    Provider* prov = provider_.get();
    Blackboard* bus = bb_;
    std::atomic<bool>* busy = &busy_;
    const std::string store = store_;
    const std::string model = model_;
    const double price = price_per_mtok_;
    const std::size_t max_facts = max_facts_;
    LlmRequest req;
    req.model = model;
    // Sanitize both digest sides to a valid UTF-8 boundary BEFORE build_extract_digest so the
    // request the provider strict-dumps is always valid UTF-8 (see trunc_utf8 above).
    req.messages = {nlohmann::json{{"role", "system"}, {"content", kSystemPrompt}},
                    nlohmann::json{{"role", "user"},
                                   {"content", build_extract_digest(trunc_utf8(last_user_),
                                                                    trunc_utf8(assistant))}}};
    auto run = [prov, bus, busy, store, price, max_facts](const LlmRequest& r) {
      try {
        const LlmResponse resp = prov->complete(r);
        const auto facts = parse_extract_reply(resp.text, max_facts);
        if (!facts.empty()) append_facts(store, facts);
        const double delta =
            (static_cast<double>(resp.prompt_tokens) + resp.completion_tokens) / 1e6 * price;
        if (delta > 0.0) bus->post("AUX_SPENT_USD", delta, "auto_extract");
      } catch (...) { /* fail-soft: an extractor error never touches a turn */ }
      busy->store(false);
    };
    if (executor_) {
      // If the enqueue itself fails (bad_alloc building the std::function, or the executor
      // already stopping), the task never runs and busy_ would leak true — permanently
      // killing extraction for the process. Clear it and drop this one review instead.
      try {
        executor_->submit([req = std::move(req), run] { run(req); });
      } catch (...) {
        busy_.store(false);
      }
    } else {
      run(req);
    }
  });
}
}  // namespace hades
