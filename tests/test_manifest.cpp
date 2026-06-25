#include <gtest/gtest.h>
#include "hades/config.h"
using namespace hades;
const char* SRC = R"(
# demo
Session
{
  name        = hades-dev
  api_key_env = HADES_API_KEY
}
Module = llm
Tool = fs { native = ./tools/hades-fs-read }
Objective = stay_on_budget
{
  hard_cap_usd = 1.5
}
)";
TEST(Manifest, ParsesBlocksKeysAndInline) {
  Manifest m = parse_manifest(SRC);
  ASSERT_TRUE(m.session().has_value());
  EXPECT_EQ(m.session()->kv.at("name"), "hades-dev");
  auto mods = m.of("Module");
  ASSERT_EQ(mods.size(), 1u);
  EXPECT_EQ(mods[0].name, "llm");
  EXPECT_TRUE(mods[0].kv.empty());
  auto tools = m.of("tool");                 // case-insensitive section
  ASSERT_EQ(tools.size(), 1u);
  EXPECT_EQ(tools[0].kv.at("native"), "./tools/hades-fs-read");
  EXPECT_EQ(m.of("Objective")[0].kv.at("hard_cap_usd"), "1.5");
}
TEST(Manifest, Validators) {
  double d=0; bool b=false;
  EXPECT_TRUE(set_double_on_string("1.5", d)); EXPECT_DOUBLE_EQ(d,1.5);
  EXPECT_FALSE(set_double_on_string("xx", d));
  EXPECT_FALSE(set_pos_double_on_string("-2", d));
  EXPECT_TRUE(set_bool_on_string("true", b)); EXPECT_TRUE(b);
}
