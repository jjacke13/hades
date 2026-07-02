// include/hades/turn_gate.h — shared whole-turn serializer for concurrent front-ends
//
// One agent = one bus = ONE turn at a time. Each front-end (REPL, HTTP, Telegram) locks this
// gate around its post(USER_MESSAGE / CONFIRM_RESPONSE) -> run_until(...) sequence, so exactly
// one thread pumps the Blackboard at any moment — the single-threaded-dispatch invariant now
// rests on this gate instead of "one front-end per process". Owned by Agent as its FIRST member
// (destroyed LAST — outlives every module holding a pointer). Idle front-ends (REPL blocked on
// stdin, HTTP awaiting a request, Telegram long-polling) hold NOTHING — other surfaces proceed.
#pragma once
#include <mutex>
namespace hades {
struct TurnGate {
  std::mutex mu;
};
}  // namespace hades
