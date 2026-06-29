// tests/test_offload_e2e.cpp — live-path proof: an offloaded LLM drives a full turn
//
// Uses the TEST build_agent overload (which leaves agent.executor null) but then
// attaches an Executor explicitly to the LLMModule, so the blocking provider call
// runs on a worker thread. A slow scripted Provider returns a final answer after a
// short sleep; the test drives one USER_MESSAGE -> ASSISTANT_MESSAGE turn via
// bb.run_until(...) exactly as the live front-ends do, asserting the answer arrives.
//
// Offline + cheap (no socket, ~30ms sleep). Local declaration order is load-bearing
// (mirrors the live Agent/Blackboard order): the Executor is declared LAST so it is
// destroyed FIRST — its dtor joins the worker while the Agent (its LLMModule) and the
// Blackboard are still alive, closing the use-after-free Task 3 hit.

#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/executor.h"
using namespace hades;

namespace {
// Sleeps briefly, then returns a final text answer (no tool call). The sleep forces
// the offloaded path: pump() returns before the worker has posted LLM_RESPONSE.
struct SlowAnswerProvider : Provider {
  LlmResponse complete(const LlmRequest&) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    LlmResponse r;
    r.text = "offloaded answer";
    return r;
  }
};
}  // namespace

TEST(OffloadE2E, OffloadedLlmCompletesAFullTurnViaRunUntil) {
  Blackboard bb;
  auto agent = build_agent(bb, std::make_unique<SlowAnswerProvider>(), {}, {}, "m");
  ASSERT_TRUE(agent.llm);
  EXPECT_EQ(agent.executor, nullptr);  // test overload never sets one

  // Attach a worker pool to the LLMModule (declared LAST -> destroyed FIRST: joins the
  // worker while `agent` and `bb` are still alive).
  Executor ex(2);
  agent.llm->set_executor(&ex);

  std::string answer;
  bb.subscribe("ASSISTANT_MESSAGE", [&](const Entry& e) {
    if (e.value.is_string()) answer = e.value.get<std::string>();
  });

  bb.post("USER_MESSAGE", "say something", "chat");
  // The LLM call is offloaded, so a bare pump() cannot finish the turn synchronously;
  // run_until sleeps on the bus until the worker posts the response back, exactly as
  // the live REPL/HTTP front-ends now do.
  const bool done = bb.run_until([&] { return !answer.empty(); }, 5.0);
  EXPECT_TRUE(done);
  EXPECT_EQ(answer, "offloaded answer");
}
