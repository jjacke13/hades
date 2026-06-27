// tools/list_dir_main.cpp — bundled list_dir native tool binary
//
// Reads one JSON line ({"call":"describe"|"list_dir","args":{path}}), lists the
// directory's entries (name, type, size) and returns one JSON line. Spawned as a
// subprocess by ToolRunner; speaks the hades one-JSON-line native tool protocol.
// Read-only and guarded; a malformed request or unreadable dir never throws.
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <nlohmann/json.hpp>
namespace fs = std::filesystem;

int main() {
  std::string line;
  std::getline(std::cin, line);
  auto in = nlohmann::json::parse(line, nullptr, false);

  nlohmann::json out;
  std::string call = in.is_object() ? in.value("call", "") : "";

  if (call == "describe") {
    out = {{"ok", true},
           {"result",
            {{"name", "list_dir"},
             {"description", "List the entries (name, type, size) in a directory."},
             {"schema",
              {{"type", "object"},
               {"properties", {{"path", {{"type", "string"}}}}},
               {"required", {"path"}}}}}}};
  } else if (call == "list_dir") {
    nlohmann::json args =
        (in.is_object() && in.contains("args") && in["args"].is_object())
            ? in["args"]
            : nlohmann::json::object();
    std::string path = args.value("path", "");
    std::error_code ec;
    if (path.empty()) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: path"}}}};
    } else if (!fs::is_directory(path, ec)) {
      out = {{"ok", false}, {"result", {{"error", "not a directory: " + path}}}};
    } else {
      nlohmann::json entries = nlohmann::json::array();
      for (const auto& e : fs::directory_iterator(path, ec)) {
        std::error_code se;
        bool dir = e.is_directory(se);
        long long size = dir ? 0 : static_cast<long long>(fs::file_size(e.path(), se));
        entries.push_back({{"name", e.path().filename().string()},
                           {"type", dir ? "dir" : "file"},
                           {"size", size}});
      }
      out = {{"ok", true}, {"result", {{"path", path}, {"entries", entries}}}};
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }

  std::cout << out.dump() << std::endl;
  return 0;
}
