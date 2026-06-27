// tools/fs_read_main.cpp — bundled fs_read native tool binary
//
// Reads one JSON line from stdin ({"call":"describe"|"fs_read","args":{...}}),
// handles describe (returns name/description/schema) or fs_read (opens a UTF-8
// file and returns its content), then writes one JSON line to stdout. Launched
// as a subprocess by ToolRunner via ToolRegistry; speaks the hades one-JSON-line
// native tool protocol. All stdin parsing is guarded — a malformed request never throws.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>

int main() {
  std::string line;
  std::getline(std::cin, line);
  auto in = nlohmann::json::parse(line, nullptr, false);  // false => no throw

  nlohmann::json out;
  std::string call = in.is_object() ? in.value("call", "") : "";

  if (call == "describe") {
    out = {{"ok", true},
           {"result",
            {{"name", "fs_read"},
             {"description", "Read a UTF-8 text file and return its contents."},
             {"schema",
              {{"type", "object"},
               {"properties", {{"path", {{"type", "string"}}}}},
               {"required", {"path"}}}}}}};
  } else if (call == "fs_read") {
    nlohmann::json args =
        (in.is_object() && in.contains("args") && in["args"].is_object())
            ? in["args"]
            : nlohmann::json::object();
    std::string path = args.value("path", "");
    if (path.empty()) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: path"}}}};
    } else {
      std::ifstream f(path);
      if (!f) {
        out = {{"ok", false}, {"result", {{"error", "cannot open: " + path}}}};
      } else {
        std::stringstream s;
        s << f.rdbuf();
        out = {{"ok", true}, {"result", {{"content", s.str()}}}};
      }
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }

  std::cout << out.dump() << std::endl;
  return 0;
}
