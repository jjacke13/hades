// tests/test_serve_config.cpp — Serve block resolution + defaults + CLI override
#include <gtest/gtest.h>
#include "hades/serve_config.h"
using namespace hades;

TEST(ServeConfig, DefaultsWhenNoBlock) {
  auto c = resolve_serve_config(parse_manifest("Session\n{\n}\n"), 0);
  EXPECT_EQ(c.host, "127.0.0.1");
  EXPECT_EQ(c.port, 8080);
  EXPECT_EQ(c.webroot, "web");
}
TEST(ServeConfig, ReadsBlockValues) {
  auto c = resolve_serve_config(
      parse_manifest("Serve {\n  host = 0.0.0.0\n  port = 9000\n  webroot = public\n}\n"), 0);
  EXPECT_EQ(c.host, "0.0.0.0");
  EXPECT_EQ(c.port, 9000);
  EXPECT_EQ(c.webroot, "public");
}
TEST(ServeConfig, CliPortOverridesBlock) {
  auto c = resolve_serve_config(parse_manifest("Serve {\n  port = 9000\n}\n"), 1234);
  EXPECT_EQ(c.port, 1234);
}
TEST(ServeConfig, InvalidPortFallsBackToDefault) {
  auto c = resolve_serve_config(parse_manifest("Serve {\n  port = not-a-port\n}\n"), 0);
  EXPECT_EQ(c.port, 8080);
}
TEST(ServeConfig, OutOfRangePortFallsBackToDefault) {
  auto c = resolve_serve_config(parse_manifest("Serve {\n  port = 65536\n}\n"), 0);
  EXPECT_EQ(c.port, 8080);
}
TEST(ServeConfig, NegativePortFallsBackToDefault) {
  auto c = resolve_serve_config(parse_manifest("Serve {\n  port = -1\n}\n"), 0);
  EXPECT_EQ(c.port, 8080);
}
TEST(ServeConfig, EmptyHostAndWebrootKeepDefaults) {
  auto c = resolve_serve_config(parse_manifest("Serve {\n  host = \n  webroot = \n}\n"), 0);
  EXPECT_EQ(c.host, "127.0.0.1");
  EXPECT_EQ(c.webroot, "web");
}
TEST(ServeConfig, CliPortOverridesDefaultsWithNoBlock) {
  auto c = resolve_serve_config(parse_manifest("Session\n{\n}\n"), 7000);
  EXPECT_EQ(c.port, 7000);
}
