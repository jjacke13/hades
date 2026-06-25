#include <gtest/gtest.h>
#include "hades/llm/openai_compat_provider.h"
using namespace hades;
TEST(Provider, BuildsBodyWithModelMessagesTools) {
  OpenAICompatProvider p("https://x/v1","k","m", [](auto,auto,auto){ return HttpResponse{200,"{}"}; });
  LlmRequest req; req.messages={{{"role","user"},{"content","hi"}}};
  req.tools={ ToolSpec{"fs_read","read a file", {{"type","object"}}} };
  auto body=p.build_body(req);
  EXPECT_EQ(body["model"], "m");
  EXPECT_EQ(body["messages"][0]["content"], "hi");
  EXPECT_EQ(body["tools"][0]["function"]["name"], "fs_read");
}
TEST(Provider, ParsesTextAndUsage) {
  std::string canned = R"({"choices":[{"message":{"content":"hello"},"finish_reason":"stop"}],
                           "usage":{"prompt_tokens":10,"completion_tokens":3}})";
  OpenAICompatProvider p("https://x/v1","k","m",
    [&](auto,auto,auto){ return HttpResponse{200,canned}; });
  auto r=p.complete({});
  EXPECT_EQ(r.text,"hello"); EXPECT_FALSE(r.tool_call.has_value());
  EXPECT_EQ(r.prompt_tokens,10); EXPECT_EQ(r.completion_tokens,3);
}
TEST(Provider, ParsesToolCall) {
  std::string canned = R"({"choices":[{"message":{"content":null,"tool_calls":[
    {"id":"c1","function":{"name":"fs_read","arguments":"{\"path\":\"/a\"}"}}]},
    "finish_reason":"tool_calls"}],"usage":{}})";
  OpenAICompatProvider p("https://x/v1","k","m",[&](auto,auto,auto){ return HttpResponse{200,canned}; });
  auto r=p.complete({});
  ASSERT_TRUE(r.tool_call.has_value());
  EXPECT_EQ((*r.tool_call)["name"],"fs_read");
  EXPECT_EQ((*r.tool_call)["arguments"]["path"],"/a");
}
