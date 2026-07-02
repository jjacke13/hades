// tests/test_turn_gate.cpp — shared TurnGate serializes whole turns across front-end threads
#include <gtest/gtest.h>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "hades/blackboard.h"
#include "hades/module/http_server_module.h"
#include "hades/turn_gate.h"
using namespace hades;

TEST(TurnGate, TurnWaitsWhileAnotherFrontEndHoldsTheGate) {
  Blackboard bb;
  HttpServerModule m;
  TurnGate gate;
  m.set_turn_gate(&gate);
  m.on_attach(bb);
  bb.subscribe("USER_MESSAGE", [&](const Entry& e) {
    bb.post("ASSISTANT_MESSAGE", "echo:" + e.value.get<std::string>(), "t");
  });
  std::vector<std::string> order;   // ordering established by the gate's acquire/release
  std::unique_lock<std::mutex> hold(gate.mu);          // simulate another front-end mid-turn
  std::thread t([&] {
    auto r = m.handle_message("late");                 // must block on the shared gate
    order.push_back("turn:" + r.value("reply", ""));
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  order.push_back("released");
  hold.unlock();
  t.join();
  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], "released");                     // the turn could not run while held
  EXPECT_EQ(order[1], "turn:echo:late");
}

TEST(TurnGate, NullGateFallsBackToLocalSerialization) {
  Blackboard bb;
  HttpServerModule m;                                  // no set_turn_gate call
  m.on_attach(bb);
  bb.subscribe("USER_MESSAGE", [&](const Entry& e) {
    bb.post("ASSISTANT_MESSAGE", "echo:" + e.value.get<std::string>(), "t");
  });
  auto r = m.handle_message("hi");
  EXPECT_EQ(r.value("reply", ""), "echo:hi");          // pre-gate behavior intact
}
