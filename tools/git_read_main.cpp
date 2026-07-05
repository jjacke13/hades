// tools/git_read_main.cpp — bundled git_read native tool binary (read-only git introspection)
//
// Reads one JSON line ({"call":"describe"|"git_read","args":{op,path,staged,max_lines}}) and
// runs `git` with a FIXED argv per op — status / diff / log only, never a shell, never a write
// op. Security: a path beginning with '-' is REJECTED (flag injection) and every pathspec is
// preceded by a literal "--". Output (stdout, or stderr on failure) is line- and byte-capped.
// Runs in the agent's cwd. Capability: GitRead -> allow (read-only by construction).
// V1 GAP (documented): git_read is capability-allow UNCONDITIONALLY — a git-TRACKED and MODIFIED
// file listed in fs_deny still surfaces its diff content here (fs_deny gates fs_read paths, not
// git-surfaced content). Keep real secrets gitignored.
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"

namespace {
// Cap a blob to n lines + 64 KB; reports whether anything was dropped.
std::string cap_lines(const std::string& s, long long n, bool& truncated) {
  std::istringstream is(s);
  std::string line, out;
  long long count = 0;
  while (std::getline(is, line)) {
    if (count >= n || out.size() >= 64 * 1024) { truncated = true; break; }
    out += line;
    out += '\n';
    ++count;
  }
  return out;
}
}  // namespace

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
            {{"name", "git_read"},
             {"description",
              "Read-only git introspection in the current repo: op=status (porcelain+branch), "
              "op=diff (optionally staged, optionally limited to path), op=log (--oneline). "
              "No write operations."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"op", {{"type", "string"}, {"enum", {"status", "diff", "log"}}}},
                 {"path", {{"type", "string"}}},
                 {"staged", {{"type", "boolean"}}},
                 {"max_lines", {{"type", "integer"}}}}},
               {"required", {"op"}}}}}}};
  } else if (call == "git_read") {
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
    auto jint = [&](const char* k, long long dflt) {
      auto it = args.find(k);
      return (it != args.end() && it->is_number_integer()) ? it->get<long long>() : dflt;
    };
    const std::string op = jstr("op");
    const std::string path = jstr("path");
    if (!path.empty() && path[0] == '-') {
      out = {{"ok", false}, {"result", {{"error", "path may not begin with '-'"}}}};
    } else if (op != "status" && op != "diff" && op != "log") {
      out = {{"ok", false}, {"result", {{"error", "op must be status, diff or log"}}}};
    } else {
      std::vector<std::string> argv = {"git"};
      long long line_cap = 200;
      if (op == "status") {
        argv.insert(argv.end(), {"status", "--porcelain=v1", "--branch"});
        line_cap = std::clamp(jint("max_lines", 200), 1LL, 1000LL);
      } else if (op == "diff") {
        argv.push_back("diff");
        if (jbool("staged", false)) argv.push_back("--staged");
        line_cap = std::clamp(jint("max_lines", 200), 1LL, 1000LL);
      } else {  // log
        const long long n = std::clamp(jint("max_lines", 30), 1LL, 200LL);
        argv.insert(argv.end(), {"log", "--oneline", "--decorate", "-n", std::to_string(n)});
        line_cap = n;
      }
      if (!path.empty()) {
        argv.push_back("--");
        argv.push_back(path);
      }
      hades::ProcResult r = hades::run_subprocess(argv, "", 30.0);
      bool truncated = false;
      if (r.timed_out) {
        out = {{"ok", false}, {"result", {{"error", "git timed out"}}}};
      } else if (r.code != 0) {
        out = {{"ok", false},
               {"result", {{"error", cap_lines(r.err.empty() ? r.out : r.err, 50, truncated)},
                           {"exit_code", r.code}}}};
      } else {
        out = {{"ok", true},
               {"result", {{"output", cap_lines(r.out, line_cap, truncated)},
                           {"truncated", truncated},
                           {"exit_code", r.code}}}};
      }
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }
  std::cout << out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << std::endl;
  return 0;
}
