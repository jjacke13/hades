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

// From an <a ...> open tag's source, pull the href value. The attribute match requires a
// word boundary — whitespace before and '=' (spaces allowed) after — so hreflang= or
// data-href= never latch onto the wrong attribute. "" for anchors (#...), javascript:
// URLs, empty/missing hrefs — the label then stands alone.
inline std::string parse_href(const std::string& tag) {
  std::size_t h = 0;
  std::size_t eq = std::string::npos;
  while ((h = ifind(tag, "href", h)) != std::string::npos) {
    const bool left_ok = h > 0 && std::isspace(static_cast<unsigned char>(tag[h - 1]));
    std::size_t after = h + 4;
    while (after < tag.size() && std::isspace(static_cast<unsigned char>(tag[after]))) ++after;
    if (left_ok && after < tag.size() && tag[after] == '=') {
      eq = after;
      break;
    }
    ++h;
  }
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
