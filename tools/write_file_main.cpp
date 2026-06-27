// tools/write_file_main.cpp — bundled write_file native tool binary
//
// Reads one JSON line ({"call":"describe"|"write_file","args":{path,content}}),
// writes/overwrites the file, and returns one JSON line. Spawned as a subprocess
// by ToolRunner; speaks the hades one-JSON-line native tool protocol. Overwrite is
// destructive — actions are gated upstream by the AvoidDestructive objective. All
// stdin parsing is guarded; a malformed request never throws.
#include <fstream>
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>

int main() {
  std::string line;
  std::getline(std::cin, line);
  auto in = nlohmann::json::parse(line, nullptr, false);

  nlohmann::json out;
  std::string call = in.is_object() ? in.value("call", "") : "";

  if (call == "describe") {
    out = {{"ok", true},
           {"result",
            {{"name", "write_file"},
             {"description", "Write text to a file, creating it or overwriting it."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"path", {{"type", "string"}}}, {"content", {{"type", "string"}}}}},
               {"required", {"path", "content"}}}}}}};
  } else if (call == "write_file") {
    nlohmann::json args =
        (in.is_object() && in.contains("args") && in["args"].is_object())
            ? in["args"]
            : nlohmann::json::object();
    std::string path = args.value("path", "");
    std::string content = args.value("content", "");
    if (path.empty()) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: path"}}}};
    } else {
      std::ofstream f(path, std::ios::binary | std::ios::trunc);
      if (!f) {
        out = {{"ok", false}, {"result", {{"error", "cannot write: " + path}}}};
      } else {
        f << content;
        out = {{"ok", true},
               {"result",
                {{"path", path}, {"bytes_written", static_cast<int>(content.size())}}}};
      }
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }

  std::cout << out.dump() << std::endl;
  return 0;
}
