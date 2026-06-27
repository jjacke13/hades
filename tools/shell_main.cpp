// tools/shell_main.cpp — bundled shell native tool binary
//
// Reads one JSON line ({"call":"describe"|"shell","args":{cmd,timeout_s?}}), runs
// the command via `sh -c` using hades::run_subprocess (timeout + SIGKILL), and
// returns stdout/stderr/exit-code as one JSON line. Spawned as a subprocess by
// ToolRunner; the hades one-JSON-line native tool protocol. Destructive commands
// are gated upstream by the AvoidDestructive objective (veto + human confirm).
#include <iostream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"

int main() {
  std::string line;
  std::getline(std::cin, line);
  auto in = nlohmann::json::parse(line, nullptr, false);

  nlohmann::json out;
  std::string call = in.is_object() ? in.value("call", "") : "";

  if (call == "describe") {
    out = {{"ok", true},
           {"result",
            {{"name", "shell"},
             {"description", "Run a shell command (sh -c) and return stdout, stderr, and exit code."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"cmd", {{"type", "string"}}}, {"timeout_s", {{"type", "number"}}}}},
               {"required", {"cmd"}}}}}}};
  } else if (call == "shell") {
    nlohmann::json args =
        (in.is_object() && in.contains("args") && in["args"].is_object())
            ? in["args"]
            : nlohmann::json::object();
    std::string cmd = args.value("cmd", "");
    double timeout = args.value("timeout_s", 30.0);
    if (cmd.empty()) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: cmd"}}}};
    } else {
      hades::ProcResult r = hades::run_subprocess({"sh", "-c", cmd}, "", timeout);
      out = {{"ok", !r.timed_out},
             {"result",
              {{"stdout", r.out},
               {"stderr", r.err},
               {"code", r.code},
               {"timed_out", r.timed_out}}}};
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }

  std::cout << out.dump() << std::endl;
  return 0;
}
