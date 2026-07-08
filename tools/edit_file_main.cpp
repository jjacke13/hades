// tools/edit_file_main.cpp — bundled edit_file native tool binary
//
// Reads one JSON line ({"call":"describe"|"edit_file","args":{path,old_string,new_string,
// replace_all}}), performs an exact-substring replacement in the file, and writes the result
// ATOMICALLY (temp file + rename — a concurrent reader never sees a torn file). The surgical
// alternative to whole-file write_file: old_string must match EXACTLY ONCE unless replace_all.
// Capability-gated upstream as FsWrite (fs_write_allow scope / confirm). Staleness-guarded: an
// Arbiter-injected expect_version (absent from the describe schema) is checked against the file's
// current content hash before any change — mismatch -> refuse, file untouched. Fail-closed: any
// malformed input, missing file, zero or ambiguous match -> ok:false, file untouched.
// If path is a symlink, the rename REPLACES THE SYMLINK with a regular file (target untouched)
// — lexical-path v1 semantics, matching the capability model's documented symlink gap.
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
  std::string call;
  if (in.is_object() && in.contains("call") && in["call"].is_string())
    call = in["call"].get<std::string>();

  if (call == "describe") {
    out = {{"ok", true},
           {"result",
            {{"name", "edit_file"},
             {"description",
              "Surgically edit a file: replace old_string with new_string. old_string must "
              "match EXACTLY ONCE (give more surrounding context if ambiguous) unless "
              "replace_all is true. Atomic write."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"path", {{"type", "string"}}},
                 {"old_string", {{"type", "string"}}},
                 {"new_string", {{"type", "string"}}},
                 {"replace_all", {{"type", "boolean"}}}}},
               {"required", {"path", "old_string", "new_string"}}}}}}};
  } else if (call == "edit_file") {
    nlohmann::json args = (in.is_object() && in.contains("args") && in["args"].is_object())
                              ? in["args"] : nlohmann::json::object();
    auto jstr = [&](const char* k) {
      auto it = args.find(k);
      return (it != args.end() && it->is_string()) ? it->get<std::string>() : std::string{};
    };
    auto jbool = [&](const char* k, bool dflt) {
      auto it = args.find(k);
      return (it != args.end() && it->is_boolean()) ? it->get<bool>() : dflt;
    };
    const std::string path = jstr("path");
    const std::string olds = jstr("old_string");
    const std::string news = jstr("new_string");
    const bool replace_all = jbool("replace_all", false);
    std::ifstream f(path, std::ios::binary);
    if (path.empty() || olds.empty()) {
      out = {{"ok", false}, {"result", {{"error", "path and a non-empty old_string are required"}}}};
    } else if (olds == news) {
      out = {{"ok", false}, {"result", {{"error", "old_string and new_string are identical"}}}};
    } else if (!f) {
      out = {{"ok", false}, {"result", {{"error", "cannot read: " + path}}}};
    } else {
      std::stringstream ss;
      ss << f.rdbuf();
      std::string content = ss.str();
      f.close();
      // Staleness guard: expect_version is Arbiter-injected (never LLM-supplied; absent from the
      // describe schema). It is the hash of the file as the conversation last observed it — a
      // mismatch means someone else changed the file since; refuse so nothing is clobbered.
      const std::string expect = jstr("expect_version");
      if (!expect.empty() && hades::file_version(content) != expect) {
        out = {{"ok", false},
               {"result", {{"error",
                 "file changed on disk since you last read it — fs_read it again and retry"}}}};
      } else {
        // Count non-overlapping occurrences.
        int count = 0;
        for (std::size_t pos = content.find(olds); pos != std::string::npos;
             pos = content.find(olds, pos + olds.size()))
          ++count;
        if (count == 0) {
          out = {{"ok", false}, {"result", {{"error", "old_string not found in " + path}}}};
        } else if (count > 1 && !replace_all) {
          out = {{"ok", false},
                 {"result",
                  {{"error", "old_string matches " + std::to_string(count) +
                                 " times — give more surrounding context or set replace_all"}}}};
        } else {
          int done = 0;
          std::size_t pos = 0;
          while ((pos = content.find(olds, pos)) != std::string::npos) {
            content.replace(pos, olds.size(), news);
            pos += news.size();
            ++done;
            if (!replace_all) break;
          }
          const std::string tmp = path + ".tmp";
          std::ofstream o(tmp, std::ios::trunc | std::ios::binary);
          if (o) { o << content; o.close(); }
          std::error_code ec;
          if (!o) {
            std::remove(tmp.c_str());
            out = {{"ok", false}, {"result", {{"error", "cannot write: " + path}}}};
          } else {
            // Preserve the original file's mode: the tmp was created with the umask default, and
            // rename would otherwise silently drop exec bits / tighten-or-loosen permissions.
            struct stat st{};
            if (::stat(path.c_str(), &st) == 0) ::chmod(tmp.c_str(), st.st_mode);
            std::filesystem::rename(tmp, path, ec);
            if (ec) {
              std::remove(tmp.c_str());
              out = {{"ok", false}, {"result", {{"error", "cannot save: " + path}}}};
            } else {
              out = {{"ok", true},
                     {"result", {{"path", path}, {"replacements", done},
                                 {"version", hades::file_version(content)}}}};
            }
          }
        }
      }
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }
  std::cout << out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << std::endl;
  return 0;
}
