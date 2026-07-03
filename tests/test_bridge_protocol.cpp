// tests/test_bridge_protocol.cpp — pure bridge wire protocol: build/parse/validate
#include <gtest/gtest.h>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/bridge/protocol.h"
using namespace hades;

TEST(BridgeProtocol, ValidPeerNameGate) {
  EXPECT_TRUE(valid_peer_name("worker-1_A"));
  EXPECT_TRUE(valid_peer_name("A"));
  EXPECT_FALSE(valid_peer_name(""));
  EXPECT_FALSE(valid_peer_name("a b"));
  EXPECT_FALSE(valid_peer_name("a/b"));
  EXPECT_FALSE(valid_peer_name("peer:x"));
  EXPECT_FALSE(valid_peer_name(std::string(65, 'a')));
}

TEST(BridgeProtocol, BuildAskRoundTripsThroughParse) {
  auto j = build_ask("front", 0, "what is the disk usage?");
  auto m = parse_ask(j.dump());
  ASSERT_TRUE(m.ok) << m.error;
  EXPECT_EQ(m.from, "front");
  EXPECT_EQ(m.hops, 0);
  EXPECT_EQ(m.message, "what is the disk usage?");
}

TEST(BridgeProtocol, BuildShareRoundTripsThroughParse) {
  auto j = build_share("front", "STATUS", nlohmann::json{{"cpu", 0.5}});
  auto m = parse_share(j.dump());
  ASSERT_TRUE(m.ok) << m.error;
  EXPECT_EQ(m.from, "front");
  EXPECT_EQ(m.key, "STATUS");
  EXPECT_EQ(m.value["cpu"], 0.5);
}

TEST(BridgeProtocol, ParseAskRejectsBadInput) {
  EXPECT_FALSE(parse_ask("not json").ok);
  EXPECT_FALSE(parse_ask("42").ok);                                        // non-object
  EXPECT_FALSE(parse_ask(R"({"v":2,"from":"a","hops":0,"message":"m"})").ok);   // version
  EXPECT_FALSE(parse_ask(R"({"v":"1","from":"a","hops":0,"message":"m"})").ok); // v not int
  EXPECT_FALSE(parse_ask(R"({"v":1,"hops":0,"message":"m"})").ok);         // no from
  EXPECT_FALSE(parse_ask(R"({"v":1,"from":"../x","hops":0,"message":"m"})").ok); // bad name
  EXPECT_FALSE(parse_ask(R"({"v":1,"from":"a","hops":-1,"message":"m"})").ok);  // neg hops
  EXPECT_FALSE(parse_ask(R"({"v":1,"from":"a","hops":"0","message":"m"})").ok); // hops not int
  EXPECT_FALSE(parse_ask(R"({"v":1,"from":"a","hops":0,"message":""})").ok);    // empty msg
  EXPECT_FALSE(parse_ask(R"({"v":1,"from":"a","hops":0,"message":7})").ok);     // non-string
}

TEST(BridgeProtocol, ParseAskIgnoresUnknownFields) {
  auto m = parse_ask(R"({"v":1,"from":"a","hops":0,"message":"m","future":"stuff"})");
  EXPECT_TRUE(m.ok) << m.error;   // forward compatibility: unknown fields ignored
}

TEST(BridgeProtocol, ParseShareRejectsBadInput) {
  EXPECT_FALSE(parse_share("{}").ok);
  EXPECT_FALSE(parse_share(R"({"v":1,"from":"a","value":1})").ok);          // no key
  EXPECT_FALSE(parse_share(R"({"v":1,"from":"a","key":"","value":1})").ok); // empty key
  EXPECT_FALSE(parse_share(R"({"v":1,"from":"a","key":"K K","value":1})").ok); // ws in key
  EXPECT_FALSE(parse_share(R"({"v":1,"from":"a","key":"K"})").ok);          // no value
  EXPECT_FALSE(parse_share(R"({"v":1,"from":"a","key":7,"value":1})").ok);  // key not string
}

TEST(BridgeProtocol, PeerBusKeyFormat) {
  EXPECT_EQ(peer_bus_key("front", "STATUS"), "PEER.front.STATUS");
}
