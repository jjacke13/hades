// tests/test_launcher.cpp — unit tests for Launcher factory registration and build
//
// Verifies that Launcher builds Module instances from a Manifest via registered
// factories (calling on_start), throws MalConfig for unknown types, and clears
// all modules on shutdown — the pAntler-analog wiring exercised in isolation.

#include <gtest/gtest.h>
#include "hades/launcher.h"
#include "hades/blackboard.h"
using namespace hades;
struct FakeMod : Module {
  std::string t; bool started=false;
  explicit FakeMod(std::string t):t(std::move(t)){}
  std::string type() const override { return t; }
  void on_start(const Block&, Blackboard&) override { started=true; }
};
TEST(Launcher, BuildsRegisteredModulesFromManifest) {
  Blackboard bb; Launcher L(bb);
  L.register_factory("llm", []{ return std::make_unique<FakeMod>("llm"); });
  Manifest m = parse_manifest("Module = llm\n");
  L.build(m);
  ASSERT_EQ(L.modules().size(), 1u);
  EXPECT_EQ(L.modules()[0]->type(), "llm");
  EXPECT_TRUE(static_cast<FakeMod*>(L.modules()[0])->started);
}
TEST(Launcher, UnknownModuleTypeIsMalConfig) {
  Blackboard bb; Launcher L(bb);
  Manifest m = parse_manifest("Module = ghost\n");
  EXPECT_THROW(L.build(m), MalConfig);
}
TEST(Launcher, ShutdownClearsModules) {
  Blackboard bb; Launcher L(bb);
  L.register_factory("llm", []{ return std::make_unique<FakeMod>("llm"); });
  L.build(parse_manifest("Module = llm\n"));
  ASSERT_EQ(L.modules().size(), 1u);
  L.shutdown();
  EXPECT_TRUE(L.modules().empty());
}
TEST(Launcher, InstantiateBuildsRosterAndHasByType) {
  Blackboard bb; Launcher L(bb);
  L.register_factory("a", []{ return std::make_unique<FakeMod>("a"); });
  L.register_factory("b", []{ return std::make_unique<FakeMod>("b"); });
  L.instantiate(parse_manifest("Module = a\nModule = b\n"));
  EXPECT_TRUE(L.has("a"));
  EXPECT_TRUE(L.has("b"));
  EXPECT_FALSE(L.has("c"));
}
TEST(Launcher, TakeTransfersOwnershipOnceByType) {
  Blackboard bb; Launcher L(bb);
  L.register_factory("a", []{ return std::make_unique<FakeMod>("a"); });
  L.instantiate(parse_manifest("Module = a\n"));
  auto m1 = L.take("a");
  ASSERT_NE(m1, nullptr);
  EXPECT_EQ(m1->type(), "a");
  EXPECT_EQ(L.take("a"), nullptr);   // already taken
  EXPECT_FALSE(L.has("a"));
}
TEST(Launcher, InstantiateUnknownTypeThrowsMalConfig) {
  Blackboard bb; Launcher L(bb);
  L.register_factory("a", []{ return std::make_unique<FakeMod>("a"); });
  EXPECT_THROW(L.instantiate(parse_manifest("Module = ghost\n")), MalConfig);
}
TEST(Launcher, ModulesSkipsTakenSlots) {
  Blackboard bb; Launcher L(bb);
  L.register_factory("a", []{ return std::make_unique<FakeMod>("a"); });
  L.register_factory("b", []{ return std::make_unique<FakeMod>("b"); });
  L.instantiate(parse_manifest("Module = a\nModule = b\n"));
  auto taken = L.take("a");
  ASSERT_NE(taken, nullptr);
  auto mods = L.modules();
  EXPECT_EQ(mods.size(), 1u);          // the taken slot is filtered out
  for (auto* m : mods) { ASSERT_NE(m, nullptr); EXPECT_EQ(m->type(), "b"); }
}
