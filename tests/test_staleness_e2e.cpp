// tests/test_staleness_e2e.cpp — lost-update protection end-to-end with the REAL tool binaries:
// read -> external modification -> edit REFUSED (file intact) -> re-read -> edit succeeds.
#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "hades/arbiter.h"
#include "hades/blackboard.h"
#include "hades/module/tool_runner.h"
using namespace hades;

static std::string slurp(const std::string& p) {
  std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str();
}

TEST(StalenessE2E, ExternalChangeRefusedThenRereadSucceeds) {
  const std::string path =
      ::testing::TempDir() + "/stale_e2e_" + std::to_string(::getpid()) + ".txt";
  { std::ofstream f(path, std::ios::trunc); f << "line one\nline two\n"; }

  Blackboard bb;
  ToolRunner tools;
  Block fs;   fs.section = "Tool"; fs.name = "fs_read";    fs.kv["native"] = FS_READ_BIN;
  Block ed;   ed.section = "Tool"; ed.name = "edit_file";  ed.kv["native"] = EDIT_FILE_BIN;
  tools.add_tool(fs); tools.add_tool(ed);
  tools.on_start(Block{}, bb); tools.on_attach(bb);
  Arbiter a; a.on_attach(bb);
  a.set_tools(tools.registry().specs());

  nlohmann::json last_result;
  bb.subscribe("TOOL_RESULT", [&](const Entry& e) { last_result = e.value; });
  bb.post("USER_MESSAGE", "work on the file", "chat"); bb.pump();

  // 1) LLM reads the file (real fs_read runs; Arbiter records its version).
  bb.post("LLM_RESPONSE", {{"text", ""}, {"epoch", 1},
      {"tool_call", {{"id", "r1"}, {"name", "fs_read"}, {"arguments", {{"path", path}}}}}}, "llm");
  bb.pump();
  ASSERT_TRUE(last_result.value("ok", false));

  // 2) The file changes EXTERNALLY (another turn / a human / a heartbeat).
  { std::ofstream f(path, std::ios::trunc); f << "line one CHANGED\nline two\n"; }

  // 3) The stale edit is REFUSED and the file is untouched.
  bb.post("LLM_RESPONSE", {{"text", ""}, {"epoch", 1},
      {"tool_call", {{"id", "e1"}, {"name", "edit_file"},
                     {"arguments", {{"path", path}, {"old_string", "line two"}, {"new_string", "LINE 2"}}}}}}, "llm");
  bb.pump();
  EXPECT_FALSE(last_result.value("ok", true));
  EXPECT_NE(last_result["content"].value("error", "").find("changed on disk"), std::string::npos);
  EXPECT_EQ(slurp(path), "line one CHANGED\nline two\n");

  // 4) Re-read, then the same edit succeeds.
  bb.post("LLM_RESPONSE", {{"text", ""}, {"epoch", 1},
      {"tool_call", {{"id", "r2"}, {"name", "fs_read"}, {"arguments", {{"path", path}}}}}}, "llm");
  bb.pump();
  bb.post("LLM_RESPONSE", {{"text", ""}, {"epoch", 1},
      {"tool_call", {{"id", "e2"}, {"name", "edit_file"},
                     {"arguments", {{"path", path}, {"old_string", "line two"}, {"new_string", "LINE 2"}}}}}}, "llm");
  bb.pump();
  EXPECT_TRUE(last_result.value("ok", false)) << last_result.dump();
  EXPECT_EQ(slurp(path), "line one CHANGED\nLINE 2\n");
}
