// include/hades/heartbeat/when.h — pure reactive-trigger condition (KEY changes|is|not|above|below)
//
// The `when =` counterpart to cron.h: parses and evaluates the 5 keyword condition forms a Heartbeat
// entry may carry instead of a schedule. Keyword operators (never '='/'=='), because an inline
// manifest value containing '=' trips the one-kv-per-line fail-loud parser. Evaluation is pure and
// stateless here — the edge detection ("fire once per change / per false->true transition") lives in
// the HeartbeatModule, which owns the per-entry state. Tolerant: malformed -> nullopt/false, never
// throws. Compiled into hades_core AND the schedule_task binary (cron.h precedent).
#pragma once
#include <optional>
#include <string>
#include <nlohmann/json.hpp>
namespace hades {

struct WhenCond {
  std::string key;                    // the Blackboard key to watch
  enum class Op { Changes, Is, Not, Above, Below } op = Op::Changes;
  std::string operand;                // "" for Changes; string for Is/Not; numeric text for Above/Below
};

// Parse "<KEY> <op> [operand]". Is/Not take the REMAINDER of the string as the operand (spaces ok);
// Above/Below require a parseable number; Changes takes nothing. nullopt on any violation.
std::optional<WhenCond> parse_when(const std::string& expr);

// Fail-loud validator for wiring (MalConfig) and the schedule_task tool (ok:false).
bool when_valid(const std::string& expr);

// Evaluate Is/Not/Above/Below against a latest value (nullptr = key absent -> false).
// Changes always returns false here — it is stateful and evaluated by the module.
bool when_holds(const WhenCond& c, const nlohmann::json* value);

}  // namespace hades
