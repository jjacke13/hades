// hades-fs-read: a native tool following the hades native tool protocol.
//
// Reads ONE JSON line on stdin: {"call": <name|"describe">, "args": {...}}
// Writes ONE JSON line on stdout:
//   describe -> {"ok":true,"result":{"name","description","schema"}}
//   call ok  -> {"ok":true,"result":{...}}
//   call err -> {"ok":false,"result":{"error":"..."}}
//
// All external input (the stdin line, its `args`) is guarded so a malformed
// request can never throw — we always emit a single well-formed JSON line.
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
