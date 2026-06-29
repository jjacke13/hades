// tests/test_blackboard.cpp — unit tests for Blackboard pub/sub and pump dispatch
//
// Covers post/pump delivery ordering (FIFO event queue, latest-value store),
// prefix-wildcard ("TOOL_*") and catch-all ("*") subscription matching, and
// the case where a handler re-posts during pump() — the foundation all Module
// communication relies on.

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include "hades/blackboard.h"
using namespace hades;
TEST(Blackboard, PostStoresLatestAndPumpDispatches) {
  Blackboard bb;
  std::vector<std::string> got;
  bb.subscribe("USER_MESSAGE", [&](const Entry& e){ got.push_back(e.value.get<std::string>()); });
  bb.post("USER_MESSAGE", "a", "chat");
  bb.post("USER_MESSAGE", "b", "chat");
  EXPECT_TRUE(got.empty());                 // nothing delivered during post
  bb.pump();
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[1], "b");
  EXPECT_EQ(bb.get("USER_MESSAGE")->value.get<std::string>(), "b");  // latest-value
}
TEST(Blackboard, PrefixWildcardAndStarMatch) {
  Blackboard bb; int pfx=0, all=0;
  bb.subscribe("TOOL_*", [&](const Entry&){ ++pfx; });
  bb.subscribe("*",      [&](const Entry&){ ++all; });
  bb.post("TOOL_REQUEST", 1, "arb");
  bb.post("USER_MESSAGE", "x", "chat");
  bb.pump();
  EXPECT_EQ(pfx, 1); EXPECT_EQ(all, 2);
}
TEST(Blackboard, HandlerCanPostMore) {
  Blackboard bb; int n=0;
  bb.subscribe("A", [&](const Entry&){ bb.post("B", 1, "s"); });
  bb.subscribe("B", [&](const Entry&){ ++n; });
  bb.post("A", 1, "s"); bb.pump();
  EXPECT_EQ(n, 1);
}

TEST(Blackboard, RunUntilDeliversAWorkerThreadPost) {
  Blackboard bb;
  std::atomic<int> got{0};
  bb.subscribe("FROM_WORKER", [&](const Entry&){ got.fetch_add(1); });
  std::thread worker([&]{
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    bb.post("FROM_WORKER", 1, "worker");   // posted from another thread
  });
  bool ok = bb.run_until([&]{ return got.load() > 0; }, 5.0);
  worker.join();
  EXPECT_TRUE(ok);
  EXPECT_EQ(got.load(), 1);   // handler ran on the main (run_until) thread
}

TEST(Blackboard, RunUntilTimesOutWhenPredicateNeverHolds) {
  Blackboard bb;
  bool ok = bb.run_until([]{ return false; }, 0.2);   // ~200ms
  EXPECT_FALSE(ok);
}

TEST(Blackboard, RunUntilReturnsImmediatelyOnInlineCompletion) {
  Blackboard bb;
  bool fired = false;
  bb.subscribe("A", [&](const Entry&){ fired = true; });
  bb.post("A", 1, "s");                       // already queued, inline handler
  bool ok = bb.run_until([&]{ return fired; }, 5.0);
  EXPECT_TRUE(ok);
  EXPECT_TRUE(fired);
}

TEST(Blackboard, PumpReturnsDispatchCount) {
  Blackboard bb;
  bb.subscribe("X", [&](const Entry&){});
  bb.post("X", 1, "s");
  bb.post("X", 2, "s");
  EXPECT_EQ(bb.pump(), 2u);                    // two entries dequeued/dispatched
  EXPECT_EQ(bb.pump(), 0u);                    // empty queue -> no progress
}

TEST(Blackboard, RunUntilIdleTimeoutResetsOnProgress) {
  // A worker posts TICK every ~40ms for ~0.4s while run_until's idle timeout is
  // 0.2s. Each delivered TICK resets the idle deadline, so the call must NOT bail
  // at 0.2s — it returns false only after the ticks stop (steady progress kept it
  // alive past 0.4s). Verifies the deadline-on-progress (idle) timeout.
  Blackboard bb;
  std::atomic<int> ticks{0};
  bb.subscribe("TICK", [&](const Entry&){ ticks.fetch_add(1); });
  std::atomic<bool> stop{false};
  std::thread worker([&]{
    for (int i = 0; i < 10 && !stop.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      bb.post("TICK", i, "worker");
    }
  });
  const auto t_start = std::chrono::steady_clock::now();
  bool ok = bb.run_until([]{ return false; }, 0.2);   // predicate never true
  const auto t_end = std::chrono::steady_clock::now();
  stop.store(true);
  worker.join();
  const double elapsed_s =
      std::chrono::duration<double>(t_end - t_start).count();
  EXPECT_FALSE(ok);                            // predicate never held -> timed out
  EXPECT_GT(elapsed_s, 0.4);                   // progress reset the 0.2s idle window
}
