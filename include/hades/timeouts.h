// include/hades/timeouts.h — single source of truth for the two think-time defaults
//
// Both limits are manifest-configurable (Session block: llm_timeout_s /
// turn_idle_timeout_s). These constants are the defaults used when a key is absent,
// shared by the LLMModule (per-call cpr cap), the two front-ends (run_until idle
// ceiling), and the wiring that validates the load-bearing invariant
// turn_idle_timeout_s > llm_timeout_s (a slow-but-alive call must post back before
// the idle timer abandons the turn).

#pragma once
namespace hades {
// Per-call LLM HTTP timeout (cpr). The real cap on ONE "think" request.
inline constexpr double kDefaultLlmTimeoutS = 600.0;       // 10 min
// run_until IDLE ceiling (front-ends). Resets on every bus event, so it bounds a
// single SILENT stretch, not total turn time. MUST stay > kDefaultLlmTimeoutS.
inline constexpr double kDefaultTurnIdleTimeoutS = 900.0;  // 15 min
}  // namespace hades
