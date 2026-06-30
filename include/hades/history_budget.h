// include/hades/history_budget.h — single source of truth for the per-turn history char budget
//
// The Arbiter persists the FULL conversation (history_) in memory and on disk, but a long or
// resumed session can exceed the LLM's context window. start_turn() therefore sends only the
// most-recent suffix of history_ whose cumulative serialized size fits this char budget — the
// full history is never lost; only the per-turn LLM REQUEST is bounded. Manifest-configurable
// (Session block: history_budget_chars); this constant is the default used when the key is absent,
// shared by the Arbiter (default member) and the wiring that reads it from the manifest.

#pragma once
namespace hades {
// Max cumulative chars of serialized history_ messages sent in one LLM request (~30k tokens).
// Setting it very high effectively means "always send the full session".
inline constexpr double kDefaultHistoryBudgetChars = 120000.0;
}  // namespace hades
