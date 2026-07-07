// src/apps/heartbeat/cron_store.cpp — cron.jsonl record model: fold, compact, serialize, time parse
#include "hades/heartbeat/cron_store.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <sstream>
#include <nlohmann/json.hpp>
namespace hades {
namespace {
CronTask task_from_json(const nlohmann::json& j) {
  CronTask t;
  t.id       = j.value("id", "");
  t.name     = j.value("name", "");
  t.kind     = j.value("kind", "");
  if (j.contains("schedule") && j["schedule"].is_string()) t.schedule = j["schedule"].get<std::string>();
  if (j.contains("fire_epoch") && j["fire_epoch"].is_number_integer())
    t.fire_epoch = j["fire_epoch"].get<long long>();
  t.prompt   = j.value("prompt", "");
  t.notify   = j.value("notify", false);
  t.created  = j.value("created", 0LL);
  return t;
}
}  // namespace

std::vector<CronTask> fold_cron_store(const std::string& jsonl_text) {
  std::map<std::string, CronTask> active;   // id -> task; add inserts, cancel/done erase
  std::istringstream in(jsonl_text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    auto j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) continue;      // tolerant: skip a torn/partial line
    try {
      const std::string op = j.value("op", "");
      const std::string id = j.value("id", "");
      if (id.empty()) continue;
      if (op == "add") active[id] = task_from_json(j);
      else if (op == "cancel" || op == "done") active.erase(id);
    } catch (const nlohmann::json::exception&) {
      continue;   // tolerant: a wrong-typed field on an otherwise well-formed line skips the line
    }
  }
  std::vector<CronTask> out;
  out.reserve(active.size());
  for (auto& [id, t] : active) out.push_back(t);
  std::sort(out.begin(), out.end(), [](const CronTask& a, const CronTask& b) {
    return a.created != b.created ? a.created < b.created : a.id < b.id;
  });
  return out;
}

std::string add_record(const CronTask& t) {
  nlohmann::json j{{"op", "add"}, {"id", t.id}, {"name", t.name}, {"kind", t.kind},
                   {"schedule", t.kind == "cron" ? nlohmann::json(t.schedule) : nlohmann::json()},
                   {"fire_epoch", t.kind == "once" ? nlohmann::json(t.fire_epoch) : nlohmann::json()},
                   {"prompt", t.prompt}, {"notify", t.notify}, {"created", t.created}};
  return j.dump();
}
std::string cancel_record(const std::string& id) {
  return nlohmann::json{{"op", "cancel"}, {"id", id}}.dump();
}
std::string done_record(const std::string& id) {
  return nlohmann::json{{"op", "done"}, {"id", id}}.dump();
}

std::string compact_cron_store(const std::string& jsonl_text) {
  std::string out;
  for (const auto& t : fold_cron_store(jsonl_text)) out += add_record(t) + "\n";
  return out;
}

std::optional<long long> parse_at(const std::string& at, long long now_epoch) {
  int y, mo, d, h, mi, s = 0;
  if (std::sscanf(at.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) >= 5) {
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h >= 24 || mi < 0 || mi >= 60 || s < 0 || s >= 60)
      return std::nullopt;   // reject before mktime silently normalizes an out-of-range field
    std::tm tm{};
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s; tm.tm_isdst = -1;
    std::time_t e = std::mktime(&tm);
    if (e == static_cast<std::time_t>(-1)) return std::nullopt;
    return static_cast<long long>(e);
  }
  if (std::sscanf(at.c_str(), "%d:%d", &h, &mi) == 2 && h >= 0 && h < 24 && mi >= 0 && mi < 60) {
    std::time_t now = static_cast<std::time_t>(now_epoch);
    std::tm local{};
    localtime_r(&now, &local);
    local.tm_hour = h; local.tm_min = mi; local.tm_sec = 0; local.tm_isdst = -1;
    std::time_t e = std::mktime(&local);
    if (e == static_cast<std::time_t>(-1)) return std::nullopt;
    if (static_cast<long long>(e) <= now_epoch) e += 24 * 3600;   // already passed -> tomorrow
    return static_cast<long long>(e);
  }
  return std::nullopt;
}

std::string make_task_id(long long created_epoch, unsigned rand16) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "t%lld-%04x", created_epoch, rand16 & 0xffffu);
  return buf;
}
}  // namespace hades
