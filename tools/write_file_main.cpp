// tools/write_file_main.cpp — bundled write_file native tool binary
//
// Reads one JSON line ({"call":"describe"|"write_file","args":{path,content}}),
// writes/overwrites the file ATOMICALLY (temp file + rename, mode preserved — the
// edit_file pattern), and returns one JSON line. Spawned as a subprocess by
// ToolRunner; speaks the hades one-JSON-line native tool protocol. Overwrite is
// destructive — actions are gated upstream by the AvoidDestructive objective. All
// stdin parsing is guarded; a malformed request never throws. An optional
// expect_version arg (Arbiter-injected, never LLM-supplied; absent from the describe
// schema) gates the write against a staleness check — see the file_version header.
#include <sys/stat.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/tool/file_version.h"

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
    std::string expect = args.contains("expect_version") && args["expect_version"].is_string()
                              ? args["expect_version"].get<std::string>() : std::string{};
    if (path.empty()) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: path"}}}};
    } else if (!expect.empty()) {
      // Staleness guard (Arbiter-injected): verify the file still matches what the conversation
      // last observed. Unreadable/deleted counts as changed — the observed file is gone.
      std::ifstream cur(path, std::ios::binary);
      std::stringstream cs;
      if (cur) cs << cur.rdbuf();
      if (!cur || hades::file_version(cs.str()) != expect) {
        out = {{"ok", false},
               {"result", {{"error",
                 "file changed on disk since you last read it — fs_read it again and retry"}}}};
      }
    }
    if (out.is_null()) {
      // Atomic write (tmp + rename, edit_file pattern): a crash never leaves a torn file, and a
      // refusal above never touched it. Preserve an existing file's mode across the rename.
      const std::string tmp = path + ".tmp";
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      if (f) { f << content; f.close(); }
      if (!f) {
        std::remove(tmp.c_str());
        out = {{"ok", false}, {"result", {{"error", "cannot write: " + path}}}};
      } else {
        struct stat st{};
        if (::stat(path.c_str(), &st) == 0) ::chmod(tmp.c_str(), st.st_mode);
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
          std::remove(tmp.c_str());
          out = {{"ok", false}, {"result", {{"error", "cannot save: " + path}}}};
        } else {
          out = {{"ok", true},
                 {"result", {{"path", path}, {"bytes_written", static_cast<int>(content.size())},
                             {"version", hades::file_version(content)}}}};
        }
      }
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }

  std::cout << out.dump() << std::endl;
  return 0;
}
