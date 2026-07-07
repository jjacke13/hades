// tools/schedule_task_main.cpp — bundled schedule_task native tool binary
//
// Creates a scheduled task by APPENDING an add-record to the cron store (argv[1], fallback
// .hades/cron.jsonl). One of schedule (5-field cron) | in_minutes (relative) | at (absolute local)
// is required; kind is "cron" or "once". Caps: argv[2] max_tasks (refuse when the active count is at
// the cap), argv[3] min_interval_s (one-shot delay floor). The store path + caps are fixed by wiring
// argv — never chosen by the LLM. Fail-closed: malformed/adversarial input returns ok:false, never
// throws. A task is a PROMPT to a future gated self-turn (never a raw command).
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/heartbeat/cron.h"         // cron_valid
#include "hades/heartbeat/cron_store.h"   // CronTask, fold_cron_store, add_record, parse_at, make_task_id

using nlohmann::json;
namespace fs = std::filesystem;

static std::string iso_local(long long epoch) {
  std::time_t t = static_cast<std::time_t>(epoch);
  std::tm lt{}; localtime_r(&t, &lt);
  char buf[32]; std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M", &lt);
  return buf;
}

int main(int argc, char** argv) {
  const std::string store = argc > 1 ? argv[1] : ".hades/cron.jsonl";
  long long max_tasks = argc > 2 ? std::strtoll(argv[2], nullptr, 10) : 20;
  long long min_interval_s = argc > 3 ? std::strtoll(argv[3], nullptr, 10) : 60;
  if (max_tasks <= 0) max_tasks = 20;          // a wiring misconfig must not brick scheduling
  if (min_interval_s < 0) min_interval_s = 60;

  std::string line;
  std::getline(std::cin, line);
  auto in = json::parse(line, nullptr, false);

  json out;
  std::string call;
  if (in.is_object() && in.contains("call") && in["call"].is_string()) call = in["call"].get<std::string>();

  if (call == "describe") {
    json required = json::array({"name", "prompt"});
    out = {{"ok", true},
           {"result",
            {{"name", "schedule_task"},
             {"description",
              "Schedule one of YOUR OWN future turns. Provide name + prompt (the instruction your "
              "future self runs, gated as a normal turn — to run a command, say so in the prompt and "
              "you will call run_command then). Timing is exactly ONE of: schedule (5-field cron, "
              "recurring), in_minutes (run once, N minutes from now), at (run once, absolute "
              "'YYYY-MM-DDTHH:MM' or 'HH:MM' local). notify=true forwards the reply to the user. Use "
              "list_tasks/cancel_task to manage them."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"name", {{"type", "string"}}},
                 {"prompt", {{"type", "string"}}},
                 {"notify", {{"type", "boolean"}}},
                 {"schedule", {{"type", "string"}}},
                 {"in_minutes", {{"type", "number"}}},
                 {"at", {{"type", "string"}}}}},
               {"required", required}}}}}};
    std::cout << out.dump() << std::endl;
    return 0;
  }
  if (call != "schedule_task") {
    std::cout << json{{"ok", false}, {"result", {{"error", "unknown call: " + call}}}}.dump() << std::endl;
    return 0;
  }

  json args = (in.is_object() && in.contains("args") && in["args"].is_object()) ? in["args"] : json::object();
  auto str = [&](const char* k) {
    return args.contains(k) && args[k].is_string() ? args[k].get<std::string>() : std::string{};
  };
  const std::string name = str("name");
  const std::string prompt = str("prompt");
  auto fail = [&](const std::string& e) {
    std::cout << json{{"ok", false}, {"result", {{"error", e}}}}.dump() << std::endl;
    return 0;
  };
  if (name.empty() || prompt.empty()) return fail("missing arg: name and prompt required");

  const bool has_sched = args.contains("schedule") && args["schedule"].is_string();
  const bool has_in    = args.contains("in_minutes") && args["in_minutes"].is_number();
  const bool has_at    = args.contains("at") && args["at"].is_string();
  if (has_sched + has_in + has_at != 1)
    return fail("provide exactly one of: schedule, in_minutes, at");

  const long long now =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

  hades::CronTask t;
  t.name = name; t.prompt = prompt; t.created = now;
  t.notify = args.contains("notify") && args["notify"].is_boolean() ? args["notify"].get<bool>() : false;
  std::string when;
  if (has_sched) {
    const std::string sched = args["schedule"].get<std::string>();
    if (!hades::cron_valid(sched)) return fail("invalid cron schedule: " + sched);
    t.kind = "cron"; t.schedule = sched; when = sched;
  } else if (has_in) {
    const double mins = args["in_minutes"].get<double>();
    if (!std::isfinite(mins) || mins < 0 || mins > 1e9)
      return fail("in_minutes out of range");
    const long long delay = static_cast<long long>(mins * 60);
    if (delay < min_interval_s) return fail("in_minutes below the min interval floor");
    t.kind = "once"; t.fire_epoch = now + delay; when = iso_local(t.fire_epoch);
  } else {
    auto e = hades::parse_at(args["at"].get<std::string>(), now);
    if (!e) return fail("unparseable at (want YYYY-MM-DDTHH:MM or HH:MM)");
    t.kind = "once"; t.fire_epoch = *e; when = iso_local(t.fire_epoch);
  }

  // Cap: refuse when the active set is already at max_tasks.
  std::string body;
  { std::ifstream f(store); std::stringstream s; s << f.rdbuf(); body = s.str(); }
  if (static_cast<long long>(hades::fold_cron_store(body).size()) >= max_tasks)
    return fail("task cap reached (max_tasks=" + std::to_string(max_tasks) + ")");

  std::random_device rd;
  t.id = hades::make_task_id(now, rd());

  fs::path p(store);
  if (p.has_parent_path()) { std::error_code ec; fs::create_directories(p.parent_path(), ec); }
  std::ofstream f(store, std::ios::app);
  if (!f) return fail("cannot append to store: " + store);
  f << hades::add_record(t) << "\n";

  std::cout << json{{"ok", true},
                    {"result", {{"id", t.id}, {"name", t.name}, {"kind", t.kind}, {"when", when}}}}.dump()
            << std::endl;
  return 0;
}
