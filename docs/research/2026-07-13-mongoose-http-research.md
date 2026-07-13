# Mongoose (Cesanta) as hades's HTTP Layer — Research & Recommendation

**Date:** 2026-07-13
**Question:** Should hades replace cpp-httplib (and possibly cpr/libcurl) with the
[cesanta/mongoose](https://github.com/cesanta/mongoose) embedded C networking library, to get
one HTTP dependency + native SSE/WebSocket streaming?
**Method:** Web-verified (license text, latest version, CVEs, API model). URLs cited inline.

---

## TL;DR — verdict: **REJECT** (do not adopt mongoose)

Three independent reasons, any one of which is sufficient, and the first is decisive:

1. **License kills it.** Mongoose is **GPLv2 / commercial dual-licensed**. The hades author wants
   to publish under MIT. Linking GPLv2 mongoose makes every **distributed binary** GPLv2 (copyleft),
   regardless of what the hades `.cpp` files say in their headers. "MIT source that builds into a
   GPLv2 binary" is legally coherent but is a **trap for downstream users** who read "MIT" and inherit
   copyleft obligations they didn't expect. The commercial escape hatch is **bespoke enterprise
   pricing** — unrealistic for a hobby/self-host project. Notably, **civetweb (MIT) exists precisely
   because mongoose left MIT** — the permissive-C world already voted with its feet.
2. **It doesn't deliver "one dependency."** Mongoose's HTTP *client* is a bare TCP-connect primitive;
   it will not sanely replace cpr/libcurl for 600 s LLM POSTs, proxies, redirects, multipart STT
   uploads, and zero-config system-CA TLS. **cpr/curl stays regardless**, so mongoose only replaces
   httplib on the *server* side — the "collapse to one dep" argument evaporates.
3. **It forces a rearchitecture and just ate three preauth-RCE CVEs.** Mongoose is a
   single-threaded event loop where a **blocking handler stalls every connection** — but hades's
   current handlers *block for minutes* inside `run_until`. Adopting mongoose means rewriting both
   listeners around `mg_wakeup()`. And its **built-in TLS stack** had three preauth-RCE-class CVEs
   disclosed **April 2026** (CVE-2026-5244/5245/5246).

**The actual motivating feature — SSE token streaming to the web UI — does not need mongoose.**
cpp-httplib already ships `set_chunked_content_provider` (SSE today, no new dependency, stays MIT).
The hard part of streaming is getting partial tokens *out of the Arbiter*, which is identical work
under any HTTP library. See **Recommendation**.

---

## 1. License analysis (the decision-maker)

### The facts (verified)
- Mongoose is **dual-licensed: GNU GPL version 2 OR a commercial license**. Verified against the
  [LICENSE file at repo HEAD](https://github.com/cesanta/mongoose/blob/master/LICENSE) and the
  [licensing page](https://mongoose.ws/licensing/). Copyright: *2004–2013 Sergey Lyubka* and
  *2013–2026 Cesanta Software Limited*.
- **GPLv2, not "or later."** The LICENSE header and `mongoose.h` cite "the GNU General Public License
  version 2" with **no "(or any later version)" clause** → read as **GPLv2-only**. (For hades this
  distinction barely matters — see below — but it's the honest reading.)
- **History:** mongoose was **MIT-licensed until ~v6** (2013/2015), when Cesanta relicensed to
  GPLv2/commercial (see [issue #222 "Licence Change from MIT to GPL2/Commercial"](https://github.com/cesanta/mongoose/issues/222)).
  **civetweb** was forked from the last MIT-era mongoose and remains MIT to this day. This is the single
  most important comparable-project data point (see (d)).
- **Commercial pricing model:** "**Our pricing is bespoke to your project**" — no public price. Structure
  per the [licensing page](https://mongoose.ws/licensing/): a **one-time license fee bound to a specific
  product/family AND library version**, plus an **optional yearly maintenance subscription** (mandatory
  only for company-wide licenses). I.e. contact-sales enterprise licensing; re-purchase on major upgrades.
  **Not realistic** for a hobby/self-host open-source project.

### Answering the specific questions

**(a) Can the hades repo itself still be MIT source?** Individual hades-authored files can carry MIT
headers — that's legal, because MIT is one-way GPL-compatible (the FSF lists MIT/Expat as GPL-compatible;
MIT code may be *absorbed into* a GPL work). **But** GPLv2 §2(b) says any work you *distribute* that "in
whole or in part contains or is derived from the Program … must be licensed as a whole at no charge to all
third parties under the terms of this License." A hades build that links mongoose **is** such a combined
work. So: MIT file headers survive as isolated files, but you **cannot honestly call the project MIT** —
the work-as-distributed is GPLv2.

**(b) What license governs distributed binaries?** **GPLv2.** Any hades binary that links mongoose must be
distributed under GPLv2: full corresponding source offered, no additional restrictions, recipients get the
four freedoms. The static musl Pi build (`scp` a binary to the device) is a *distribution* — it triggers
this in full.

**(c) Is "MIT source + GPLv2 binary" coherent or a trap?** Legally coherent, **practically a trap.** A
downstream user who sees the MIT badge, forks hades, and ships a product with the default (mongoose-linked)
build is silently bound by GPLv2 copyleft — they must offer source and cannot relicense proprietary. An
honest posture would demand a **prominent, load-bearing notice**: *"hades sources are MIT, but the default
build links GPLv2 mongoose, so any binary you distribute is GPLv2."* That caveat defeats the entire reason
to pick MIT (frictionless permissive reuse). A permissive project should not carry a copyleft dependency in
its default build.

**(d) What do comparable MIT projects do?** They **avoid GPL mongoose**, and the ecosystem provides
purpose-built MIT alternatives:
- **civetweb** (MIT) — literally the maintained fork of MIT-era mongoose, created for this exact reason.
- **cpp-httplib** (MIT) — what hades already uses.
- **libwebsockets** (MIT), **uWebSockets** (Apache-2.0) — MIT/permissive WS+HTTP servers.
- **libcurl** (MIT/X-derivative) — the permissive client.
When a permissive project needs an embeddable C web server, it reaches for civetweb or httplib, **not**
mongoose. Embedding GPL mongoose in an "MIT" project is the anti-pattern the fork was made to avoid.

### License bottom line
**GPLv2 mongoose is incompatible with an MIT publishing goal.** This alone ends the evaluation; the rest of
this document assumes the license were somehow a non-issue, and shows mongoose *still* loses on the merits.

---

## 2. Server-side fit — would it replace the two listeners?

Today: `HttpServerModule` (web UI + JSON API + `authorize()` CSRF seam) and `BridgeModule`
(`/ask`, `/share`, `/card`, secret-header auth) each run cpp-httplib, whose **blocking thread-per-connection**
model maps 1:1 onto the current code — a handler calls `run_until(...)` and **blocks for up to minutes**
while the turn completes, then returns the response. The TurnGate serializes turns, so blocking is fine.

Mongoose is the **opposite model**: a **single-threaded event loop** (`mg_mgr_poll(&mgr, timeout_ms)`
iterates *all* connections on one thread), and its core is **not thread-safe** — "all `mg_*` API functions
[must be] called from the same thread"
([multithreading tutorial](https://mongoose.ws/documentation/tutorials/core/multi-threaded/),
[docs](https://mongoose.ws/documentation/)). Consequence, stated plainly in Cesanta's own docs:

> A blocking handler **stalls all other connections.** Long-running work in an event handler blocks the
> entire event loop.

hades's handlers are exactly that — they block. So mongoose **forces a rearchitecture** of both listeners:

- On request, **hand off** the work to a worker thread (hades already has `Executor` + `TurnGate` +
  `run_until`), **return from the handler immediately**, and later **`mg_wakeup()`** the event loop from the
  worker; deliver the HTTP response in an `MG_EV_WAKEUP` handler (`mg_wakeup_init()` at startup).
- The **confirm round-trip** is worse: today the handler blocks on `pending_confirm_`; under mongoose the
  confirm-gated flow becomes an explicit async state machine across the poll loop.
- The `authorize()` pre-routing CSRF seam, routing, `/history`, `/card`, and the secret-header auth all get
  reimplemented against mongoose's `MG_EV_HTTP_MSG` + `mg_http_match_uri` style.

This is a **real, non-trivial rewrite of two currently-stable modules**, converting a
blocking-thread-per-request design into an event-driven one — for no functional gain over what httplib
already does. hades *has* the offload machinery, but the handlers currently *use it synchronously*; mongoose
would require them to become genuinely async. **Verdict: net negative on server fit.**

---

## 3. SSE / WebSocket server — the actual "win," measured honestly

**Mongoose side.** WS: `mg_ws_upgrade()` turns an HTTP connection into a WebSocket, messages arrive as
`MG_EV_WS_MSG` ([WS server tutorial](https://mongoose.ws/documentation/tutorials/websocket/websocket-server/)).
SSE: there is no special API — you simply keep the connection open and write chunks (`mg_http_write_chunk` /
`mg_printf`) from the event loop. In an event-driven server this is idiomatic. **But** the tokens are produced
by the LLM on a *worker* thread, and you **cannot touch the connection from the worker** (non-thread-safe
core) — so you *still* marshal every token back through `mg_wakeup()` to the poll thread. The event model does
not remove the cross-thread bridge; it just relocates it.

**httplib side (what hades has).** `set_chunked_content_provider("text/event-stream", provider)` already gives
SSE — verified in [cpp-httplib README-stream.md](https://github.com/yhirose/cpp-httplib/blob/master/README-stream.md)
and community SSE examples. The provider lambda runs on httplib's per-connection thread and **pulls** data via
`DataSink::write` / `sink.done()`; you bridge the Arbiter's partial emits into a queue the provider drains.
Known wrinkle: chunked-provider socket-disconnect detection is imperfect
([issue #1245](https://github.com/yhirose/cpp-httplib/issues/1245)) — manageable, not a blocker.

**Honest comparison.** Both approaches require the *same* essential work: a thread-safe queue carrying partial
tokens from the Arbiter/worker to whatever holds the socket. Mongoose's event model shines when you must fan
one data source out to *many* concurrent streaming clients — but **hades runs one serialized turn at a time
behind the TurnGate**, so that advantage is moot. The streaming problem is 90% "make the Arbiter emit partial
tokens," 10% HTTP plumbing, and the 10% is already solved in httplib with zero new dependency. **The SSE win
is real but small, and it is not free on either library.**

For a *server-side* WebSocket (if ever wanted): hades already has an **in-house RFC6455 codec** (the simplex
client). A minimal WS *server* is the same frame encode/decode plus an HTTP `Upgrade` handshake — reusable
without any new dependency.

---

## 4. Client-side fit — mongoose will not replace cpr/libcurl

cpr/libcurl currently carries **all** client calls: LLM completions (long-lived POSTs up to 600 s), Telegram
long-poll, bridge outbound, embeddings/STT/TTS, and MCP Streamable HTTP (JSON-RPC POST with manual SSE-frame
parsing). What it gives for free: **timeouts, proxies, redirects, multipart POST (STT uploads), robust TLS
with zero-config system-CA discovery** (already verified working static-musl on the Pi, per CLAUDE.md), retries.

Mongoose's client is a **bare primitive**: `mg_http_connect()` "only creates a TCP connection … does not send
a request" ([HTTP client tutorial](https://mongoose.ws/documentation/tutorials/http/http-client/)); you write
the request yourself with `mg_printf`, drive `MG_EV_CONNECT`/`MG_EV_HTTP_MSG` by hand, implement your own
connect/response **timeout** by watching `MG_EV_POLL` and calling `mg_error()`, hand-roll the proxy `CONNECT`
handshake ([proxy tutorial](https://mongoose.ws/documentation/tutorials/http/http-proxy-client/)), and build
**client-side multipart bodies by hand** (mongoose's multipart helpers, `mg_http_next_multipart` /
`mg_parse_multipart`, are *parse*-only, i.e. server-inbound). Replacing every cpr call with this — inside the
same single-threaded, non-thread-safe event loop — is a large, high-risk rewrite for negative value.

**Conclusion:** cpr/curl **stays no matter what.** Therefore mongoose could at most replace httplib (server),
never curl (client). The build would then carry **mongoose (GPLv2) + curl + a TLS backend** — *more* license
and dependency surface than today, not less. The "one dependency" premise is false.

---

## 5. TLS under musl static

Mongoose TLS options ([TLS tutorial](https://mongoose.ws/documentation/tutorials/tls/)): a **built-in TLS 1.3
stack** (`MG_TLS_BUILTIN`), or **mbedTLS**, or **OpenSSL**.

**The built-in stack is not production-ready.** In **April 2026**, three CVEs were disclosed, **all in
`MG_TLS_BUILTIN`**, affecting mongoose **7.0–7.20**, fixed in **7.21** (2026-04-01)
([evilsocket writeup](https://www.evilsocket.net/2026/04/02/Mongoose-Preauth-Remote-Code-Execution-and-mTLS-Bypass/),
[Nozomi — 10 vulnerabilities](https://www.nozominetworks.com/blog/hunting-the-mongoose-discovering-10-vulnerabilities-in-the-mongoose-web-server-library)):
- **CVE-2026-5244** — preauth **RCE as root**: attacker-controlled RSA public key copied into a fixed 528-byte
  heap buffer, no bounds check (TLS handshake cert parsing).
- **CVE-2026-5245** — mDNS handler stack overflow → preauth RCE.
- **CVE-2026-5246** — **mTLS bypass**: signature verification returns success without checking the signature
  when the CA uses a P-384 key.

The researcher's own conclusion: *"maybe don't roll your own TLS."* For an agent that fetches arbitrary URLs
and exposes network listeners, shipping a young hand-written TLS stack is a liability.

So realistically you'd pair mongoose with **mbedTLS or OpenSSL**. If OpenSSL, you're back to the **exact heavy
static dependency hades has today** — and the client side keeps curl+OpenSSL anyway. **Net binary-size effect
vs. today's httplib+cpr+curl+OpenSSL is roughly neutral-to-worse** once a real TLS backend is added and curl is
retained. mbedTLS would be lighter than OpenSSL but is a *new* dependency to vet, package static-musl, and
maintain — again for the server side only.

---

## 6. Nix packaging

**Mongoose is not in nixpkgs** (0 matches for the C library; "mongoose" in that ecosystem is the Node.js
MongoDB ODM). That's expected: mongoose ships as a **two-file amalgamation** (`mongoose.c` + `mongoose.h`)
designed to be **vendored directly into your source tree** and compiled with your app. So "packaging" is
trivial — drop two files in, add a compile unit and feature `#define`s; it builds static-musl fine (it's plain
C). **But** vendoring makes the license problem *literal and glaring*: **GPLv2 `mongoose.c`/`mongoose.h` files
sitting inside an "MIT" repository**, compiled into every binary. There is no packaging obstacle — the obstacle
is that doing it advertises the copyleft contamination in the file tree itself.

---

## 7. Alternatives (brief)

If a real SSE/WS *server* need materializes, and staying on httplib is not enough:

- **Stay on cpp-httplib (MIT) + reuse the in-house WS codec** — *recommended*. SSE via the existing
  `set_chunked_content_provider`; a WS server is the in-house RFC6455 encode/decode + an `Upgrade` handshake.
  **Zero new dependency, MIT throughout, no rearchitecture.**
- **civetweb (MIT)** — the maintained MIT fork of old mongoose; embeddable C, HTTP + **WebSocket server**,
  thread-per-connection (blocking handlers OK → maps to hades's current model, unlike mongoose). The "mongoose
  but MIT and no rearchitecture" option if httplib ever proves insufficient.
- **libwebsockets (MIT)** — mature, high-performance WS/HTTP server+client; but a heavier, more complex event
  API and a bigger integration cost. Consider only if WS becomes central.
- **uWebSockets (Apache-2.0)** — very fast; C++/template-heavy and opinionated about its event loop. Overkill
  for one serialized turn.

---

## 8. Recommendation — concrete path for hades

**Reject mongoose.** Do not add it to any build. Rationale, in priority order:

1. **License:** GPLv2 copyleft is fundamentally at odds with the MIT publishing goal; the commercial license is
   bespoke enterprise pricing; the ecosystem already forked civetweb to escape exactly this. Decisive on its own.
2. **No consolidation:** cpr/curl must stay for the client, so mongoose replaces only httplib — it *grows*
   dependency + license surface, not shrinks it.
3. **Rearchitecture cost:** the single-threaded, non-thread-safe event loop is incompatible with hades's
   blocking `run_until` handlers without a genuine async rewrite of both listeners via `mg_wakeup()`.
4. **Security:** the built-in TLS stack took three preauth-RCE-class CVEs in April 2026; a safe deployment
   forces OpenSSL/mbedTLS anyway, erasing any footprint win.

**Instead, when SSE token streaming is picked up as a feature:**
- Keep **cpp-httplib (MIT)** and **cpr/libcurl (MIT)** as-is.
- Implement SSE with httplib's existing **`set_chunked_content_provider("text/event-stream", …)`** — no new
  dependency, stays MIT.
- Spend the effort where it actually is: make the **Arbiter emit partial tokens** (needs provider streaming +
  Arbiter partial emits) and bridge them to the SSE provider via a thread-safe queue. This is the real 90% of
  the work and is HTTP-library-independent.
- If a *server-side WebSocket* is ever needed, extend the **in-house RFC6455 codec** (already in the simplex
  client) with a small `Upgrade` handshake, or adopt **civetweb (MIT)** — both preserve the MIT license and the
  blocking-handler model.

Mongoose is a fine library for its target (proprietary embedded firmware with a commercial license, or fully-GPL
IoT products). It is the **wrong fit for an MIT-published, self-hosted C++ agent** on every axis that matters here.

---

## Sources

- [mongoose LICENSE (repo HEAD)](https://github.com/cesanta/mongoose/blob/master/LICENSE)
- [mongoose.ws — Licensing (types, pricing model, GPLv2 obligations)](https://mongoose.ws/licensing/)
- [Issue #222 — Licence Change from MIT to GPL2/Commercial](https://github.com/cesanta/mongoose/issues/222)
- [Multithreading tutorial (event loop, blocking handler, mg_wakeup)](https://mongoose.ws/documentation/tutorials/core/multi-threaded/)
- [Documentation index](https://mongoose.ws/documentation/)
- [WebSocket server tutorial](https://mongoose.ws/documentation/tutorials/websocket/websocket-server/)
- [HTTP client tutorial](https://mongoose.ws/documentation/tutorials/http/http-client/)
- [HTTP proxy client tutorial](https://mongoose.ws/documentation/tutorials/http/http-proxy-client/)
- [TLS tutorial](https://mongoose.ws/documentation/tutorials/tls/)
- [evilsocket — Mongoose preauth RCE + mTLS bypass (CVE-2026-5244/5245/5246, fixed 7.21)](https://www.evilsocket.net/2026/04/02/Mongoose-Preauth-Remote-Code-Execution-and-mTLS-Bypass/)
- [Nozomi Networks — 10 vulnerabilities in Mongoose](https://www.nozominetworks.com/blog/hunting-the-mongoose-discovering-10-vulnerabilities-in-the-mongoose-web-server-library)
- [cpp-httplib README-stream.md (chunked content provider / SSE)](https://github.com/yhirose/cpp-httplib/blob/master/README-stream.md)
- [cpp-httplib #1245 — chunked provider & socket disconnects](https://github.com/yhirose/cpp-httplib/issues/1245)
- nixpkgs: no `mongoose` C-library package (GitHub code search, 0 matches)
