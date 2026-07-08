// tools/list_tasks_main.cpp — bundled list_tasks native tool binary
//
// Lists the agent's OWN active dynamic tasks from the cron store (argv[1], fallback
// .hades/cron.jsonl). Static Heartbeat manifest entries are operator-owned and NOT in the store, so
// they are not listed. Read-only; fail-closed. `at`/`in_minutes` one-shots surface fire_epoch as a
// local ISO; `when`-kind tasks surface the reactive condition string instead.
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/heartbeat/cron_store.h"
using nlohmann::json;

static std::string iso_local(long long epoch) {
  std::time_t t = static_cast<std::time_t>(epoch);
  std::tm lt{}; localtime_r(&t, &lt);
  char buf[32]; std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M", &lt);
  return buf;
}

int main(int argc, char** argv) {
  const std::string store = argc > 1 ? argv[1] : ".hades/cron.jsonl";
  std::string line;
  std::getline(std::cin, line);
  auto in = json::parse(line, nullptr, false);
  std::string call;
  if (in.is_object() && in.contains("call") && in["call"].is_string()) call = in["call"].get<std::string>();

  if (call == "describe") {
    std::cout << json{{"ok", true},
                      {"result",
                       {{"name", "list_tasks"},
                        {"description",
                         "List the scheduled tasks YOU created (id, name, kind, timing, prompt, "
                         "notify). Static operator-configured heartbeats are not shown. Use the id "
                         "with cancel_task to remove one."},
                        {"schema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}}}}
                     .dump()
              << std::endl;
    return 0;
  }
  if (call != "list_tasks") {
    std::cout << json{{"ok", false}, {"result", {{"error", "unknown call: " + call}}}}.dump() << std::endl;
    return 0;
  }

  std::string body;
  { std::ifstream f(store); std::stringstream s; s << f.rdbuf(); body = s.str(); }
  json tasks = json::array();
  for (const auto& t : hades::fold_cron_store(body)) {
    json e{{"id", t.id}, {"name", t.name}, {"kind", t.kind}, {"prompt", t.prompt}, {"notify", t.notify}};
    if (t.kind == "cron") e["schedule"] = t.schedule;
    else if (t.kind == "when") e["when"] = t.when;
    else e["at"] = iso_local(t.fire_epoch);
    tasks.push_back(e);
  }
  std::cout << json{{"ok", true}, {"result", {{"tasks", tasks}}}}.dump() << std::endl;
  return 0;
}
