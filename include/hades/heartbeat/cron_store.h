// include/hades/heartbeat/cron_store.h — pure task-store record model + fold/compact + time parse
//
// The .hades/cron.jsonl store is append-only with three op records: "add" (a task), "cancel" and
// "done" (tombstones). fold_cron_store replays a file's lines into the active task set (add inserts,
// cancel/done erase by id); compact_cron_store re-serializes that set as add-records. Pure string
// in/out (no file IO) so the three tools AND the module share one implementation and it is unit-
// testable. Times are machine-LOCAL epoch seconds (matches the machine-local cron convention).
#pragma once
#include <optional>
#include <string>
#include <vector>
namespace hades {

struct CronTask {
  std::string id;              // "t<epoch>-<hex4>"
  std::string name;            // agent-chosen label
  std::string kind;            // "cron" | "once" | "when"
  std::string schedule;        // 5-field cron (kind=="cron"); "" otherwise
  long long   fire_epoch = 0;  // local epoch seconds (kind=="once"); 0 otherwise
  std::string prompt;          // the self-turn prompt
  bool        notify = false;
  long long   created = 0;     // local epoch seconds at creation
  std::string when;            // reactive condition (kind=="when"); "" otherwise
  long long   cooldown_s = 60; // min seconds between fires (when kind)
};

// Replay append-only jsonl into the active set. add -> insert by id; cancel/done -> erase. Tolerant:
// blank/corrupt/partial/id-less lines skipped. Sorted by (created, id) for a deterministic list.
std::vector<CronTask> fold_cron_store(const std::string& jsonl_text);

// The folded active set re-serialized as add-records (one per line, trailing '\n') = the compacted store.
std::string compact_cron_store(const std::string& jsonl_text);

// One record line (no trailing newline).
std::string add_record(const CronTask& t);
std::string cancel_record(const std::string& id);
std::string done_record(const std::string& id);

// Parse an absolute `at` to a local epoch. Accepts "YYYY-MM-DDTHH:MM[:SS]" and bare "HH:MM" (the next
// future occurrence vs now_epoch). nullopt on any unparseable input.
std::optional<long long> parse_at(const std::string& at, long long now_epoch);

// Task id from a creation epoch + 16 random bits: "t<epoch>-<hex4>".
std::string make_task_id(long long created_epoch, unsigned rand16);

}  // namespace hades
