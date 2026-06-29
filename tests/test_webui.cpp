// tests/test_webui.cpp — the static web assets exist and wire the HTTP JSON API
#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include <string>
static std::string slurp(const std::string& p) {
  std::ifstream f(p);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}
TEST(WebUI, IndexLinksAssets) {
  std::string html = slurp(WEB_INDEX);
  ASSERT_FALSE(html.empty());
  EXPECT_NE(html.find("style.css"), std::string::npos);
  EXPECT_NE(html.find("app.js"), std::string::npos);
}
TEST(WebUI, AppJsWiresChatConfirmAndControls) {
  std::string js = slurp(WEB_APP);
  ASSERT_FALSE(js.empty());
  EXPECT_NE(js.find("/chat"), std::string::npos);
  EXPECT_NE(js.find("/confirm"), std::string::npos);
  EXPECT_NE(js.find("needs_confirm"), std::string::npos);
  EXPECT_NE(js.find("Approve"), std::string::npos);
  EXPECT_NE(js.find("Deny"), std::string::npos);
  EXPECT_NE(js.find("X-Hades"), std::string::npos);   // CSRF guard header sent by the page
}
TEST(WebUI, StyleNonEmpty) {
  EXPECT_FALSE(slurp(WEB_STYLE).empty());
}
