// tests/test_html_text.cpp — pure HTML→text extractor behind http_fetch's default conversion
#include <gtest/gtest.h>
#include <string>
#include "hades/tool/html_text.h"
using namespace hades;

TEST(HtmlText, TitleBecomesFirstLine) {
  const std::string out = extract_html_text(
      "<html><head><title> My Page </title></head><body><p>Body text</p></body></html>");
  EXPECT_EQ(out.rfind("My Page\n", 0), 0u);
  EXPECT_NE(out.find("Body text"), std::string::npos);
  // title appears ONCE (pre-pass grabs it, main pass drops the <title> block)
  EXPECT_EQ(out.find("My Page"), out.rfind("My Page"));
}

TEST(HtmlText, NoTitleNoLeadingLine) {
  EXPECT_EQ(extract_html_text("<body><p>Just text</p></body>"), "Just text");
}

TEST(HtmlText, ScriptStyleHeadCommentsDropped) {
  const std::string out = extract_html_text(
      "<head><meta name=\"k\" content=\"v\"><style>p{color:red}</style></head>"
      "<body><!-- secret --><script>var x=1;</script><noscript>enable js</noscript>"
      "<template><b>tpl</b></template><svg><path d=\"M0\"/></svg><p>Kept</p></body>");
  EXPECT_EQ(out, "Kept");
}

TEST(HtmlText, UnclosedDropBlockDropsToEnd) {
  EXPECT_EQ(extract_html_text("<p>before</p><script>var x=1; // never closed"), "before");
}

TEST(HtmlText, LinksInlineWithUrl) {
  const std::string out =
      extract_html_text("<a href=\"https://x.example/d?a=1&amp;b=2\">Docs</a>");
  EXPECT_EQ(out, "Docs (https://x.example/d?a=1&b=2)");
}

TEST(HtmlText, AnchorJsAndEmptyHrefsKeepLabelOnly) {
  EXPECT_EQ(extract_html_text("<a href=\"#top\">Top</a>"), "Top");
  EXPECT_EQ(extract_html_text("<a href=\"JavaScript:void(0)\">Click</a>"), "Click");
  EXPECT_EQ(extract_html_text("<a href=\"\">Blank</a>"), "Blank");
  EXPECT_EQ(extract_html_text("<a>NoHref</a>"), "NoHref");
}

TEST(HtmlText, UnquotedAndSingleQuotedHrefs) {
  EXPECT_EQ(extract_html_text("<a href=/rel>R</a>"), "R (/rel)");
  EXPECT_EQ(extract_html_text("<a href='/sq'>S</a>"), "S (/sq)");
}

TEST(HtmlText, BlockTagsBecomeNewlines) {
  // Open AND close of a block tag emit a newline (robust against old-style HTML that
  // uses <p> as a separator without closes); runs collapse to a blank line at most.
  const std::string out =
      extract_html_text("<h1>Head</h1><p>One</p><ul><li>A</li><li>B</li></ul>");
  EXPECT_EQ(out, "Head\n\nOne\n\nA\n\nB");
}

TEST(HtmlText, TableCellsBecomePipes) {
  const std::string out =
      extract_html_text("<table><tr><td>a</td><td>b</td></tr><tr><td>c</td><td>d</td></tr></table>");
  EXPECT_NE(out.find("a | b |"), std::string::npos);
  EXPECT_NE(out.find("c | d |"), std::string::npos);
  EXPECT_NE(out.find('\n'), std::string::npos);   // rows on separate lines
}

TEST(HtmlText, NamedEntitiesDecoded) {
  EXPECT_EQ(extract_html_text("a &amp; b &lt;c&gt; &quot;d&quot; &#39;e&#39; &apos;f&apos;"),
            "a & b <c> \"d\" 'e' 'f'");
}

TEST(HtmlText, NbspBecomesPlainSpaceAndCollapses) {
  EXPECT_EQ(extract_html_text("a&nbsp;&nbsp;&nbsp;b"), "a b");
}

TEST(HtmlText, NumericEntitiesDecodeToUtf8) {
  EXPECT_EQ(extract_html_text("&#945;&#946;"), "\xCE\xB1\xCE\xB2");        // αβ (Greek)
  EXPECT_EQ(extract_html_text("&#x3B1;"), "\xCE\xB1");                     // hex form
  EXPECT_EQ(extract_html_text("&#8217;"), "\xE2\x80\x99");                 // typographic ’
}

TEST(HtmlText, InvalidEntitiesPassThroughLiterally) {
  EXPECT_EQ(extract_html_text("&#x110000; &#xD800; &notanentity; &frac12;"),
            "&#x110000; &#xD800; &notanentity; &frac12;");
  EXPECT_EQ(extract_html_text("&#999999999999999999;"), "&#999999999999999999;");
  EXPECT_EQ(extract_html_text("stray & ampersand"), "stray & ampersand");
}

TEST(HtmlText, WhitespaceCollapsed) {
  const std::string out = extract_html_text(
      "<p>a    b</p>\n\n\n<p>c</p><div></div><div></div><div></div><p>d</p>");
  EXPECT_EQ(out, "a b\n\nc\n\nd");   // space runs -> one; 3+ newlines -> 2 max
}

TEST(HtmlText, LiteralLessThanInTextKept) {
  EXPECT_EQ(extract_html_text("if a < b then"), "if a < b then");
}

TEST(HtmlText, DoctypeAndProcessingInstructionsDropped) {
  EXPECT_EQ(extract_html_text("<!DOCTYPE html><?xml version=\"1.0\"?><p>x</p>"), "x");
}

TEST(HtmlText, MalformedNoCrash) {
  // fuzz-shaped: none may throw or crash (ASan/UBSan lane enforces memory safety)
  extract_html_text("");
  extract_html_text("<");
  extract_html_text("<div class=\"x");                       // truncated open tag
  extract_html_text("<a href=");                             // truncated href
  extract_html_text("<a href=\"u\">no close");
  extract_html_text("<!--");                                 // unclosed comment
  extract_html_text(std::string("<p>\x00 null\x00</p>", 14));   // 14 = the literal's true length
  extract_html_text("<<<>>><//><p</p>");
  std::string big(300000, 'x');
  extract_html_text("<p>" + big + "</p>");
  SUCCEED();
}

TEST(HtmlText, LooksLikeHtmlHeaderWins) {
  EXPECT_TRUE(looks_like_html("text/html; charset=utf-8", ""));
  EXPECT_TRUE(looks_like_html("application/xhtml+xml", ""));
  EXPECT_TRUE(looks_like_html("TEXT/HTML", ""));
  // explicit non-HTML type: html-looking body must NOT flip it
  EXPECT_FALSE(looks_like_html("application/json", "<html><body>x</body></html>"));
  EXPECT_FALSE(looks_like_html("text/plain", "<!doctype html>"));
}

TEST(HtmlText, LooksLikeHtmlSniffsWhenHeaderAbsent) {
  EXPECT_TRUE(looks_like_html("", "  \n<!DOCTYPE html><html>"));
  EXPECT_TRUE(looks_like_html("", "<HTML lang=\"en\">"));
  EXPECT_FALSE(looks_like_html("", "{\"k\":\"v\"}"));
  EXPECT_FALSE(looks_like_html("", ""));
}
