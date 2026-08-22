// tools/save_memory_main.cpp — bundled save_memory native tool binary
//
// Reads one JSON line ({"call":"describe"|"save_memory","args":{text,topic?}}), APPENDS one
// record line {"text","ts","topic"?} to the memory store (topic present only when a
// non-empty one was given — legacy line shape otherwise), and writes one JSON line. Store path =
// argv[1] (fallback ".hades/memory.jsonl"). Append-only to the agent's own store; NOT
// confirm-gated (unlike write_file). Speaks the hades one-JSON-line native tool protocol;
// all stdin parsing is guarded so a malformed request never throws.
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>

int main(int argc, char** argv) {
  const std::string store = argc > 1 ? argv[1] : ".hades/memory.jsonl";
  std::string line;
  std::getline(std::cin, line);
  auto in = nlohmann::json::parse(line, nullptr, false);

  nlohmann::json out;
  std::string call;
  if (in.is_object() && in.contains("call") && in["call"].is_string())
    call = in["call"].get<std::string>();

  if (call == "describe") {
    out = {{"ok", true},
           {"result",
            {{"name", "save_memory"},
             {"description",
              "Persist a fact or observation to long-term memory. Pass `topic` (a short "
              "stable slug) when this fact REPLACES something you saved before — the newest "
              "record for a topic is the one that gets recalled, so the stale value stops "
              "surfacing. Leave `topic` out for standalone observations."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"text", {{"type", "string"}}},
                 {"topic",
                  {{"type", "string"},
                   {"description",
                    "optional supersession key, e.g. \"seat-pref\"; newest record for a "
                    "topic wins"}}}}},
               {"required", {"text"}}}}}}};
  } else if (call == "save_memory") {
    nlohmann::json args = (in.is_object() && in.contains("args") && in["args"].is_object())
                              ? in["args"]
                              : nlohmann::json::object();
    bool has_text = args.contains("text") && args["text"].is_string();
    std::string text = has_text ? args["text"].get<std::string>() : "";
    // Non-string topic fails the WHOLE call (house rule; an empty string counts as absent).
    const bool bad_topic = args.contains("topic") && !args["topic"].is_string();
    std::string topic;
    if (!bad_topic && args.contains("topic")) topic = args["topic"].get<std::string>();
    if (bad_topic) {
      out = {{"ok", false}, {"result", {{"error", "topic must be a string"}}}};
    } else if (!has_text || text.empty()) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: text"}}}};
    } else {
      double ts = std::chrono::duration<double>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
      std::ofstream f(store, std::ios::app);  // append-only
      if (!f) {
        out = {{"ok", false}, {"result", {{"error", "cannot append: " + store}}}};
      } else {
        nlohmann::json rec{{"text", text}, {"ts", ts}};
        if (!topic.empty()) rec["topic"] = topic;   // absent when unused: legacy line shape
        f << rec.dump() << "\n";
        out = {{"ok", true}, {"result", {{"saved", true}}}};
      }
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }

  std::cout << out.dump() << std::endl;
  return 0;
}
