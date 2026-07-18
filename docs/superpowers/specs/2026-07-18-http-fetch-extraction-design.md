# http_fetch HTML→text extraction — design

**Date:** 2026-07-18
**Status:** approved (Vaios)
**Motivation:** `http_fetch` returns raw HTML — a typical page is mostly markup/script, so
every fetch burns tokens on noise and the 64 KB cap truncates the useful text away. Item 1
of the 2026-07-18 CC tool-gap analysis (CLAUDE.md).

## Decision summary

HTML responses are converted to readable text **by default**; `raw = true` returns the
untouched body. Conversion is a **zero-dependency in-tool C++ lexical stripper** (no DOM
parser, no external command — the musl static aarch64 build stays trivial). Hyperlinks are
kept **inline** as `label (url)` so the agent can navigate onward (pairs with the future
`web_search` tool).

## Tool contract (additive — no breaking change)

`http_fetch { url, raw? }`:

- `raw` — optional boolean, default `false`. Non-boolean value → treated as absent
  (house rule: adversarial/LLM-malformed args fail soft, never crash).
- Result: `{ status, body, truncated, extracted }`. New `extracted` boolean tells the
  LLM whether it received converted text or the raw body.

**HTML detection:** the response `Content-Type` header contains `text/html` or
`application/xhtml` (case-insensitive); if the header is absent, sniff the body prefix for
`<!doctype html` or `<html` (case-insensitive, leading whitespace allowed). Non-HTML
responses (JSON APIs, plain text, binaries) pass through untouched with `extracted:false` —
existing behavior byte-identical for them.

**Cap ordering:** extraction runs on the **full** fetched body, then the 64 KB cap applies
to the extracted text (`truncated` reflects the output). Cap-first would amputate a large
page's content before the text was pulled out of the markup. The `raw` path truncates the
body at 64 KB exactly as today. Cap stays `constexpr` (no manifest key — YAGNI).

**Describe text** (the LLM's API surface) documents the behavior:
"HTTP GET a URL. HTML pages are converted to readable text with links shown as
'label (url)'; pass raw=true for the unconverted body. Response truncated to 64 KB."

## Extractor

Pure function in a new **header-only** `include/hades/tool/html_text.h`
(`file_version.h` precedent — tool binaries include it without linking `hades_core`;
the test suite includes it directly):

```cpp
// namespace hades
std::string extract_html_text(const std::string& html);   // never throws
bool looks_like_html(const std::string& content_type, const std::string& body);
```

Single lexical pass over the bytes:

1. **Title first:** the `<title>…</title>` text (entity-decoded, whitespace-collapsed)
   becomes the first output line, followed by a blank line. Absent/empty title → no line.
2. **Dropped wholesale** (case-insensitive open→close, including contents):
   `<!-- -->` comments, `<script>`, `<style>`, `<head>`, `<noscript>`, `<template>`,
   `<svg>`. An unclosed drop-block drops to end-of-input (lexical, deterministic).
3. **Links:** `<a href="u">label</a>` → `label (u)`. Skip the `(u)` suffix when the href
   is empty, starts with `#`, or starts with `javascript:` (case-insensitive) — the label
   alone remains. Relative hrefs are kept as written (no base-URL resolution — the agent
   sees the page URL in its own conversation).
4. **Block boundaries → newline:** `p div li ul ol tr table h1..h6 br hr section article
   header footer blockquote pre` (open or close tag). `td`/`th` close → ` | `
   (crude table columns).
5. **Everything else:** remaining tags stripped; text content kept.
6. **Entities:** decode `&amp; &lt; &gt; &quot; &#39;/&apos; &nbsp;` (nbsp → plain space)
   plus numeric `&#NNN;` and `&#xHH;` → UTF-8 encoding of the code point (matters for
   Greek pages and typographic punctuation). Unknown named entities pass through
   literally. Invalid/oversized code points (`> 0x10FFFF`, surrogates) pass through
   literally — never emit invalid UTF-8 from the decoder.
7. **Whitespace:** runs of spaces/tabs collapse to one space; 3+ consecutive newlines
   collapse to 2; leading/trailing whitespace trimmed.

**Malformed HTML** yields imperfect text, never a crash or throw. **No emptiness
heuristic:** a JS-only page extracts to (near-)nothing and that is the returned result —
the describe text tells the agent `raw=true` exists; deterministic beats a magic fallback.

Note: the extracted output is later embedded in the tool's JSON reply via the standard
strict `dump()` path. Rule 6 guarantees the DECODER introduces no invalid UTF-8, but the
input page itself may contain invalid bytes that pass through — so the tool serializes its
reply with the house UTF-8-replace dump (`error_handler_t::replace`), same as
session_search.

## Security

Unchanged. Same `Net` capability classification, same CapabilityPolicy private-host gate on
the initial URL, redirects remain disabled (redirect-SSRF). `raw` has no security meaning.
The extractor runs on untrusted bytes: it is a pure string transform with no allocation
pattern beyond `std::string` append — fuzz-shaped unit tests (truncated tags, nested
brokenness, huge entity, null bytes) pin no-crash behavior under ASan/UBSan.

## Files

- `include/hades/tool/html_text.h` — new, header-only extractor + sniffer.
- `tools/http_fetch_main.cpp` — `raw` arg, detection, extract-then-cap, `extracted` field,
  describe text, replace-dump.
- `tests/test_html_text.cpp` — new: title line, drop-blocks, links (incl. skip-cases),
  block newlines/table pipes, entities (named/numeric/UTF-8/invalid), whitespace collapse,
  malformed/fuzz-shaped no-crash, `looks_like_html` (header hit, sniff hit, JSON miss).
- Existing http_fetch tool tests (if any exercise the binary) updated for the new field.
- Docs: `docs/manifest-reference.md` http_fetch row; CLAUDE.md gap-list item 1 → shipped.

## Non-goals (recorded, not built)

Readability-style main-content scoring · base-URL resolution of relative links ·
configurable cap · markdown output (headings/bold) · following redirects. Add when the
plain stripper proves insufficient in live use.

## Test plan

Pure extractor tests as above (the bulk). Tool-level (today http_fetch is describe-only in
`tests/test_tools.cpp`): describe schema includes `raw`, plus functional tests against a
loopback `httplib::Server` on an ephemeral port (the `test_ask_agent_tool.cpp` precedent —
the private-host gate lives in CapabilityPolicy at the Arbiter, not in the tool, so the
binary fetches loopback fine in tests): HTML body → converted + `extracted:true`;
`raw=true` → untouched; JSON body → passthrough + `extracted:false`.
