// tools/run_command_main.cpp — bundled run_command native tool binary (allowlisted runner)
//
// Reads one JSON line ({"call":"describe"|"run_command","args":{command,timeout_s}}), splits
// the command on WHITESPACE into argv and execs it DIRECTLY (hades::run_subprocess) — there is
// NO shell: no pipes, redirection, $() expansion, globbing or cd. That no-shell property is
// what makes the upstream exec_allow prefix gate sound (CapabilityPolicy: metachars -> confirm,
// allowlisted prefix -> allow, else confirm). Need shell features -> use the `shell` tool,
// which stays human-confirmed. The inner timeout stays BELOW the Tool block's timeout_s cap
// (dev.hades: 600) so this tool reports its own timeout instead of being SIGKILLed mid-write.
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"

namespace {
constexpr std::size_t kCap = 48 * 1024;   // per-stream byte cap
std::string cap(std::string s, bool& truncated) {
  if (s.size() > kCap) { s.resize(kCap); truncated = true; }
  return s;
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
            {{"name", "run_command"},
             {"description",
              "Run a build/test command WITHOUT a shell: the command is split on whitespace "
              "and executed directly — no pipes, redirection, $() or globs (use shell for "
              "those). Returns stdout, stderr and the exit code."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"command", {{"type", "string"}}}, {"timeout_s", {{"type", "number"}}}}},
               {"required", {"command"}}}}}}};
  } else if (call == "run_command") {
    nlohmann::json args = (in.is_object() && in.contains("args") && in["args"].is_object())
                              ? in["args"] : nlohmann::json::object();
    auto jstr = [&](const char* k) {
      auto it = args.find(k);
      return (it != args.end() && it->is_string()) ? it->get<std::string>() : std::string{};
    };
    double timeout = 300.0;
    if (auto it = args.find("timeout_s"); it != args.end() && it->is_number())
      timeout = it->get<double>();
    timeout = std::clamp(timeout, 0.05, 570.0);
    const std::string command = jstr("command");
    std::vector<std::string> argv;
    std::istringstream is(command);
    std::string w;
    while (is >> w) argv.push_back(w);
    if (argv.empty()) {
      out = {{"ok", false}, {"result", {{"error", "missing/empty command"}}}};
    } else {
      hades::ProcResult r = hades::run_subprocess(argv, "", timeout);
      bool trunc_out = false, trunc_err = false;
      nlohmann::json result = {{"stdout", cap(r.out, trunc_out)},
                               {"stderr", cap(r.err, trunc_err)},
                               {"exit_code", r.code},
                               {"timed_out", r.timed_out},
                               {"truncated", trunc_out || trunc_err}};
      out = {{"ok", !r.timed_out}, {"result", result}};
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }
  std::cout << out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << std::endl;
  return 0;
}
