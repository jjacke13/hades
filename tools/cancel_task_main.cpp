// tools/cancel_task_main.cpp — bundled cancel_task native tool binary
//
// Cancels one of the agent's OWN tasks by id: APPENDS a cancel tombstone to the cron store (argv[1],
// fallback .hades/cron.jsonl) if the id is currently active. Unknown/inactive id -> ok:false.
// Append-only (the module compacts on boot); fail-closed.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/heartbeat/cron_store.h"
using nlohmann::json;
namespace fs = std::filesystem;

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
                       {{"name", "cancel_task"},
                        {"description", "Cancel one of your scheduled tasks by its id (from list_tasks)."},
                        {"schema",
                         {{"type", "object"},
                          {"properties", {{"id", {{"type", "string"}}}}},
                          {"required", json::array({"id"})}}}}}}
                     .dump()
              << std::endl;
    return 0;
  }
  if (call != "cancel_task") {
    std::cout << json{{"ok", false}, {"result", {{"error", "unknown call: " + call}}}}.dump() << std::endl;
    return 0;
  }

  json args = (in.is_object() && in.contains("args") && in["args"].is_object()) ? in["args"] : json::object();
  const std::string id = args.contains("id") && args["id"].is_string() ? args["id"].get<std::string>() : "";
  auto fail = [&](const std::string& e) {
    std::cout << json{{"ok", false}, {"result", {{"error", e}}}}.dump() << std::endl;
    return 0;
  };
  if (id.empty()) return fail("missing arg: id");

  std::string body;
  { std::ifstream f(store); std::stringstream s; s << f.rdbuf(); body = s.str(); }
  bool active = false;
  for (const auto& t : hades::fold_cron_store(body)) if (t.id == id) { active = true; break; }
  if (!active) return fail("no active task with id: " + id);

  std::ofstream f(store, std::ios::app);
  if (!f) return fail("cannot append to store: " + store);
  f << hades::cancel_record(id) << "\n";
  std::cout << json{{"ok", true}, {"result", {{"cancelled", true}, {"id", id}}}}.dump() << std::endl;
  return 0;
}
