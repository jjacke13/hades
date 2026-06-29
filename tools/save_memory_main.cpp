// tools/save_memory_main.cpp — bundled save_memory native tool binary
//
// Reads one JSON line ({"call":"describe"|"save_memory","args":{text}}), APPENDS one
// record line {"text","ts"} to the memory store, and writes one JSON line. Store path =
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
  std::string call = in.is_object() ? in.value("call", "") : "";

  if (call == "describe") {
    out = {{"ok", true},
           {"result",
            {{"name", "save_memory"},
             {"description", "Persist a fact or observation to long-term memory."},
             {"schema",
              {{"type", "object"},
               {"properties", {{"text", {{"type", "string"}}}}},
               {"required", {"text"}}}}}}};
  } else if (call == "save_memory") {
    nlohmann::json args = (in.is_object() && in.contains("args") && in["args"].is_object())
                              ? in["args"]
                              : nlohmann::json::object();
    std::string text = args.value("text", "");
    if (text.empty()) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: text"}}}};
    } else {
      double ts = std::chrono::duration<double>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
      std::ofstream f(store, std::ios::app);  // append-only
      if (!f) {
        out = {{"ok", false}, {"result", {{"error", "cannot append: " + store}}}};
      } else {
        f << nlohmann::json{{"text", text}, {"ts", ts}}.dump() << "\n";
        out = {{"ok", true}, {"result", {{"saved", true}}}};
      }
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }

  std::cout << out.dump() << std::endl;
  return 0;
}
