// tests/test_bridge_wiring.cpp — manifest wiring + the two-agent e2e over real sockets.
// The e2e builds TWO full manifest agents in one process (no llm module: the "brain" is an
// echo subscriber, LLM_REQUEST is never consumed) and drives A's real ask_agent binary
// through A's ToolRunner at B's real bridge listener.
#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
using namespace hades;

namespace {
std::string bridge_manifest(const std::string& name, const std::string& peer_name,
                            const std::string& peer_url) {
  return std::string("Session\n{\n  model = m\n}\n") +
         "Module = tool_runner\n" +
         "Module = arbiter\n" +
         "Module = bridge\n" +
         "Bridge\n{\n  name = " + name + "\n  port = 0\n}\n" +
         "Peer = " + peer_name + " { url = " + peer_url + " }\n" +
         "Tool = ask_agent { native = " + ASK_AGENT_BIN + " }\n";
}
}  // namespace

TEST(BridgeWiring, BridgeModuleWithoutNameThrows) {
  ::setenv("HADES_BRIDGE_SECRET", "s3cret", 1);
  Blackboard bb;
  Manifest m = parse_manifest(
      "Session\n{\n  model = m\n}\nModule = arbiter\nModule = bridge\n"
      "Peer = x { url = http://127.0.0.1:1 }\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(BridgeWiring, AskAgentToolWithoutPeersThrows) {
  Blackboard bb;
  Manifest m = parse_manifest(
      std::string("Session\n{\n  model = m\n}\nModule = tool_runner\nModule = arbiter\n") +
      "Bridge\n{\n  name = solo\n}\n" +
      "Tool = ask_agent { native = " + ASK_AGENT_BIN + " }\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(BridgeWiring, AskAgentToolWithoutBridgeNameThrows) {
  Blackboard bb;
  Manifest m = parse_manifest(
      std::string("Session\n{\n  model = m\n}\nModule = tool_runner\nModule = arbiter\n") +
      "Peer = x { url = http://127.0.0.1:1 }\n" +
      "Tool = ask_agent { native = " + ASK_AGENT_BIN + " }\n");
  EXPECT_THROW(build_agent(bb, m), MalConfig);
}

TEST(BridgeWiring, BadPeerBlocksThrow) {
  Blackboard bb;
  // duplicate peer name
  EXPECT_THROW(build_agent(bb, parse_manifest(
      "Session\n{\n  model = m\n}\nModule = arbiter\n"
      "Peer = x { url = http://a:1 }\nPeer = x { url = http://b:1 }\n")), MalConfig);
  // missing url
  EXPECT_THROW(build_agent(bb, parse_manifest(
      "Session\n{\n  model = m\n}\nModule = arbiter\nPeer = x { nope = 1 }\n")), MalConfig);
}

TEST(BridgeWiring, NoBridgeRosterLeavesAgentBridgeNull) {
  Blackboard bb;
  Manifest m = parse_manifest("Session\n{\n  model = m\n}\nModule = arbiter\n");
  Agent agent = build_agent(bb, m);
  EXPECT_EQ(agent.bridge, nullptr);
}

TEST(BridgeWiring, TwoAgentAskEndToEnd) {
  ::setenv("HADES_BRIDGE_SECRET", "s3cret", 1);

  // Agent B ("worker1"): real bridge listener; its "brain" echoes USER_MESSAGE.
  Blackboard bb_b;
  Agent b = build_agent(bb_b, parse_manifest(
      bridge_manifest("worker1", "front", "http://127.0.0.1:1")));
  ASSERT_NE(b.bridge, nullptr);
  bb_b.subscribe("USER_MESSAGE", [&](const Entry& e) {
    bb_b.post("ASSISTANT_MESSAGE", "B says: " + e.value.get<std::string>(), "t");
  });
  const int b_port = b.bridge->start_listening();
  ASSERT_GT(b_port, 0);

  // Agent A ("front"): knows B by its real port; drive A's REAL ask_agent tool binary
  // through A's ToolRunner (the argv was assembled by wiring — this is what the e2e pins).
  Blackboard bb_a;
  Agent a = build_agent(bb_a, parse_manifest(
      bridge_manifest("front", "worker1", "http://127.0.0.1:" + std::to_string(b_port))));
  nlohmann::json result;
  bb_a.subscribe("TOOL_RESULT", [&](const Entry& e) { result = e.value; });
  bb_a.post("TOOL_REQUEST",
            {{"id", "e2e1"},
             {"tool", "ask_agent"},
             {"args", {{"peer", "worker1"}, {"message", "status?"}}}},
            "arbiter");
  bb_a.pump();
  ASSERT_TRUE(result.is_object());
  EXPECT_TRUE(result.value("ok", false)) << result.dump();
  EXPECT_EQ(result["content"].value("reply", ""),
            "B says: (from peer agent \"front\") status?");
}

TEST(BridgeWiring, PeerTurnCannotAskOnward) {
  ::setenv("HADES_BRIDGE_SECRET", "s3cret", 1);
  Blackboard bb;
  Agent agent = build_agent(bb, parse_manifest(
      bridge_manifest("worker1", "front", "http://127.0.0.1:1")));
  // Simulate a peer-driven turn, then ask the ARBITER to dispatch ask_agent: the
  // auto-registered PeerLoopGuard must hard-veto it (no confirm, no subprocess spawn).
  bb.post("TURN_ORIGIN", "peer:front", "bridge");
  nlohmann::json result;
  bool confirm = false;
  bb.subscribe("CONFIRM_REQUEST", [&](const Entry&) { confirm = true; });
  bb.subscribe("ASSISTANT_MESSAGE", [&](const Entry& e) { result = e.value; });
  // LLM_RESPONSE with a tool call is the Arbiter's dispatch entry (no llm module rostered:
  // post it directly, the shape the LLMModule would produce). The Arbiter consumes a SINGULAR
  // `tool_call` object with an `arguments` object (see arbiter.cpp on_llm_response /
  // tests/test_arbiter.cpp) — the brief's `tool_calls`/`args` sketch is adjusted to match.
  bb.post("USER_MESSAGE", "(from peer agent \"front\") do it", "bridge");
  bb.pump();
  auto req = bb.get("LLM_REQUEST");
  ASSERT_TRUE(req.has_value());   // the turn started
  bb.post("LLM_RESPONSE",
          {{"epoch", req->value.value("epoch", static_cast<std::uint64_t>(0))},
           {"tool_call",
            {{"id", "t1"},
             {"name", "ask_agent"},
             {"arguments", {{"peer", "front"}, {"message", "loop!"}}}}}},
          "test");
  bb.pump();
  EXPECT_FALSE(confirm);                        // hard veto, not confirm-gated
  ASSERT_TRUE(result.is_string());
  EXPECT_NE(result.get<std::string>().find("loop guard"), std::string::npos);
}
