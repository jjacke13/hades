// tools/pin_fact_main.cpp — bundled pin_fact native tool binary
//
// Reads one JSON line ({"call":"describe"|"pin_fact","args":{text}}), APPENDS one
// markdown bullet "- <text>" to the always-on core-memory file, and writes one JSON
// line. File path = argv[1] (fallback "memory/facts.md"); the parent dir is created if
// missing. Append-only (never truncate) to the agent's own curated file; NOT confirm-
// gated. Type-guarded: a malformed/adversarial request returns ok:false, never throws.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>

int main(int argc, char** argv) {
  const std::string file = argc > 1 ? argv[1] : "memory/facts.md";
  std::string line;
  std::getline(std::cin, line);
  auto in = nlohmann::json::parse(line, nullptr, false);

  nlohmann::json out;
  std::string call;
  if (in.is_object() && in.contains("call") && in["call"].is_string())
    call = in["call"].get<std::string>();

  if (call == "describe") {
    nlohmann::json required = nlohmann::json::array();
    required.push_back("text");
    out = {{"ok", true},
           {"result",
            {{"name", "pin_fact"},
             {"description",
              "Pin a standing fact to your always-on core memory — kept in your context "
              "every turn. Use for identity/preferences/standing facts you always need; "
              "use save_memory instead for details to recall by keyword later."},
             {"schema",
              {{"type", "object"},
               {"properties", {{"text", {{"type", "string"}}}}},
               {"required", required}}}}}};
  } else if (call == "pin_fact") {
    nlohmann::json args = (in.is_object() && in.contains("args") && in["args"].is_object())
                              ? in["args"]
                              : nlohmann::json::object();
    bool has_text = args.contains("text") && args["text"].is_string();
    if (!has_text) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: text"}}}};
    } else {
      std::string text = args["text"].get<std::string>();
      for (char& ch : text)
        if (ch == '\n' || ch == '\r') ch = ' ';  // one pin = one bullet; no injected multi-line structure
      std::filesystem::path p(file);
      if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);  // best-effort; ofstream reports real failure
      }
      std::ofstream f(file, std::ios::app);  // append-only
      if (!f) {
        out = {{"ok", false}, {"result", {{"error", "cannot append: " + file}}}};
      } else {
        f << "- " << text << "\n";
        out = {{"ok", true}, {"result", {{"pinned", true}}}};
      }
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }

  std::cout << out.dump() << std::endl;
  return 0;
}
