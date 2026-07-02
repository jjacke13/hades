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

  // Movable so the owning Agent (which holds a TurnGate by value as its FIRST member) can be
  // returned by value from build_agent. In practice NRVO constructs the gate in place, so this
  // move is never actually executed — modules capture `&gate` only AFTER build_agent returns.
  // A moved-to gate therefore gets a FRESH mutex, which is sound: a gate is only ever moved at
  // Agent construction/return, never while a turn holds the lock. std::mutex is non-copyable, so
  // declaring the move also (correctly) leaves the copy operations deleted.
  TurnGate() = default;
  TurnGate(TurnGate&&) noexcept {}
  TurnGate& operator=(TurnGate&&) noexcept { return *this; }
};
}  // namespace hades
