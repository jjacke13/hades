# http_fetch HTML→text Extraction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `http_fetch` converts HTML responses to readable text by default (title first, links inline as `label (url)`), with `raw = true` returning the untouched body — so fetched pages stop burning tokens on markup.

**Architecture:** A pure, header-only lexical HTML stripper (`include/hades/tool/html_text.h`, `file_version.h` precedent — no core link) + a small change to `tools/http_fetch_main.cpp` (detect HTML by Content-Type/sniff, extract from the FULL body, then apply the 64 KB cap, report an `extracted` flag). Zero new dependencies — the musl static aarch64 build is untouched.

**Tech Stack:** C++20, nlohmann_json, cpr (tool), httplib (tests only), GoogleTest, CMake+Ninja in `nix develop`.

**Spec:** `docs/superpowers/specs/2026-07-18-http-fetch-extraction-design.md` (approved, committed `3e6c3df`).

## Global Constraints

- Every build/test command runs inside `nix develop`: `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure`. Baseline: **690/690 tests green** before Task 1.
- Branch `feat/http-fetch-extract` (already created; spec committed). Commit style `<type>: <desc>` — **NO attribution footer, NO Co-Authored-By**.
- The extractor **never throws** and never emits invalid UTF-8 of its own making (input bytes may pass through untouched); the tool's final JSON line is serialized with the house UTF-8-replace dump: `out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)`.
- Non-boolean `raw` arg = treated as absent (house rule: LLM-malformed args fail soft).
- Cap ordering: extraction runs on the **full** fetched body; the `constexpr` 64 KB cap applies to the OUTPUT (`truncated` reflects the output). Raw path caps the body exactly as today. No manifest key.
- Result field order/names: `{status, body, truncated, extracted}`.
- Describe text exactly: `HTTP GET a URL. HTML pages are converted to readable text with links shown as 'label (url)'; pass raw=true for the unconverted body. Response truncated to 64 KB.`
- Security invariants unchanged: `cpr::Redirect{false}` stays; no capability changes.
- File headers: `// path — one-line purpose` + short explanation block (house style).

---

## File Structure

```
include/hades/tool/html_text.h   T1  header-only: extract_html_text + looks_like_html + detail helpers
tests/test_html_text.cpp         T1  pure extractor tests (the bulk)
tools/http_fetch_main.cpp        T2  raw arg, detection, extract-then-cap, extracted field, describe, replace-dump
tests/test_http_fetch_tool.cpp   T2  functional tests vs loopback httplib server
CMakeLists.txt                   T1, T2
docs/manifest-reference.md       T3  http_fetch behavior paragraph (§4)
CLAUDE.md                        T3  gap-list item 1 shipped + feature note + test count
```

---

## Task 1: Header-only HTML→text extractor

**Files:**
- Create: `include/hades/tool/html_text.h`
- Test: `tests/test_html_text.cpp`
- Modify: `CMakeLists.txt` (one test-source line)

**Interfaces — Produces (all `namespace hades`, header-only `inline`):**
- `std::string extract_html_text(const std::string& html)` — never throws; empty in → empty out.
- `bool looks_like_html(const std::string& content_type, const std::string& body)` — header wins when present (`text/html`/`application/xhtml`, case-insensitive substring); non-empty other header → false; empty header → sniff body prefix (leading whitespace allowed) for `<!doctype html` or `<html` (case-insensitive).

- [ ] **Step 1: Write the failing tests** `tests/test_html_text.cpp`:

```cpp
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
  extract_html_text(std::string("<p>\x00 null\x00</p>", 16));
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
```

- [ ] **Step 2: Add to CMake and run — expect FAIL (missing header).** In `CMakeLists.txt`, next to the other `target_sources(hades_tests …)` lines (after the `tests/test_tools.cpp` line, ~line 100), add:

```cmake
target_sources(hades_tests PRIVATE tests/test_html_text.cpp)
```

Run: `nix develop --command cmake --build build` → compile error (`hades/tool/html_text.h` not found).

- [ ] **Step 3: Implement** `include/hades/tool/html_text.h`:

```cpp
// include/hades/tool/html_text.h — lexical HTML→readable-text extraction for http_fetch
//
// A zero-dependency, single-pass tag stripper — NOT a DOM parser. Title becomes the first
// line; script/style/head/noscript/template/svg/comment blocks are dropped wholesale; links
// render as "label (url)"; block tags become newlines and table cells " | "; entities
// (named + numeric) decode to UTF-8. Header-only (file_version.h precedent) so the
// standalone tool binary uses it without linking hades_core. Never throws; the decoder
// never EMITS invalid UTF-8 (invalid input bytes may pass through — the tool serializes
// its JSON reply with the UTF-8-replace dump). Malformed HTML yields imperfect text,
// deterministically: an unclosed drop-block drops to end-of-input, no fallback heuristics.
#pragma once
#include <cctype>
#include <cstdint>
#include <string>

namespace hades {
namespace html_detail {

inline char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

// Case-insensitive find of needle in hay starting at pos.
inline std::size_t ifind(const std::string& hay, const std::string& needle, std::size_t pos) {
  if (needle.empty() || hay.size() < needle.size()) return std::string::npos;
  for (std::size_t i = pos; i + needle.size() <= hay.size(); ++i) {
    std::size_t j = 0;
    while (j < needle.size() && lower(hay[i + j]) == lower(needle[j])) ++j;
    if (j == needle.size()) return i;
  }
  return std::string::npos;
}

// Append a code point as UTF-8. Rejects surrogates and > U+10FFFF (never emit invalid UTF-8).
inline bool append_utf8(std::string& out, std::uint32_t cp) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
  return true;
}

// Decode &amp; &lt; &gt; &quot; &#39;/&apos; &nbsp; (nbsp -> plain space) and numeric
// &#NNN;/&#xHH;. Unknown/invalid entities pass through literally.
inline std::string decode_entities(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  std::size_t i = 0;
  while (i < s.size()) {
    if (s[i] != '&') {
      out += s[i++];
      continue;
    }
    const std::size_t semi = s.find(';', i + 1);
    if (semi == std::string::npos || semi - i > 12) {   // no ';' nearby: not an entity
      out += s[i++];
      continue;
    }
    const std::string ent = s.substr(i + 1, semi - i - 1);
    bool decoded = true;
    if (ent == "amp") out += '&';
    else if (ent == "lt") out += '<';
    else if (ent == "gt") out += '>';
    else if (ent == "quot") out += '"';
    else if (ent == "apos" || ent == "#39") out += '\'';
    else if (ent == "nbsp") out += ' ';
    else if (!ent.empty() && ent[0] == '#') {
      std::uint32_t cp = 0;
      bool ok = false;
      std::size_t j = 1;
      const bool hex = j < ent.size() && (ent[j] == 'x' || ent[j] == 'X');
      if (hex) ++j;
      for (; j < ent.size(); ++j) {
        const char c = ent[j];
        std::uint32_t d;
        if (c >= '0' && c <= '9') d = static_cast<std::uint32_t>(c - '0');
        else if (hex && c >= 'a' && c <= 'f') d = static_cast<std::uint32_t>(10 + c - 'a');
        else if (hex && c >= 'A' && c <= 'F') d = static_cast<std::uint32_t>(10 + c - 'A');
        else { ok = false; break; }
        cp = cp * (hex ? 16u : 10u) + d;
        if (cp > 0x110000) { ok = false; break; }   // overflow guard: fail before wrapping
        ok = true;
      }
      decoded = ok && append_utf8(out, cp);
    } else {
      decoded = false;
    }
    if (decoded) i = semi + 1;
    else out += s[i++];   // keep the literal '&', re-scan the rest as plain text
  }
  return out;
}

// From an <a ...> open tag's source, pull the href value. "" for anchors (#...),
// javascript: URLs, empty/missing hrefs — the label then stands alone.
inline std::string parse_href(const std::string& tag) {
  const std::size_t h = ifind(tag, "href", 0);
  if (h == std::string::npos) return "";
  const std::size_t eq = tag.find('=', h + 4);
  if (eq == std::string::npos) return "";
  std::size_t v = eq + 1;
  while (v < tag.size() && std::isspace(static_cast<unsigned char>(tag[v]))) ++v;
  if (v >= tag.size()) return "";
  std::string url;
  if (tag[v] == '"' || tag[v] == '\'') {
    const char q = tag[v++];
    const std::size_t e = tag.find(q, v);
    if (e == std::string::npos) return "";
    url = tag.substr(v, e - v);
  } else {
    std::size_t e = v;
    while (e < tag.size() && !std::isspace(static_cast<unsigned char>(tag[e])) && tag[e] != '>')
      ++e;
    url = tag.substr(v, e - v);
  }
  if (url.empty() || url[0] == '#') return "";
  std::string lo;
  for (char c : url) lo += lower(c);
  if (lo.rfind("javascript:", 0) == 0) return "";
  return url;
}

inline bool is_block_tag(const std::string& name) {
  static const char* kBlocks[] = {"p",  "div",     "li",      "ul",     "ol",   "tr",
                                  "table", "h1",   "h2",      "h3",     "h4",   "h5",
                                  "h6", "br",      "hr",      "section", "article", "header",
                                  "footer", "blockquote", "pre"};
  for (const char* b : kBlocks)
    if (name == b) return true;
  return false;
}

// Collapse whitespace: \r/\t -> space, space runs -> one, spaces around newlines vanish,
// 3+ newlines -> 2, leading/trailing newlines trimmed.
inline std::string collapse_ws(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  int newlines = 0;
  bool space = false;
  for (char raw : s) {
    const char c = (raw == '\r' || raw == '\t') ? ' ' : raw;
    if (c == ' ') {
      space = true;
      continue;
    }
    if (c == '\n') {
      if (newlines < 2) {
        out += '\n';
        ++newlines;
      }
      space = false;
      continue;
    }
    if (space && !out.empty() && out.back() != '\n') out += ' ';
    space = false;
    out += c;
    newlines = 0;
  }
  const std::size_t b = out.find_first_not_of('\n');
  if (b == std::string::npos) return "";
  const std::size_t e = out.find_last_not_of('\n');
  return out.substr(b, e - b + 1);
}

}  // namespace html_detail

// True when the response should be treated as HTML: a Content-Type containing
// text/html or application/xhtml wins; any OTHER non-empty Content-Type is final
// (an html-looking JSON string must not flip it); absent header -> sniff the body
// prefix for <!doctype html or <html (leading whitespace allowed).
inline bool looks_like_html(const std::string& content_type, const std::string& body) {
  using html_detail::ifind;
  if (!content_type.empty()) {
    return ifind(content_type, "text/html", 0) != std::string::npos ||
           ifind(content_type, "application/xhtml", 0) != std::string::npos;
  }
  const std::size_t i = body.find_first_not_of(" \t\r\n");
  if (i == std::string::npos) return false;
  return ifind(body, "<!doctype html", 0) == i || ifind(body, "<html", 0) == i;
}

// The extractor. Single lexical pass; see the file header for the rules. Never throws.
inline std::string extract_html_text(const std::string& html) {
  using namespace html_detail;
  std::string text;
  text.reserve(html.size() / 4);

  // Title pre-pass: <title> lives inside <head>, which the main pass drops wholesale.
  {
    const std::size_t t = ifind(html, "<title", 0);
    if (t != std::string::npos) {
      const std::size_t gt = html.find('>', t);
      const std::size_t end = (gt == std::string::npos) ? std::string::npos
                                                        : ifind(html, "</title", gt + 1);
      if (gt != std::string::npos && end != std::string::npos) {
        text += html.substr(gt + 1, end - gt - 1);
        text += "\n\n";
      }
    }
  }

  static const char* kDrop[] = {"script", "style", "head", "noscript", "template", "svg",
                                "title"};   // title: already taken by the pre-pass
  std::string pending_href;   // set at <a href=...>, emitted at </a>
  std::size_t i = 0;
  while (i < html.size()) {
    if (html[i] != '<') {
      text += html[i++];
      continue;
    }
    if (html.compare(i, 4, "<!--") == 0) {   // comment (before generic <! handling)
      const std::size_t e = html.find("-->", i + 4);
      i = (e == std::string::npos) ? html.size() : e + 3;
      continue;
    }
    if (i + 1 < html.size() && (html[i + 1] == '!' || html[i + 1] == '?')) {
      const std::size_t e = html.find('>', i);   // <!DOCTYPE ...>, <?xml ...?>
      i = (e == std::string::npos) ? html.size() : e + 1;
      continue;
    }
    std::size_t j = i + 1;
    const bool closing = j < html.size() && html[j] == '/';
    if (closing) ++j;
    const std::size_t name_start = j;
    while (j < html.size() && std::isalnum(static_cast<unsigned char>(html[j]))) ++j;
    if (j == name_start) {   // "<" not opening a tag ("a < b") — literal text
      text += html[i++];
      continue;
    }
    std::string name;
    for (std::size_t k = name_start; k < j; ++k) name += lower(html[k]);
    const std::size_t gt = html.find('>', j);
    const std::size_t tag_end = (gt == std::string::npos) ? html.size() : gt + 1;

    if (!closing) {
      bool dropped = false;
      for (const char* d : kDrop) {
        if (name != d) continue;
        const std::size_t close = ifind(html, "</" + name, tag_end);
        if (close == std::string::npos) {
          i = html.size();   // unclosed drop-block: drop to end (lexical, deterministic)
        } else {
          const std::size_t cgt = html.find('>', close);
          i = (cgt == std::string::npos) ? html.size() : cgt + 1;
        }
        dropped = true;
        break;
      }
      if (dropped) continue;
      if (name == "a") pending_href = parse_href(html.substr(i, tag_end - i));
      if (is_block_tag(name)) text += '\n';
    } else {
      if (name == "a") {
        if (!pending_href.empty()) {
          text += " (" + pending_href + ")";
          pending_href.clear();
        }
      } else if (name == "td" || name == "th") {
        text += " | ";
      } else if (is_block_tag(name)) {
        text += '\n';
      }
    }
    i = tag_end;
  }

  return collapse_ws(decode_entities(text));
}

}  // namespace hades
```

- [ ] **Step 4: Build + test.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R HtmlText` → all pass. Then the full suite → 690 baseline + new, all green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/tool/html_text.h tests/test_html_text.cpp CMakeLists.txt
git commit -m "feat: header-only HTML→text extractor (title, links inline, entities, drop-blocks)"
```

---

## Task 2: http_fetch integration + loopback functional tests

**Files:**
- Modify: `tools/http_fetch_main.cpp` (whole file shown below)
- Create: `tests/test_http_fetch_tool.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `hades::extract_html_text(const std::string&)`, `hades::looks_like_html(const std::string& content_type, const std::string& body)` from `hades/tool/html_text.h` (Task 1, header-only — no core link).
- Produces: `http_fetch` result `{status, body, truncated, extracted}`; schema property `raw` (boolean, optional). cpr's `r.header` is a case-insensitive map — `r.header.find("Content-Type")` matches any casing.

- [ ] **Step 1: Write the failing tests** `tests/test_http_fetch_tool.cpp`:

```cpp
// tests/test_http_fetch_tool.cpp — drive hades-http-fetch against a loopback httplib server
//
// The private-host gate lives in CapabilityPolicy at the Arbiter, NOT in the tool, so the
// binary fetches loopback fine here (the test_ask_agent_tool.cpp precedent). Serves canned
// HTML/JSON; asserts default extraction, the raw escape, and non-HTML passthrough.
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "hades/tool/subprocess.h"
using namespace hades;

namespace {
struct StubWeb {
  httplib::Server srv;
  int port = 0;
  std::thread th;
  StubWeb() {
    srv.Get("/page", [](const httplib::Request&, httplib::Response& res) {
      res.set_content(
          "<html><head><title>T</title><script>var x=1;</script></head>"
          "<body><p>Hello &amp; welcome</p><a href=\"/next\">Next</a></body></html>",
          "text/html");
    });
    srv.Get("/api", [](const httplib::Request&, httplib::Response& res) {
      res.set_content(R"({"k":"v"})", "application/json");
    });
    port = srv.bind_to_any_port("127.0.0.1");
    th = std::thread([this] { srv.listen_after_bind(); });
    srv.wait_until_ready();
  }
  ~StubWeb() {
    srv.stop();
    th.join();
  }
  std::string url(const std::string& path) {
    return "http://127.0.0.1:" + std::to_string(port) + path;
  }
};

nlohmann::json fetch(const nlohmann::json& args) {
  nlohmann::json req{{"call", "http_fetch"}, {"args", args}};
  ProcResult r = run_subprocess({HTTP_FETCH_BIN}, req.dump(), 30.0);
  return nlohmann::json::parse(r.out, nullptr, false);
}
}  // namespace

TEST(HttpFetchTool, HtmlIsExtractedByDefault) {
  StubWeb web;
  auto j = fetch({{"url", web.url("/page")}});
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_TRUE(j["result"].value("extracted", false));
  const std::string body = j["result"].value("body", "");
  EXPECT_EQ(body.rfind("T\n", 0), 0u);                          // title first line
  EXPECT_NE(body.find("Hello & welcome"), std::string::npos);   // entity decoded
  EXPECT_NE(body.find("Next (/next)"), std::string::npos);      // link inline
  EXPECT_EQ(body.find("var x=1"), std::string::npos);           // script dropped
}

TEST(HttpFetchTool, RawFlagReturnsUntouchedBody) {
  StubWeb web;
  auto j = fetch({{"url", web.url("/page")}, {"raw", true}});
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_FALSE(j["result"].value("extracted", true));
  EXPECT_NE(j["result"].value("body", "").find("<script>var x=1;</script>"),
            std::string::npos);
}

TEST(HttpFetchTool, NonHtmlPassesThroughUntouched) {
  StubWeb web;
  auto j = fetch({{"url", web.url("/api")}});
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_FALSE(j["result"].value("extracted", true));
  EXPECT_EQ(j["result"].value("body", ""), R"({"k":"v"})");
}

TEST(HttpFetchTool, NonBooleanRawTreatedAsAbsent) {
  StubWeb web;
  auto j = fetch({{"url", web.url("/page")}, {"raw", "yes"}});   // string, not bool
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_TRUE(j["result"].value("extracted", false));            // still extracts
}

TEST(HttpFetchTool, DescribeIncludesRawProperty) {
  ProcResult r = run_subprocess({HTTP_FETCH_BIN}, R"({"call":"describe"})", 30.0);
  auto j = nlohmann::json::parse(r.out, nullptr, false);
  ASSERT_TRUE(j.is_object() && j.value("ok", false));
  EXPECT_TRUE(j["result"]["schema"]["properties"].contains("raw"));
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** In `CMakeLists.txt`:

After the `add_executable(hades-http-fetch …)` / `target_link_libraries(hades-http-fetch …)` pair (~line 85-86), add the include dir (the binary now includes `hades/tool/html_text.h`; `use_skill` precedent):

```cmake
target_include_directories(hades-http-fetch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
```

Next to the Task 1 test-source line, add:

```cmake
target_sources(hades_tests PRIVATE tests/test_http_fetch_tool.cpp)
```

(`HTTP_FETCH_BIN` compile-def and the `add_dependencies(hades_tests … hades-http-fetch)` already exist, ~lines 105-106. `hades_tests` gets httplib transitively through `hades_core`.)

Run: `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R HttpFetchTool` → new tests FAIL (`extracted` missing, describe lacks `raw`).

- [ ] **Step 3: Implement — replace `tools/http_fetch_main.cpp` with:**

```cpp
// tools/http_fetch_main.cpp — bundled http_fetch native tool binary
//
// Reads one JSON line ({"call":"describe"|"http_fetch","args":{url,raw?}}), does an HTTP
// GET via cpr, and returns one JSON line. HTML responses (Content-Type, or body sniff when
// the header is absent) are converted to readable text by default — title first, links
// inline as "label (url)" — pass raw=true for the untouched body. Extraction runs on the
// FULL body, THEN the 64 KB cap applies to the output; non-HTML bodies pass through capped
// as before. Spawned as a subprocess by ToolRunner; read-only web egress; guarded so a
// malformed request never throws. Reply uses the UTF-8-replace dump: the page's bytes are
// untrusted and a cap cut can land mid-codepoint.
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>
#include <cpr/cpr.h>
#include "hades/tool/html_text.h"

int main() {
  std::string line;
  std::getline(std::cin, line);
  auto in = nlohmann::json::parse(line, nullptr, false);

  nlohmann::json out;
  std::string call = in.is_object() ? in.value("call", "") : "";

  if (call == "describe") {
    out = {{"ok", true},
           {"result",
            {{"name", "http_fetch"},
             {"description",
              "HTTP GET a URL. HTML pages are converted to readable text with links shown "
              "as 'label (url)'; pass raw=true for the unconverted body. Response truncated "
              "to 64 KB."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"url", {{"type", "string"}}},
                 {"raw",
                  {{"type", "boolean"},
                   {"description", "return the unconverted response body"}}}}},
               {"required", {"url"}}}}}}};
  } else if (call == "http_fetch") {
    nlohmann::json args =
        (in.is_object() && in.contains("args") && in["args"].is_object())
            ? in["args"]
            : nlohmann::json::object();
    std::string url = args.value("url", "");
    // Non-boolean raw counts as absent (LLM-malformed args fail soft, never crash).
    const bool raw = args.contains("raw") && args["raw"].is_boolean() && args["raw"].get<bool>();
    if (url.empty()) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: url"}}}};
    } else {
      // Redirects are DISABLED so the CapabilityPolicy host-gate (which classifies only the
      // initial URL's host) cannot be bypassed by a 3xx Location to a private/loopback host
      // (redirect-SSRF). A 3xx now returns the redirect response unfollowed.
      cpr::Response r = cpr::Get(cpr::Url{url}, cpr::Timeout{30000}, cpr::Redirect{false});
      std::string content_type;
      if (auto it = r.header.find("Content-Type"); it != r.header.end())
        content_type = it->second;   // cpr header map is case-insensitive
      std::string body = r.text;
      bool extracted = false;
      if (!raw && hades::looks_like_html(content_type, body)) {
        body = hades::extract_html_text(body);   // full body first; cap applies to output
        extracted = true;
      }
      constexpr std::size_t kCap = 64 * 1024;
      bool truncated = body.size() > kCap;
      if (truncated) body.resize(kCap);
      out = {{"ok", r.status_code > 0},
             {"result",
              {{"status", static_cast<int>(r.status_code)},
               {"body", body},
               {"truncated", truncated},
               {"extracted", extracted}}}};
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }

  std::cout << out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)
            << std::endl;
  return 0;
}
```

- [ ] **Step 4: Build + test.** `-R HttpFetchTool` → pass; `-R Tools` (existing describe test) → pass; full suite green.
- [ ] **Step 5: Commit.**

```bash
git add tools/http_fetch_main.cpp tests/test_http_fetch_tool.cpp CMakeLists.txt
git commit -m "feat: http_fetch extracts HTML to readable text by default (raw=true escape)"
```

---

## Task 3: Ship — docs

**Files:**
- Modify: `docs/manifest-reference.md` (§4, after the dev-tools paragraph ~line 183)
- Modify: `CLAUDE.md` (gap-list item 1, current-state header note)

- [ ] **Step 1: manifest-reference.** In §4, directly AFTER the dev-tools paragraph (the one ending "All five are plain `native` blocks.") and BEFORE the staleness-guard paragraph, insert:

```markdown
**`http_fetch` HTML extraction (always on, no configuration).** HTML responses (by
`Content-Type`, or a body sniff when the header is absent) are converted to readable text
before being returned: the page title first, links inline as `label (url)`, scripts/styles
dropped, entities decoded. The LLM can pass `raw = true` for the unconverted body. The
result carries an `extracted` flag; the 64 KB cap applies to the returned text (extraction
runs on the full body first). Non-HTML responses (JSON APIs, plain text) pass through
untouched. Security posture is unchanged — same Net capability verdicts (§5), redirects
still disabled.
```

- [ ] **Step 2: CLAUDE.md.** In the `### CC tool-gap analysis` section, item 1 becomes:

```markdown
1. ~~**http_fetch text extraction**~~ — **SHIPPED 2026-07-18** (`feat/http-fetch-extract`): HTML→text
   by default (title first, links `label (url)`, entities→UTF-8), `raw=true` escape, `extracted`
   result flag, extract-then-cap; zero-dep header-only `include/hades/tool/html_text.h`; loopback
   httplib functional tests. Non-HTML passthrough byte-identical.
```

Also update the current-state header's test count (690 → the new total from Task 2's full-suite run).

- [ ] **Step 3: Full build + suite.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure` → ALL green. Note the final count for the commit message.
- [ ] **Step 4: Commit.**

```bash
git add docs/manifest-reference.md CLAUDE.md
git commit -m "docs: http_fetch HTML extraction — manifest-reference §4 + CLAUDE.md ship notes"
```

---

## Verification (end-to-end)

1. Full suite in `nix develop`: 690 baseline + ~24 new, all green (ASan+UBSan lane; the
   feature is single-threaded — TSan lane at the final review's discretion).
2. Manual live smoke (Vaios, needs `HADES_API_KEY`): `fetch https://example.com` → readable
   text, no tags; `fetch it raw` → markup; fetch a JSON API → untouched.
3. Security spot-check: private-host deny still comes from CapabilityPolicy (unchanged);
   `hades-scope session.log TOOL_` shows `extracted:true` on HTML fetches.

## Execution

Subagent-driven development (house process): fresh implementer per task, per-task review,
final whole-branch review, then finishing-a-development-branch (merge ff to main — push only
on Vaios's word).
