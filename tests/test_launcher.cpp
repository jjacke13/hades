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
