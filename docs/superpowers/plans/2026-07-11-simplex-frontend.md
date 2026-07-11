# SimpleX Front-end Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A SimpleX Chat front-end (`Module = simplex`) — allowlisted contacts drive normal gated turns over a local `simplex-chat -p 5225` daemon, via an in-house minimal WebSocket client.

**Architecture:** Pure RFC6455-subset codec (`ws.h/ws.cpp`) → blocking `WsClient` over a POSIX socket → `WsSimplexApi` (corrId command round-trips + tolerant event parse) behind the `SimplexApi` seam → `SimplexModule` on the TelegramModule pattern (own thread, shared TurnGate, allowlist, text-y/N confirms, NOTIFY_USER sink) → wiring/ship.

**Tech Stack:** C++20, POSIX sockets (no new deps), nlohmann_json, CMake+Ninja in `nix develop`, GoogleTest.

**Spec:** `docs/superpowers/specs/2026-07-11-simplex-frontend-design.md` (approved, committed `b704af7`).

## Global Constraints

- Every build/test command runs inside `nix develop`: `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure`. Baseline **569/569 green**; the full suite gates every task. TSan gate at the end (new thread).
- Branch `feat/simplex`. Commit style `<type>: <desc>` — NO attribution footer, NO Co-Authored-By.
- **Zero new dependencies** (spec decision A): WebSocket client is in-house, localhost-only scope — no TLS, no compression, no proxy. Client frames masked; server fragmentation tolerated; RSV bits rejected.
- Module name exactly `simplex`; manifest block `Simplex`; config keys exactly: `host` (default `127.0.0.1`), `port` (default `5225`), `allow_contacts` (**REQUIRED**, COMMA-separated ids and/or exact display names → `MalConfig` when missing/empty), `auto_accept` (default `false`), `notify_contact` (optional), `connect_timeout_s` (default `10`).
- v1 = text DMs only: group items, non-text content, own-echo (`directSnd`) items are all skipped. Reply split at **4000** chars (reuse `split_message` from `hades/telegram/parse.h`).
- Confirm = text y/N: next message from the SAME contact while a confirm is outstanding is the answer; `y`/`yes` (case-insensitive, trimmed) approves, anything else denies.
- Daemon protocol strings (pinned from upstream `bots/api/COMMANDS.md` + the TS types package): send = `/_send @<contactId> json [{"msgContent":{"type":"text","text":<chunk>}}]` (ok resp type `newChatItems`); accept = `/_accept <contactRequestId>` (ok resp type `acceptingContactRequest`); error resp type `chatCmdError`. Events (frames without a matching corrId): `newChatItems` (path `resp.chatItems[].{chatInfo{type:"direct",contact{contactId,localDisplayName}}, chatItem{chatDir{type:"directRcv"}, content{type:"rcvMsgContent", msgContent{type:"text",text}}}}`), `receivedContactRequest` (`resp.contactRequest.{contactRequestId,localDisplayName}`), `contactConnected` (`resp.contact.{contactId,localDisplayName}`). Parser must also unwrap a `{"Right": …}` / `{"Left": …}` layer inside `resp` (Haskell Either encoding seen in some CLI versions) and skip anything malformed.
- **NEVER stage or modify `manifests/dev.hades` working values, `manifests/pi.hades`, `memory/facts.md`.** Task 5 uses the backup → checkout-clean → edit → commit → restore manifest procedure.
- `Agent::simplex` is declared BETWEEN `telegram` and `heartbeat` (teardown: heartbeat joins first — a tick may notify via simplex; simplex joins while executor+modules are alive). Thread started by `hades_main` (`start()`), never in `on_attach`.

---

## File Structure

```
include/hades/simplex/ws.h            T1 codec decls + T2 WsClient decl
src/apps/simplex/ws.cpp               T1 sha1/base64/handshake/frames + T2 WsClient impl
tests/test_ws_frame.cpp               T1
tests/test_ws_client.cpp              T2 (fake localhost WS server)
include/hades/simplex/api.h           T3 SxEvent/SxStatus/SimplexApi seam
src/apps/simplex/simplex.cpp          T3 parse + WsSimplexApi; T4 SimplexModule
tests/test_simplex_parse.cpp          T3 (canned JSON)
tests/test_simplex_api.cpp            T3 (corrId round-trip vs fake server)
include/hades/module/simplex_module.h T4
tests/test_simplex_module.cpp         T4 (fake SimplexApi rig)
app/agent_wiring.{h,cpp}, app/hades_main.cpp  T5
tests/test_simplex_wiring.cpp         T5
manifests/dev.hades, docs/manifest-reference.md, CLAUDE.md  T5
CMakeLists.txt                        T1-T5 (add rows per task)
```

---

## Task 1: WebSocket codec (pure — sha1, base64, handshake, frames, decoder)

**Files:**
- Create: `include/hades/simplex/ws.h`, `src/apps/simplex/ws.cpp`
- Test: `tests/test_ws_frame.cpp`
- Modify: `CMakeLists.txt`

**Interfaces — Produces (all `namespace hades`):**
- `std::string ws_sha1(const std::string&)` — raw 20-byte digest
- `std::string ws_base64(const std::string&)`
- `std::string ws_accept_value(const std::string& key_b64)` — `b64(sha1(key + RFC6455-GUID))`
- `std::string ws_handshake_request(const std::string& host, int port, const std::string& key_b64)`
- `bool ws_handshake_ok(const std::string& response_headers, const std::string& key_b64)`
- `enum class WsOp : std::uint8_t { Cont=0x0, Text=0x1, Binary=0x2, Close=0x8, Ping=0x9, Pong=0xA }`
- `std::string ws_encode_frame(WsOp, const std::string& payload, bool mask, std::uint32_t mask_key)`
- `struct WsMessage { WsOp op; std::string payload; }`
- `class WsDecoder { explicit WsDecoder(std::size_t max_message_bytes = 16*1024*1024); void feed(const char*, std::size_t); bool pop(WsMessage&); bool error() const; }`

- [ ] **Step 1: Write the failing tests** `tests/test_ws_frame.cpp`:

```cpp
// tests/test_ws_frame.cpp — pure RFC6455 codec: sha1/base64/handshake/frames/decoder
#include <gtest/gtest.h>
#include <cstdint>
#include <string>
#include "hades/simplex/ws.h"
using namespace hades;

static std::string hex(const std::string& s) {
  static const char* d = "0123456789abcdef";
  std::string o;
  for (unsigned char c : s) { o += d[c >> 4]; o += d[c & 0xF]; }
  return o;
}

TEST(WsCodec, Sha1KnownVectors) {
  EXPECT_EQ(hex(ws_sha1("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
  EXPECT_EQ(hex(ws_sha1("")), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  // Multi-block input (>64 bytes) exercises the block loop.
  EXPECT_EQ(hex(ws_sha1(std::string(1000, 'a'))), "291e9a6c66994949b57ba5e650361e98fc36b1ba");
}

TEST(WsCodec, Base64KnownVectors) {
  EXPECT_EQ(ws_base64(""), "");
  EXPECT_EQ(ws_base64("f"), "Zg==");
  EXPECT_EQ(ws_base64("fo"), "Zm8=");
  EXPECT_EQ(ws_base64("foo"), "Zm9v");
  EXPECT_EQ(ws_base64("foobar"), "Zm9vYmFy");
}

TEST(WsCodec, AcceptValueRfc6455Example) {
  // The worked example from RFC 6455 §1.3.
  EXPECT_EQ(ws_accept_value("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(WsCodec, HandshakeRequestShape) {
  const std::string req = ws_handshake_request("127.0.0.1", 5225, "S2V5S2V5S2V5S2V5S2V5S2U=");
  EXPECT_EQ(req.rfind("GET / HTTP/1.1\r\n", 0), 0u);
  EXPECT_NE(req.find("Host: 127.0.0.1:5225\r\n"), std::string::npos);
  EXPECT_NE(req.find("Upgrade: websocket\r\n"), std::string::npos);
  EXPECT_NE(req.find("Connection: Upgrade\r\n"), std::string::npos);
  EXPECT_NE(req.find("Sec-WebSocket-Key: S2V5S2V5S2V5S2V5S2V5S2U=\r\n"), std::string::npos);
  EXPECT_NE(req.find("Sec-WebSocket-Version: 13\r\n"), std::string::npos);
  EXPECT_EQ(req.substr(req.size() - 4), "\r\n\r\n");
}

TEST(WsCodec, HandshakeOkAcceptsValidRejectsBad) {
  const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
  const std::string good =
      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
  EXPECT_TRUE(ws_handshake_ok(good, key));
  EXPECT_FALSE(ws_handshake_ok(good, "b3RoZXIga2V5IGhlcmUuLi4="));   // wrong key
  const std::string not101 =
      "HTTP/1.1 400 Bad Request\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
  EXPECT_FALSE(ws_handshake_ok(not101, key));
  EXPECT_FALSE(ws_handshake_ok("garbage", key));
}

TEST(WsCodec, EncodeUnmaskedSmallFrame) {
  // Server-style (unmasked) "Hello": 0x81 0x05 then payload (RFC example).
  const std::string f = ws_encode_frame(WsOp::Text, "Hello", false, 0);
  ASSERT_EQ(f.size(), 7u);
  EXPECT_EQ(static_cast<unsigned char>(f[0]), 0x81u);
  EXPECT_EQ(static_cast<unsigned char>(f[1]), 0x05u);
  EXPECT_EQ(f.substr(2), "Hello");
}

TEST(WsCodec, EncodeMaskedRoundTripsThroughDecoder) {
  const std::string payload(300, 'x');   // forces the 16-bit length form
  const std::string f = ws_encode_frame(WsOp::Text, payload, true, 0xA1B2C3D4u);
  EXPECT_EQ(static_cast<unsigned char>(f[1]) & 0x80u, 0x80u);         // mask bit set
  EXPECT_EQ(static_cast<unsigned char>(f[1]) & 0x7Fu, 126u);          // extended-16 length
  WsDecoder d;
  d.feed(f.data(), f.size());
  WsMessage m;
  ASSERT_TRUE(d.pop(m));
  EXPECT_EQ(m.op, WsOp::Text);
  EXPECT_EQ(m.payload, payload);   // decoder unmasks
  EXPECT_FALSE(d.pop(m));
}

TEST(WsCodec, DecoderHandlesSplitDelivery) {
  // One frame fed byte-by-byte must yield exactly one message.
  const std::string f = ws_encode_frame(WsOp::Text, "chunked delivery", false, 0);
  WsDecoder d;
  WsMessage m;
  for (char c : f) {
    d.feed(&c, 1);
  }
  ASSERT_TRUE(d.pop(m));
  EXPECT_EQ(m.payload, "chunked delivery");
}

TEST(WsCodec, DecoderReassemblesFragmentsWithInterleavedPing) {
  // text-fragment(FIN=0) + ping + continuation(FIN=1): ping pops first (control frames may
  // interleave), then the reassembled text.
  std::string frag1 = ws_encode_frame(WsOp::Text, "hel", false, 0);
  frag1[0] = static_cast<char>(frag1[0] & 0x7F);                      // clear FIN
  const std::string ping = ws_encode_frame(WsOp::Ping, "p", false, 0);
  const std::string frag2 = ws_encode_frame(WsOp::Cont, "lo", false, 0);
  WsDecoder d;
  const std::string all = frag1 + ping + frag2;
  d.feed(all.data(), all.size());
  WsMessage m;
  ASSERT_TRUE(d.pop(m));
  EXPECT_EQ(m.op, WsOp::Ping);
  ASSERT_TRUE(d.pop(m));
  EXPECT_EQ(m.op, WsOp::Text);
  EXPECT_EQ(m.payload, "hello");
}

TEST(WsCodec, DecoderErrorsOnOversizeRsvAndBadFragmentation) {
  {   // oversize: 2-byte cap, 3-byte payload
    WsDecoder d(2);
    const std::string f = ws_encode_frame(WsOp::Text, "abc", false, 0);
    d.feed(f.data(), f.size());
    EXPECT_TRUE(d.error());
  }
  {   // RSV bits set (no extension negotiated) -> error
    std::string f = ws_encode_frame(WsOp::Text, "x", false, 0);
    f[0] = static_cast<char>(f[0] | 0x40);
    WsDecoder d;
    d.feed(f.data(), f.size());
    EXPECT_TRUE(d.error());
  }
  {   // continuation with nothing to continue -> error
    const std::string f = ws_encode_frame(WsOp::Cont, "x", false, 0);
    WsDecoder d;
    d.feed(f.data(), f.size());
    EXPECT_TRUE(d.error());
  }
  {   // fragmented control frame (ping with FIN=0) -> error
    std::string f = ws_encode_frame(WsOp::Ping, "p", false, 0);
    f[0] = static_cast<char>(f[0] & 0x7F);
    WsDecoder d;
    d.feed(f.data(), f.size());
    EXPECT_TRUE(d.error());
  }
}

TEST(WsCodec, Encode64BitLengthHeader) {
  const std::string payload(70000, 'y');   // > 0xFFFF -> 64-bit length form
  const std::string f = ws_encode_frame(WsOp::Text, payload, false, 0);
  EXPECT_EQ(static_cast<unsigned char>(f[1]) & 0x7Fu, 127u);
  WsDecoder d;
  d.feed(f.data(), f.size());
  WsMessage m;
  ASSERT_TRUE(d.pop(m));
  EXPECT_EQ(m.payload.size(), 70000u);
}
```

- [ ] **Step 2: CMake + run — expect FAIL.** In `CMakeLists.txt`, next to the other `src/apps/*` rows (after the `src/apps/telegram/telegram.cpp` row, currently line ~177):

```cmake
target_sources(hades_core PRIVATE src/apps/simplex/ws.cpp)
target_sources(hades_tests PRIVATE tests/test_ws_frame.cpp)
```

Run `nix develop --command cmake --build build` → compile error (missing header).

- [ ] **Step 3: Implement.** `include/hades/simplex/ws.h`:

```cpp
// include/hades/simplex/ws.h — minimal RFC6455 WebSocket client subset (localhost bot API)
//
// Everything hades needs to talk to the local `simplex-chat -p <port>` daemon and NOTHING
// more: client handshake, masked client frames, tolerant server-frame decoding (fragmentation
// + interleaved control frames), ping/pong/close. No TLS, no compression (RSV bits rejected),
// no proxy — the peer is the local daemon (spec decision: in-house, zero new dependencies).
// Split: pure codec (this header's free functions + WsDecoder, byte-level testable) and the
// blocking-socket WsClient (tested against a fake localhost server).
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <random>
#include <string>
namespace hades {

// -- pure codec ------------------------------------------------------------
std::string ws_sha1(const std::string& bytes);                 // raw 20-byte digest
std::string ws_base64(const std::string& bytes);
std::string ws_accept_value(const std::string& key_b64);       // b64(sha1(key + RFC6455 GUID))
std::string ws_handshake_request(const std::string& host, int port, const std::string& key_b64);
// True iff the response is a 101 whose Sec-WebSocket-Accept matches key_b64.
bool ws_handshake_ok(const std::string& response_headers, const std::string& key_b64);

enum class WsOp : std::uint8_t { Cont = 0x0, Text = 0x1, Binary = 0x2, Close = 0x8, Ping = 0x9, Pong = 0xA };

// One unfragmented frame (we never fragment sends). Client->server frames MUST set mask=true.
std::string ws_encode_frame(WsOp op, const std::string& payload, bool mask, std::uint32_t mask_key);

struct WsMessage { WsOp op = WsOp::Text; std::string payload; };

// Incremental decoder: feed raw bytes, pop complete MESSAGES. Fragmented data frames are
// reassembled; control frames (<=125 bytes, FIN required) pop as-is and may interleave.
// Violations (RSV set, oversize, bad fragmentation) poison the decoder — the connection must
// be dropped (error() stays true; feed/pop become no-ops).
class WsDecoder {
 public:
  explicit WsDecoder(std::size_t max_message_bytes = 16 * 1024 * 1024) : max_(max_message_bytes) {}
  void feed(const char* data, std::size_t n);
  bool pop(WsMessage& out);
  bool error() const { return error_; }
 private:
  void parse_();
  std::string buf_;
  std::deque<WsMessage> out_;
  bool frag_active_ = false;
  WsOp frag_op_ = WsOp::Text;
  std::string frag_payload_;
  bool error_ = false;
  std::size_t max_;
};

// -- blocking client (Task 2) ------------------------------------------------
enum class WsRecv { Text, Timeout, Closed, Error };

// Blocking WebSocket client over a plain POSIX socket. Single-threaded by contract: the
// SimplexModule's one thread owns an instance entirely (no locking). recv_text auto-answers
// pings and swallows pongs/binary; a close frame or EOF closes the socket (-> Closed).
class WsClient {
 public:
  WsClient();
  ~WsClient();
  WsClient(const WsClient&) = delete;
  WsClient& operator=(const WsClient&) = delete;
  bool connect(const std::string& host, int port, double timeout_s);   // TCP + handshake
  bool send_text(const std::string& payload);
  WsRecv recv_text(double timeout_s, std::string& out);
  void close();
  bool connected() const { return fd_ >= 0; }
 private:
  bool send_all_(const std::string& bytes);
  bool send_frame_(WsOp op, const std::string& payload);
  int fd_ = -1;
  WsDecoder dec_;
  std::mt19937 rng_;   // mask keys (anti-cache per RFC, not security)
};

}  // namespace hades
```

`src/apps/simplex/ws.cpp` (Task 1 part — codec only; Task 2 appends the WsClient):

```cpp
// src/apps/simplex/ws.cpp — RFC6455 client subset: codec (sha1/base64/handshake/frames) + WsClient
#include "hades/simplex/ws.h"
#include <algorithm>
#include <cctype>
namespace hades {

std::string ws_sha1(const std::string& in) {
  std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  std::string msg = in;
  const std::uint64_t bitlen = static_cast<std::uint64_t>(msg.size()) * 8;
  msg += static_cast<char>(0x80);
  while (msg.size() % 64 != 56) msg += '\0';
  for (int i = 7; i >= 0; --i) msg += static_cast<char>((bitlen >> (i * 8)) & 0xFF);
  auto rol = [](std::uint32_t v, int s) { return (v << s) | (v >> (32 - s)); };
  for (std::size_t off = 0; off < msg.size(); off += 64) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i)
      w[i] = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(msg[off + 4 * i])) << 24) |
             (static_cast<std::uint32_t>(static_cast<std::uint8_t>(msg[off + 4 * i + 1])) << 16) |
             (static_cast<std::uint32_t>(static_cast<std::uint8_t>(msg[off + 4 * i + 2])) << 8) |
             static_cast<std::uint32_t>(static_cast<std::uint8_t>(msg[off + 4 * i + 3]));
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f, k;
      if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999u; }
      else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1u; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCu; }
      else             { f = b ^ c ^ d;                     k = 0xCA62C1D6u; }
      const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  std::string out(20, '\0');
  for (int i = 0; i < 5; ++i)
    for (int j = 0; j < 4; ++j)
      out[static_cast<std::size_t>(4 * i + j)] = static_cast<char>((h[i] >> (24 - 8 * j)) & 0xFF);
  return out;
}

std::string ws_base64(const std::string& bytes) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  std::size_t i = 0;
  while (i + 3 <= bytes.size()) {
    const std::uint32_t v = (static_cast<std::uint8_t>(bytes[i]) << 16) |
                            (static_cast<std::uint8_t>(bytes[i + 1]) << 8) |
                            static_cast<std::uint8_t>(bytes[i + 2]);
    out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63];
    out += tbl[(v >> 6) & 63];  out += tbl[v & 63];
    i += 3;
  }
  const std::size_t rem = bytes.size() - i;
  if (rem == 1) {
    const std::uint32_t v = static_cast<std::uint8_t>(bytes[i]) << 16;
    out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += "==";
  } else if (rem == 2) {
    const std::uint32_t v = (static_cast<std::uint8_t>(bytes[i]) << 16) |
                            (static_cast<std::uint8_t>(bytes[i + 1]) << 8);
    out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += '=';
  }
  return out;
}

std::string ws_accept_value(const std::string& key_b64) {
  return ws_base64(ws_sha1(key_b64 + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
}

std::string ws_handshake_request(const std::string& host, int port, const std::string& key_b64) {
  return "GET / HTTP/1.1\r\n"
         "Host: " + host + ":" + std::to_string(port) + "\r\n"
         "Upgrade: websocket\r\n"
         "Connection: Upgrade\r\n"
         "Sec-WebSocket-Key: " + key_b64 + "\r\n"
         "Sec-WebSocket-Version: 13\r\n\r\n";
}

bool ws_handshake_ok(const std::string& response_headers, const std::string& key_b64) {
  // Status line must be HTTP/1.x 101; the accept header must match exactly (case-insensitive
  // header NAME, exact value). Everything else is the daemon's business.
  if (response_headers.rfind("HTTP/1.1 101", 0) != 0 &&
      response_headers.rfind("HTTP/1.0 101", 0) != 0)
    return false;
  const std::string want = ws_accept_value(key_b64);
  std::string lower = response_headers;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const std::size_t at = lower.find("sec-websocket-accept:");
  if (at == std::string::npos) return false;
  std::size_t v = at + std::string("sec-websocket-accept:").size();
  const std::size_t end = response_headers.find("\r\n", v);
  std::string val = response_headers.substr(v, end == std::string::npos ? std::string::npos : end - v);
  // trim spaces
  const auto b = val.find_first_not_of(' ');
  const auto e = val.find_last_not_of(' ');
  if (b == std::string::npos) return false;
  return val.substr(b, e - b + 1) == want;
}

std::string ws_encode_frame(WsOp op, const std::string& payload, bool mask, std::uint32_t mask_key) {
  std::string f;
  f += static_cast<char>(0x80u | static_cast<std::uint8_t>(op));   // FIN always set
  const std::uint8_t mbit = mask ? 0x80u : 0u;
  const std::size_t len = payload.size();
  if (len < 126) {
    f += static_cast<char>(mbit | static_cast<std::uint8_t>(len));
  } else if (len <= 0xFFFF) {
    f += static_cast<char>(mbit | 126u);
    f += static_cast<char>((len >> 8) & 0xFF);
    f += static_cast<char>(len & 0xFF);
  } else {
    f += static_cast<char>(mbit | 127u);
    for (int i = 7; i >= 0; --i)
      f += static_cast<char>((static_cast<std::uint64_t>(len) >> (8 * i)) & 0xFF);
  }
  if (mask) {
    const unsigned char k[4] = {static_cast<unsigned char>(mask_key >> 24),
                                static_cast<unsigned char>(mask_key >> 16),
                                static_cast<unsigned char>(mask_key >> 8),
                                static_cast<unsigned char>(mask_key)};
    f.append(reinterpret_cast<const char*>(k), 4);
    for (std::size_t i = 0; i < len; ++i)
      f += static_cast<char>(static_cast<unsigned char>(payload[i]) ^ k[i % 4]);
  } else {
    f += payload;
  }
  return f;
}

void WsDecoder::feed(const char* data, std::size_t n) {
  if (error_) return;
  buf_.append(data, n);
  parse_();
}

bool WsDecoder::pop(WsMessage& out) {
  if (out_.empty()) return false;
  out = std::move(out_.front());
  out_.pop_front();
  return true;
}

void WsDecoder::parse_() {
  for (;;) {
    if (buf_.size() < 2) return;
    const std::uint8_t b0 = static_cast<std::uint8_t>(buf_[0]);
    const std::uint8_t b1 = static_cast<std::uint8_t>(buf_[1]);
    if (b0 & 0x70) { error_ = true; return; }                 // RSV bits: no extensions
    const bool fin = b0 & 0x80;
    const WsOp op = static_cast<WsOp>(b0 & 0x0F);
    const bool masked = b1 & 0x80;
    std::uint64_t len = b1 & 0x7F;
    std::size_t hdr = 2;
    if (len == 126) {
      if (buf_.size() < 4) return;
      len = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(buf_[2])) << 8) |
            static_cast<std::uint8_t>(buf_[3]);
      hdr = 4;
    } else if (len == 127) {
      if (buf_.size() < 10) return;
      len = 0;
      for (int i = 0; i < 8; ++i)
        len = (len << 8) | static_cast<std::uint8_t>(buf_[2 + i]);
      hdr = 10;
    }
    if (len > max_) { error_ = true; return; }
    const std::size_t mask_off = hdr;
    if (masked) hdr += 4;
    if (buf_.size() < hdr + len) return;                       // wait for more bytes
    std::string payload = buf_.substr(hdr, static_cast<std::size_t>(len));
    if (masked)
      for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^
                                       static_cast<unsigned char>(buf_[mask_off + (i % 4)]));
    buf_.erase(0, hdr + static_cast<std::size_t>(len));
    if (static_cast<std::uint8_t>(op) >= 0x8) {                // control frame
      if (!fin || len > 125) { error_ = true; return; }
      out_.push_back({op, std::move(payload)});
    } else if (op == WsOp::Cont) {
      if (!frag_active_) { error_ = true; return; }
      if (frag_payload_.size() + payload.size() > max_) { error_ = true; return; }
      frag_payload_ += payload;
      if (fin) {
        out_.push_back({frag_op_, std::move(frag_payload_)});
        frag_payload_.clear();
        frag_active_ = false;
      }
    } else {                                                   // Text/Binary
      if (frag_active_) { error_ = true; return; }
      if (fin) {
        out_.push_back({op, std::move(payload)});
      } else {
        frag_active_ = true;
        frag_op_ = op;
        frag_payload_ = std::move(payload);
      }
    }
  }
}

}  // namespace hades
```

(Task 2 appends the WsClient section to this same file.)

- [ ] **Step 4: Build + test.** `nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure -R WsCodec` → all pass; full suite green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/simplex/ws.h src/apps/simplex/ws.cpp tests/test_ws_frame.cpp CMakeLists.txt
git commit -m "feat: RFC6455 client codec — sha1/base64/handshake, frame encode, tolerant incremental decoder"
```

---

## Task 2: Blocking WsClient vs a fake localhost server

**Files:**
- Modify: `src/apps/simplex/ws.cpp` (append the WsClient section; the class is already declared in `ws.h` by Task 1)
- Test: `tests/test_ws_client.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: the Task 1 codec (`ws_handshake_request`, `ws_handshake_ok`, `ws_encode_frame`, `WsDecoder`).
- Produces: working `WsClient::{connect,send_text,recv_text,close,connected}` per the header contract. Task 3 relies on: `connect` returns false on refused/timeout/bad-handshake; `recv_text` auto-pongs pings, returns `WsRecv::{Text,Timeout,Closed,Error}`; `send_text` masks.

- [ ] **Step 1: Write the failing tests** `tests/test_ws_client.cpp`:

```cpp
// tests/test_ws_client.cpp — WsClient vs a scripted fake WS server on an ephemeral localhost port
#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include "hades/simplex/ws.h"
using namespace hades;

namespace {
// Minimal scripted WS server: bind 127.0.0.1:0, accept ONE client, perform the server side of
// the handshake (using the same pure codec), then run a per-test script on the raw fd.
struct FakeWsServer {
  int lfd = -1, cfd = -1, port = 0;
  std::thread th;
  WsDecoder dec;   // decodes CLIENT frames (masked; the decoder unmasks)

  FakeWsServer() {
    lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    EXPECT_EQ(::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
    socklen_t alen = sizeof a;
    ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &alen);
    port = ntohs(a.sin_port);
    ::listen(lfd, 1);
  }
  ~FakeWsServer() {
    if (th.joinable()) th.join();
    if (cfd >= 0) ::close(cfd);
    if (lfd >= 0) ::close(lfd);
  }
  void run(std::function<void(FakeWsServer&)> script) {
    th = std::thread([this, script] {
      cfd = ::accept(lfd, nullptr, nullptr);
      if (cfd < 0) return;
      handshake();
      script(*this);
    });
  }
  void handshake(bool valid = true) {
    std::string req;
    char c;
    while (req.find("\r\n\r\n") == std::string::npos && ::read(cfd, &c, 1) == 1) req += c;
    const std::string marker = "Sec-WebSocket-Key: ";
    const std::size_t at = req.find(marker);
    ASSERT_NE(at, std::string::npos);
    const std::size_t end = req.find("\r\n", at);
    const std::string key = req.substr(at + marker.size(), end - at - marker.size());
    const std::string accept = valid ? ws_accept_value(key) : "bm90IHRoZSByaWdodCBrZXk=";
    const std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                             "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n";
    (void)!::write(cfd, resp.data(), resp.size());
  }
  void send_raw(const std::string& bytes) { (void)!::write(cfd, bytes.data(), bytes.size()); }
  void send_text(const std::string& s) { send_raw(ws_encode_frame(WsOp::Text, s, false, 0)); }
  // Read client frames off the socket until one complete message pops.
  WsMessage recv_msg() {
    WsMessage m;
    char buf[512];
    while (!dec.pop(m)) {
      const ssize_t n = ::read(cfd, buf, sizeof buf);
      if (n <= 0) { m.op = WsOp::Close; return m; }
      dec.feed(buf, static_cast<std::size_t>(n));
    }
    return m;
  }
};
}  // namespace

TEST(WsClient, ConnectHandshakeEchoRoundTrip) {
  FakeWsServer srv;
  srv.run([](FakeWsServer& s) {
    WsMessage m = s.recv_msg();                    // client's text (masked; decoder unmasked it)
    EXPECT_EQ(m.op, WsOp::Text);
    s.send_text("echo:" + m.payload);
  });
  WsClient c;
  ASSERT_TRUE(c.connect("127.0.0.1", srv.port, 5.0));
  EXPECT_TRUE(c.connected());
  ASSERT_TRUE(c.send_text("hello daemon"));
  std::string out;
  ASSERT_EQ(c.recv_text(5.0, out), WsRecv::Text);
  EXPECT_EQ(out, "echo:hello daemon");
}

TEST(WsClient, ClientFramesAreMasked) {
  FakeWsServer srv;
  std::string raw;
  srv.run([&raw](FakeWsServer& s) {
    char buf[256];
    const ssize_t n = ::read(s.cfd, buf, sizeof buf);   // grab the client frame RAW
    if (n > 0) raw.assign(buf, static_cast<std::size_t>(n));
    s.send_text("done");
  });
  WsClient c;
  ASSERT_TRUE(c.connect("127.0.0.1", srv.port, 5.0));
  ASSERT_TRUE(c.send_text("mask me"));
  std::string out;
  ASSERT_EQ(c.recv_text(5.0, out), WsRecv::Text);       // synchronizes with the script
  ASSERT_GE(raw.size(), 2u);
  EXPECT_EQ(static_cast<unsigned char>(raw[1]) & 0x80u, 0x80u);   // mask bit
  EXPECT_EQ(raw.find("mask me"), std::string::npos);              // payload not in cleartext
}

TEST(WsClient, RecvAnswersPingWithPong) {
  FakeWsServer srv;
  WsMessage pong;
  srv.run([&pong](FakeWsServer& s) {
    s.send_raw(ws_encode_frame(WsOp::Ping, "beat", false, 0));
    pong = s.recv_msg();                            // the client's pong
    s.send_text("after-ping");
  });
  WsClient c;
  ASSERT_TRUE(c.connect("127.0.0.1", srv.port, 5.0));
  std::string out;
  ASSERT_EQ(c.recv_text(5.0, out), WsRecv::Text);   // ping was handled invisibly
  EXPECT_EQ(out, "after-ping");
  EXPECT_EQ(pong.op, WsOp::Pong);
  EXPECT_EQ(pong.payload, "beat");
}

TEST(WsClient, RecvTimesOutWhenSilent) {
  FakeWsServer srv;
  srv.run([](FakeWsServer& s) {
    WsMessage m = s.recv_msg();      // hold the connection until the client closes
    (void)m;
  });
  WsClient c;
  ASSERT_TRUE(c.connect("127.0.0.1", srv.port, 5.0));
  std::string out;
  EXPECT_EQ(c.recv_text(0.2, out), WsRecv::Timeout);
  c.close();                          // unblocks the server's recv_msg
}

TEST(WsClient, ServerCloseYieldsClosed) {
  FakeWsServer srv;
  srv.run([](FakeWsServer& s) {
    s.send_raw(ws_encode_frame(WsOp::Close, "", false, 0));
  });
  WsClient c;
  ASSERT_TRUE(c.connect("127.0.0.1", srv.port, 5.0));
  std::string out;
  EXPECT_EQ(c.recv_text(5.0, out), WsRecv::Closed);
  EXPECT_FALSE(c.connected());
}

TEST(WsClient, BadHandshakeRefused) {
  FakeWsServer srv;
  srv.th = std::thread([&srv] {
    srv.cfd = ::accept(srv.lfd, nullptr, nullptr);
    if (srv.cfd >= 0) srv.handshake(false);         // wrong accept value
  });
  WsClient c;
  EXPECT_FALSE(c.connect("127.0.0.1", srv.port, 5.0));
  EXPECT_FALSE(c.connected());
}

TEST(WsClient, ConnectToDeadPortFails) {
  // Bind + close a socket to find a (very likely) dead port.
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
  socklen_t alen = sizeof a;
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &alen);
  const int dead = ntohs(a.sin_port);
  ::close(fd);
  WsClient c;
  EXPECT_FALSE(c.connect("127.0.0.1", dead, 1.0));
}
```

- [ ] **Step 2: CMake + run — expect FAIL (WsClient methods undefined → link error).**

```cmake
target_sources(hades_tests PRIVATE tests/test_ws_client.cpp)
```

- [ ] **Step 3: Implement — append to `src/apps/simplex/ws.cpp`:**

```cpp
// ── WsClient: blocking POSIX-socket client (connect/handshake/send/recv/pong) ──────────────
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>

namespace hades {
namespace {
double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

WsClient::WsClient() : rng_(std::random_device{}()) {}
WsClient::~WsClient() { close(); }

void WsClient::close() {
  if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
  dec_ = WsDecoder{};                        // a new connection starts a fresh stream
}

bool WsClient::send_all_(const std::string& bytes) {
  std::size_t off = 0;
  while (off < bytes.size()) {
    const ssize_t n = ::write(fd_, bytes.data() + off, bytes.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      close();
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

bool WsClient::send_frame_(WsOp op, const std::string& payload) {
  if (fd_ < 0) return false;
  return send_all_(ws_encode_frame(op, payload, /*mask=*/true, rng_()));
}

bool WsClient::send_text(const std::string& payload) { return send_frame_(WsOp::Text, payload); }

bool WsClient::connect(const std::string& host, int port, double timeout_s) {
  close();
  addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return false;
  const double deadline = now_s() + timeout_s;
  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
    const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    // Non-blocking connect with a poll deadline, then back to blocking (reads use poll anyway).
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc != 0 && errno == EINPROGRESS) {
      pollfd p{fd, POLLOUT, 0};
      const int ms = static_cast<int>(std::max(0.0, deadline - now_s()) * 1000);
      if (::poll(&p, 1, ms) == 1) {
        int err = 0;
        socklen_t elen = sizeof err;
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
        rc = err == 0 ? 0 : -1;
      } else {
        rc = -1;
      }
    }
    if (rc == 0) {
      ::fcntl(fd, F_SETFL, flags);
      fd_ = fd;
      break;
    }
    ::close(fd);
  }
  ::freeaddrinfo(res);
  if (fd_ < 0) return false;

  // Handshake: random 16-byte key, wait for the full header block, verify the accept value.
  std::string raw_key(16, '\0');
  for (char& c : raw_key) c = static_cast<char>(rng_() & 0xFF);
  const std::string key = ws_base64(raw_key);
  if (!send_all_(ws_handshake_request(host, port, key))) return false;
  std::string headers;
  char buf[512];
  while (headers.find("\r\n\r\n") == std::string::npos) {
    const int ms = static_cast<int>(std::max(0.0, deadline - now_s()) * 1000);
    pollfd p{fd_, POLLIN, 0};
    if (::poll(&p, 1, ms) != 1) { close(); return false; }
    const ssize_t n = ::read(fd_, buf, sizeof buf);
    if (n <= 0) { close(); return false; }
    headers.append(buf, static_cast<std::size_t>(n));
    if (headers.size() > 64 * 1024) { close(); return false; }   // header bomb guard
  }
  const std::size_t hdr_end = headers.find("\r\n\r\n") + 4;
  if (!ws_handshake_ok(headers.substr(0, hdr_end), key)) { close(); return false; }
  // Bytes after the header block are already frames — feed them to the decoder.
  if (hdr_end < headers.size()) dec_.feed(headers.data() + hdr_end, headers.size() - hdr_end);
  return true;
}

WsRecv WsClient::recv_text(double timeout_s, std::string& out) {
  if (fd_ < 0) return WsRecv::Closed;
  const double deadline = now_s() + timeout_s;
  char buf[4096];
  for (;;) {
    WsMessage m;
    while (dec_.pop(m)) {
      switch (m.op) {
        case WsOp::Text: out = std::move(m.payload); return WsRecv::Text;
        case WsOp::Ping: send_frame_(WsOp::Pong, m.payload); break;
        case WsOp::Close: close(); return WsRecv::Closed;
        default: break;                                  // Pong/Binary: ignore
      }
    }
    if (dec_.error()) { close(); return WsRecv::Error; }
    const int ms = static_cast<int>(std::max(0.0, deadline - now_s()) * 1000);
    if (ms <= 0) return WsRecv::Timeout;
    pollfd p{fd_, POLLIN, 0};
    const int pr = ::poll(&p, 1, ms);
    if (pr == 0) return WsRecv::Timeout;
    if (pr < 0) {
      if (errno == EINTR) continue;
      close();
      return WsRecv::Error;
    }
    const ssize_t n = ::read(fd_, buf, sizeof buf);
    if (n == 0) { close(); return WsRecv::Closed; }
    if (n < 0) {
      if (errno == EINTR) continue;
      close();
      return WsRecv::Error;
    }
    dec_.feed(buf, static_cast<std::size_t>(n));
  }
}
}  // namespace hades
```

- [ ] **Step 4: Build + test.** `-R WsClient` → pass; full suite green.
- [ ] **Step 5: Commit.**

```bash
git add src/apps/simplex/ws.cpp tests/test_ws_client.cpp CMakeLists.txt
git commit -m "feat: blocking WsClient — POSIX connect/handshake, masked sends, ping-answering recv"
```

---

## Task 3: SimplexApi seam — event parse + corrId command round-trips

**Files:**
- Create: `include/hades/simplex/api.h`, `src/apps/simplex/simplex.cpp` (parse + `WsSimplexApi`; Task 4 appends the module)
- Test: `tests/test_simplex_parse.cpp`, `tests/test_simplex_api.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `WsClient`/`WsRecv` (Task 2).
- Produces (`namespace hades`):

```cpp
struct SxEvent {
  enum class Kind { None, Text, ContactRequest, Connected };
  Kind kind = Kind::None;
  long long contact_id = 0;     // Text / Connected
  std::string display_name;     // all kinds
  long long request_id = 0;     // ContactRequest
  std::string text;             // Text
};
enum class SxStatus { Event, Timeout, Closed, Error };
class SimplexApi {
 public:
  virtual ~SimplexApi() = default;
  virtual SxStatus next_event(double timeout_s, SxEvent& out) = 0;
  virtual bool send_text(long long contact_id, const std::string& text) = 0;
  virtual bool accept_request(long long request_id) = 0;
  virtual bool reconnect() = 0;   // (re)establish; module backs off between attempts
};
std::vector<SxEvent> parse_simplex_events(const std::string& frame_json);  // pure, tolerant
class WsSimplexApi : public SimplexApi { public: WsSimplexApi(std::string host, int port, double connect_timeout_s); /* + overrides */ };
```

Task 4 relies on these exact names/semantics. `next_event` returns queued events first; `send_text`/`accept_request` are corrId round-trips that QUEUE any events received while waiting.

- [ ] **Step 1: Write the failing parse tests** `tests/test_simplex_parse.cpp`:

```cpp
// tests/test_simplex_parse.cpp — tolerant daemon-event parsing (canned frames, pure)
#include <gtest/gtest.h>
#include <string>
#include <nlohmann/json.hpp>
#include "hades/simplex/api.h"
using namespace hades;

namespace {
// A canned newChatItems frame with one direct received text item.
std::string direct_text_frame(long long cid, const std::string& name, const std::string& text) {
  nlohmann::json item{
      {"chatInfo", {{"type", "direct"}, {"contact", {{"contactId", cid}, {"localDisplayName", name}}}}},
      {"chatItem",
       {{"chatDir", {{"type", "directRcv"}}},
        {"content", {{"type", "rcvMsgContent"}, {"msgContent", {{"type", "text"}, {"text", text}}}}}}}};
  nlohmann::json f{{"resp", {{"type", "newChatItems"}, {"chatItems", nlohmann::json::array({item})}}}};
  return f.dump();
}
}  // namespace

TEST(SimplexParse, DirectReceivedTextYieldsTextEvent) {
  auto evs = parse_simplex_events(direct_text_frame(2, "Vaios K", "hello agent"));
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].kind, SxEvent::Kind::Text);
  EXPECT_EQ(evs[0].contact_id, 2);
  EXPECT_EQ(evs[0].display_name, "Vaios K");
  EXPECT_EQ(evs[0].text, "hello agent");
}

TEST(SimplexParse, RightWrapperIsUnwrapped) {
  // Some CLI builds encode resp as a Haskell Either: {"resp":{"Right":{...}}}.
  nlohmann::json inner = nlohmann::json::parse(direct_text_frame(3, "N", "hi"))["resp"];
  nlohmann::json f{{"resp", {{"Right", inner}}}};
  auto evs = parse_simplex_events(f.dump());
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].contact_id, 3);
}

TEST(SimplexParse, OwnEchoGroupAndNonTextAreSkipped) {
  // directSnd (our own sent item), a group item, and a voice msgContent: all skipped.
  nlohmann::json snd{
      {"chatInfo", {{"type", "direct"}, {"contact", {{"contactId", 2}, {"localDisplayName", "V"}}}}},
      {"chatItem",
       {{"chatDir", {{"type", "directSnd"}}},
        {"content", {{"type", "rcvMsgContent"}, {"msgContent", {{"type", "text"}, {"text", "me"}}}}}}}};
  nlohmann::json grp{
      {"chatInfo", {{"type", "group"}, {"groupInfo", {{"groupId", 7}}}}},
      {"chatItem",
       {{"chatDir", {{"type", "groupRcv"}}},
        {"content", {{"type", "rcvMsgContent"}, {"msgContent", {{"type", "text"}, {"text", "grp"}}}}}}}};
  nlohmann::json voice{
      {"chatInfo", {{"type", "direct"}, {"contact", {{"contactId", 2}, {"localDisplayName", "V"}}}}},
      {"chatItem",
       {{"chatDir", {{"type", "directRcv"}}},
        {"content", {{"type", "rcvMsgContent"}, {"msgContent", {{"type", "voice"}, {"text", ""}}}}}}}};
  nlohmann::json f{{"resp", {{"type", "newChatItems"},
                             {"chatItems", nlohmann::json::array({snd, grp, voice})}}}};
  EXPECT_TRUE(parse_simplex_events(f.dump()).empty());
}

TEST(SimplexParse, MultipleItemsYieldMultipleEvents) {
  nlohmann::json a = nlohmann::json::parse(direct_text_frame(2, "V", "one"));
  nlohmann::json b = nlohmann::json::parse(direct_text_frame(5, "W", "two"));
  nlohmann::json f{{"resp", {{"type", "newChatItems"},
                             {"chatItems", nlohmann::json::array(
                                 {a["resp"]["chatItems"][0], b["resp"]["chatItems"][0]})}}}};
  auto evs = parse_simplex_events(f.dump());
  ASSERT_EQ(evs.size(), 2u);
  EXPECT_EQ(evs[0].text, "one");
  EXPECT_EQ(evs[1].contact_id, 5);
}

TEST(SimplexParse, ContactRequestEvent) {
  nlohmann::json f{{"resp", {{"type", "receivedContactRequest"},
                             {"contactRequest", {{"contactRequestId", 9},
                                                 {"localDisplayName", "stranger"}}}}}};
  auto evs = parse_simplex_events(f.dump());
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].kind, SxEvent::Kind::ContactRequest);
  EXPECT_EQ(evs[0].request_id, 9);
  EXPECT_EQ(evs[0].display_name, "stranger");
}

TEST(SimplexParse, ContactConnectedEvent) {
  nlohmann::json f{{"resp", {{"type", "contactConnected"},
                             {"contact", {{"contactId", 4}, {"localDisplayName", "friend"}}}}}};
  auto evs = parse_simplex_events(f.dump());
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].kind, SxEvent::Kind::Connected);
  EXPECT_EQ(evs[0].contact_id, 4);
  EXPECT_EQ(evs[0].display_name, "friend");
}

TEST(SimplexParse, GarbageAndUnknownTypesYieldNothing) {
  EXPECT_TRUE(parse_simplex_events("not json at all").empty());
  EXPECT_TRUE(parse_simplex_events("42").empty());
  EXPECT_TRUE(parse_simplex_events(R"({"resp":{"type":"somethingElse"}})").empty());
  EXPECT_TRUE(parse_simplex_events(R"({"resp":{"type":"newChatItems","chatItems":"nope"}})").empty());
  EXPECT_TRUE(parse_simplex_events(R"({"noresp":true})").empty());
}
```

- [ ] **Step 2: Write the failing api tests** `tests/test_simplex_api.cpp` (reuses the Task 2 fake server, extracted inline again — tests are self-contained):

```cpp
// tests/test_simplex_api.cpp — WsSimplexApi corrId round-trips + event queuing vs a fake daemon
#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <functional>
#include <string>
#include <thread>
#include <nlohmann/json.hpp>
#include "hades/simplex/api.h"
#include "hades/simplex/ws.h"
using namespace hades;

namespace {
struct FakeDaemon {
  int lfd = -1, cfd = -1, port = 0;
  std::thread th;
  WsDecoder dec;
  FakeDaemon() {
    lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_EQ(::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
    socklen_t alen = sizeof a;
    ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &alen);
    port = ntohs(a.sin_port);
    ::listen(lfd, 1);
  }
  ~FakeDaemon() {
    if (th.joinable()) th.join();
    if (cfd >= 0) ::close(cfd);
    if (lfd >= 0) ::close(lfd);
  }
  void run(std::function<void(FakeDaemon&)> script) {
    th = std::thread([this, script] {
      cfd = ::accept(lfd, nullptr, nullptr);
      if (cfd < 0) return;
      std::string req;
      char c;
      while (req.find("\r\n\r\n") == std::string::npos && ::read(cfd, &c, 1) == 1) req += c;
      const std::string marker = "Sec-WebSocket-Key: ";
      const std::size_t at = req.find(marker);
      const std::size_t end = req.find("\r\n", at);
      const std::string key = req.substr(at + marker.size(), end - at - marker.size());
      const std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                               "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                               ws_accept_value(key) + "\r\n\r\n";
      (void)!::write(cfd, resp.data(), resp.size());
      script(*this);
    });
  }
  nlohmann::json recv_cmd() {   // next client TEXT frame parsed as the {corrId,cmd} envelope
    WsMessage m;
    char buf[4096];
    while (!dec.pop(m)) {
      const ssize_t n = ::read(cfd, buf, sizeof buf);
      if (n <= 0) return nlohmann::json{};
      dec.feed(buf, static_cast<std::size_t>(n));
    }
    return nlohmann::json::parse(m.payload, nullptr, false);
  }
  void send_json(const nlohmann::json& j) {
    const std::string f = ws_encode_frame(WsOp::Text, j.dump(), false, 0);
    (void)!::write(cfd, f.data(), f.size());
  }
};

nlohmann::json text_event(long long cid, const std::string& name, const std::string& text) {
  return {{"resp",
           {{"type", "newChatItems"},
            {"chatItems", nlohmann::json::array(
                 {{{"chatInfo", {{"type", "direct"},
                                 {"contact", {{"contactId", cid}, {"localDisplayName", name}}}}},
                   {"chatItem", {{"chatDir", {{"type", "directRcv"}}},
                                 {"content", {{"type", "rcvMsgContent"},
                                              {"msgContent", {{"type", "text"}, {"text", text}}}}}}}}})}}}};
}
}  // namespace

TEST(WsSimplexApi, SendTextRoundTripMatchesCorrIdAndParsesCommand) {
  FakeDaemon d;
  nlohmann::json got_cmd;
  d.run([&got_cmd](FakeDaemon& s) {
    got_cmd = s.recv_cmd();
    s.send_json({{"corrId", got_cmd.value("corrId", "")},
                 {"resp", {{"type", "newChatItems"}, {"chatItems", nlohmann::json::array()}}}});
  });
  auto api = make_ws_simplex_api("127.0.0.1", d.port, 5.0);
  ASSERT_TRUE(api->reconnect());
  EXPECT_TRUE(api->send_text(2, "hi there"));
  ASSERT_TRUE(got_cmd.is_object());
  const std::string cmd = got_cmd.value("cmd", "");
  EXPECT_EQ(cmd.rfind("/_send @2 json ", 0), 0u);                  // exact command prefix
  auto payload = nlohmann::json::parse(cmd.substr(std::string("/_send @2 json ").size()),
                                       nullptr, false);
  ASSERT_TRUE(payload.is_array());
  EXPECT_EQ(payload[0]["msgContent"]["type"], "text");
  EXPECT_EQ(payload[0]["msgContent"]["text"], "hi there");
}

TEST(WsSimplexApi, CmdErrorYieldsFalse) {
  FakeDaemon d;
  d.run([](FakeDaemon& s) {
    auto cmd = s.recv_cmd();
    s.send_json({{"corrId", cmd.value("corrId", "")}, {"resp", {{"type", "chatCmdError"}}}});
  });
  auto api = make_ws_simplex_api("127.0.0.1", d.port, 5.0);
  ASSERT_TRUE(api->reconnect());
  EXPECT_FALSE(api->send_text(2, "boom"));
}

TEST(WsSimplexApi, EventArrivingDuringCommandIsQueued) {
  FakeDaemon d;
  d.run([](FakeDaemon& s) {
    auto cmd = s.recv_cmd();
    s.send_json(text_event(7, "Vaios", "interleaved"));            // event FIRST
    s.send_json({{"corrId", cmd.value("corrId", "")},
                 {"resp", {{"type", "newChatItems"}, {"chatItems", nlohmann::json::array()}}}});
  });
  auto api = make_ws_simplex_api("127.0.0.1", d.port, 5.0);
  ASSERT_TRUE(api->reconnect());
  EXPECT_TRUE(api->send_text(2, "x"));
  SxEvent ev;
  ASSERT_EQ(api->next_event(0.1, ev), SxStatus::Event);             // queued, no new read needed
  EXPECT_EQ(ev.kind, SxEvent::Kind::Text);
  EXPECT_EQ(ev.contact_id, 7);
  EXPECT_EQ(ev.text, "interleaved");
}

TEST(WsSimplexApi, NextEventDeliversPushedEventAndTimesOutWhenIdle) {
  FakeDaemon d;
  d.run([](FakeDaemon& s) {
    s.send_json(text_event(2, "V", "pushed"));
    WsMessage m;                                                   // then hold until client closes
    char buf[64];
    while (::read(s.cfd, buf, sizeof buf) > 0) {}
    (void)m;
  });
  auto api = make_ws_simplex_api("127.0.0.1", d.port, 5.0);
  ASSERT_TRUE(api->reconnect());
  SxEvent ev;
  ASSERT_EQ(api->next_event(5.0, ev), SxStatus::Event);
  EXPECT_EQ(ev.text, "pushed");
  EXPECT_EQ(api->next_event(0.2, ev), SxStatus::Timeout);
}

TEST(WsSimplexApi, AcceptRequestSendsAcceptCommand) {
  FakeDaemon d;
  nlohmann::json got_cmd;
  d.run([&got_cmd](FakeDaemon& s) {
    got_cmd = s.recv_cmd();
    s.send_json({{"corrId", got_cmd.value("corrId", "")},
                 {"resp", {{"type", "acceptingContactRequest"}}}});
  });
  auto api = make_ws_simplex_api("127.0.0.1", d.port, 5.0);
  ASSERT_TRUE(api->reconnect());
  EXPECT_TRUE(api->accept_request(9));
  EXPECT_EQ(got_cmd.value("cmd", ""), "/_accept 9");
}

TEST(WsSimplexApi, ClosedConnectionReportsClosed) {
  FakeDaemon d1;
  d1.run([](FakeDaemon& s) {
    const std::string f = ws_encode_frame(WsOp::Close, "", false, 0);
    (void)!::write(s.cfd, f.data(), f.size());
  });
  auto api = make_ws_simplex_api("127.0.0.1", d1.port, 5.0);
  ASSERT_TRUE(api->reconnect());
  SxEvent ev;
  EXPECT_EQ(api->next_event(5.0, ev), SxStatus::Closed);
  EXPECT_FALSE(api->send_text(1, "x"));                              // not connected -> false
}
```

- [ ] **Step 3: CMake + run — expect FAIL.**

```cmake
target_sources(hades_core PRIVATE src/apps/simplex/simplex.cpp)
target_sources(hades_tests PRIVATE tests/test_simplex_parse.cpp tests/test_simplex_api.cpp)
```

- [ ] **Step 4: Implement.** `include/hades/simplex/api.h`:

```cpp
// include/hades/simplex/api.h — SimpleX daemon seam (real impl: WsSimplexApi; tests: scripted fake)
//
// The SimplexModule talks ONLY to this interface (TelegramApi precedent), so its allowlist/
// turn/confirm logic is testable without a daemon. Events are already-parsed SxEvents; commands
// return false on failure (fail-soft — the module logs and carries on). next_event returns
// queued events first (events can arrive while a command round-trip is waiting for its resp).
#pragma once
#include <memory>
#include <string>
#include <vector>
namespace hades {

struct SxEvent {
  enum class Kind { None, Text, ContactRequest, Connected };
  Kind kind = Kind::None;
  long long contact_id = 0;     // Text / Connected
  std::string display_name;     // all kinds (the sender's local display name)
  long long request_id = 0;     // ContactRequest
  std::string text;             // Text
};

enum class SxStatus { Event, Timeout, Closed, Error };

class SimplexApi {
 public:
  virtual ~SimplexApi() = default;
  virtual SxStatus next_event(double timeout_s, SxEvent& out) = 0;
  virtual bool send_text(long long contact_id, const std::string& text) = 0;
  virtual bool accept_request(long long request_id) = 0;
  virtual bool reconnect() = 0;   // (re)establish the connection; module backs off between tries
};

// Pure, tolerant event parse for one daemon frame: {"resp":{...}} (an optional {"Right":...}
// Either-wrapper is unwrapped). Yields Text (direct+directRcv+rcvMsgContent+text only),
// ContactRequest, Connected; everything else -> {}. Never throws.
std::vector<SxEvent> parse_simplex_events(const std::string& frame_json);

// The real seam impl (WsSimplexApi over WsClient) is file-local in simplex.cpp; this factory
// is its only exposure (the module self-builds it in on_start; tests script a FakeApi instead).
std::unique_ptr<SimplexApi> make_ws_simplex_api(std::string host, int port,
                                                double connect_timeout_s);

}  // namespace hades
```

`src/apps/simplex/simplex.cpp` (Task 3 part):

```cpp
// src/apps/simplex/simplex.cpp — the SimpleX front-end app: event parse + WsSimplexApi + module
//
// parse_simplex_events: tolerant translation of daemon frames into SxEvents (canned-JSON tested).
// WsSimplexApi: the real SimplexApi over WsClient — corrId round-trips for commands, queuing any
// events that interleave; next_event pops the queue first. SimplexModule (Task 4): the front-end.
#include <deque>
#include <iostream>
#include <nlohmann/json.hpp>
#include "hades/simplex/api.h"
#include "hades/simplex/ws.h"

namespace hades {
namespace {
// {"resp": X}; X may be {"Right": Y}/{"Left": Y} (Haskell Either encoding in some CLI builds).
nlohmann::json unwrap_resp(const nlohmann::json& frame) {
  if (!frame.is_object()) return nullptr;
  auto it = frame.find("resp");
  if (it == frame.end() || !it->is_object()) return nullptr;
  nlohmann::json r = *it;
  if (r.contains("Right") && r["Right"].is_object()) r = r["Right"];
  else if (r.contains("Left") && r["Left"].is_object()) r = r["Left"];
  return r;
}
long long num(const nlohmann::json& j, const char* key) {
  auto it = j.find(key);
  return (it != j.end() && it->is_number_integer()) ? it->get<long long>() : 0;
}
std::string str(const nlohmann::json& j, const char* key) {
  auto it = j.find(key);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}
}  // namespace

std::vector<SxEvent> parse_simplex_events(const std::string& frame_json) {
  std::vector<SxEvent> out;
  auto frame = nlohmann::json::parse(frame_json, nullptr, false);
  if (frame.is_discarded()) return out;
  const nlohmann::json resp = unwrap_resp(frame);
  if (!resp.is_object()) return out;
  const std::string type = str(resp, "type");

  if (type == "newChatItems") {
    auto items = resp.find("chatItems");
    if (items == resp.end() || !items->is_array()) return out;
    for (const auto& it : *items) {
      if (!it.is_object()) continue;
      const auto& ci = it.value("chatInfo", nlohmann::json::object());
      const auto& item = it.value("chatItem", nlohmann::json::object());
      if (str(ci, "type") != "direct") continue;                        // v1: DMs only
      const auto& contact = ci.value("contact", nlohmann::json::object());
      const auto& dir = item.value("chatDir", nlohmann::json::object());
      if (str(dir, "type") != "directRcv") continue;                    // skip our own echoes
      const auto& content = item.value("content", nlohmann::json::object());
      if (str(content, "type") != "rcvMsgContent") continue;
      const auto& mc = content.value("msgContent", nlohmann::json::object());
      if (str(mc, "type") != "text") continue;                          // v1: text only
      SxEvent ev;
      ev.kind = SxEvent::Kind::Text;
      ev.contact_id = num(contact, "contactId");
      ev.display_name = str(contact, "localDisplayName");
      ev.text = str(mc, "text");
      if (ev.contact_id != 0 && !ev.text.empty()) out.push_back(std::move(ev));
    }
  } else if (type == "receivedContactRequest") {
    const auto& cr = resp.value("contactRequest", nlohmann::json::object());
    SxEvent ev;
    ev.kind = SxEvent::Kind::ContactRequest;
    ev.request_id = num(cr, "contactRequestId");
    ev.display_name = str(cr, "localDisplayName");
    if (ev.request_id != 0) out.push_back(std::move(ev));
  } else if (type == "contactConnected") {
    const auto& c = resp.value("contact", nlohmann::json::object());
    SxEvent ev;
    ev.kind = SxEvent::Kind::Connected;
    ev.contact_id = num(c, "contactId");
    ev.display_name = str(c, "localDisplayName");
    if (ev.contact_id != 0) out.push_back(std::move(ev));
  }
  return out;
}

// ── WsSimplexApi: the real seam impl over WsClient (file-local; exposed via the factory) ─────
namespace {
constexpr double kCmdTimeoutS = 15.0;   // one command round-trip against the LOCAL daemon

class WsSimplexApi : public SimplexApi {
 public:
  WsSimplexApi(std::string host, int port, double connect_timeout_s)
      : host_(std::move(host)), port_(port), connect_timeout_s_(connect_timeout_s) {}

  bool reconnect() override {
    pending_.clear();
    return ws_.connect(host_, port_, connect_timeout_s_);
  }

  SxStatus next_event(double timeout_s, SxEvent& out) override {
    if (!pending_.empty()) {
      out = pending_.front();
      pending_.pop_front();
      return SxStatus::Event;
    }
    std::string frame;
    switch (ws_.recv_text(timeout_s, frame)) {
      case WsRecv::Timeout: return SxStatus::Timeout;
      case WsRecv::Closed: return SxStatus::Closed;
      case WsRecv::Error: return SxStatus::Error;
      case WsRecv::Text: break;
    }
    for (auto& ev : parse_simplex_events(frame)) pending_.push_back(std::move(ev));
    if (pending_.empty()) return SxStatus::Timeout;   // frame parsed to nothing: caller re-loops
    out = pending_.front();
    pending_.pop_front();
    return SxStatus::Event;
  }

  bool send_text(long long contact_id, const std::string& text) override {
    nlohmann::json msgs = nlohmann::json::array(
        {{{"msgContent", {{"type", "text"}, {"text", text}}}}});
    return command_ok_("/_send @" + std::to_string(contact_id) + " json " + msgs.dump(),
                       "newChatItems");
  }

  bool accept_request(long long request_id) override {
    return command_ok_("/_accept " + std::to_string(request_id), "acceptingContactRequest");
  }

 private:
  // Send {corrId,cmd}; read until the matching resp (events seen meanwhile are queued).
  // ok iff the resp type equals ok_type. Total wait bounded by kCmdTimeoutS even across a
  // stream of non-matching frames. Timeout/close/error -> false (fail-soft; caller logs).
  bool command_ok_(const std::string& cmd, const char* ok_type) {
    if (!ws_.connected()) return false;
    const std::string corr = std::to_string(++corr_);
    nlohmann::json env{{"corrId", corr}, {"cmd", cmd}};
    if (!ws_.send_text(env.dump())) return false;
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
      const double elapsed =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      const double remaining = kCmdTimeoutS - elapsed;
      if (remaining <= 0.0) return false;
      std::string frame;
      if (ws_.recv_text(remaining, frame) != WsRecv::Text) return false;
      auto j = nlohmann::json::parse(frame, nullptr, false);
      if (j.is_object() && j.value("corrId", "") == corr) {
        const nlohmann::json resp = unwrap_resp(j);
        return resp.is_object() && str(resp, "type") == ok_type;
      }
      for (auto& ev : parse_simplex_events(frame)) pending_.push_back(std::move(ev));
    }
  }

  std::string host_;
  int port_;
  double connect_timeout_s_;
  WsClient ws_;
  long long corr_ = 0;
  std::deque<SxEvent> pending_;
};
}  // anonymous namespace

std::unique_ptr<SimplexApi> make_ws_simplex_api(std::string host, int port,
                                                double connect_timeout_s) {
  return std::make_unique<WsSimplexApi>(std::move(host), port, connect_timeout_s);
}
}  // namespace hades
```

(Add `#include <chrono>` and `#include <memory>` to the file's include block. The `unwrap_resp`/`num`/`str` helpers sit in the same anonymous namespace above — keep the class after them.)

- [ ] **Step 5: Build + test.** `-R SimplexParse` and `-R WsSimplexApi` → pass; full suite green.
- [ ] **Step 6: Commit.**

```bash
git add include/hades/simplex/api.h src/apps/simplex/simplex.cpp tests/test_simplex_parse.cpp tests/test_simplex_api.cpp CMakeLists.txt
git commit -m "feat: SimplexApi seam — tolerant event parse + WsSimplexApi corrId round-trips over the WS client"
```

---

## Task 4: SimplexModule — thread, allowlist, turns, text-y/N confirms, notify

**Files:**
- Create: `include/hades/module/simplex_module.h`
- Modify: `src/apps/simplex/simplex.cpp` (append the module)
- Test: `tests/test_simplex_module.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `SimplexApi`/`SxEvent`/`SxStatus` + `make_ws_simplex_api` (Task 3); `TurnGate` (`hades/turn_gate.h`); `split_message` (`hades/telegram/parse.h`); `kDefaultTurnIdleTimeoutS` (`hades/timeouts.h`); `MalConfig` (`hades/launcher.h`); `set_bool_on_string`/`set_pos_double_on_string` (`hades/config.h`).
- Produces: `class SimplexModule : public Module` — `type()=="simplex"`; test ctor `explicit SimplexModule(std::unique_ptr<SimplexApi>)`; `on_start(cfg, bb)` (parses the `Simplex` block; **MalConfig without a non-empty `allow_contacts`**); `on_attach(bb)`; `set_turn_gate(TurnGate*)`; `set_turn_timeout_s(double)`; `start()`; `wait()`; public test seam `bool step_once()` (one `next_event` dispatch; returns false on Closed/Error → the loop backs off + reconnects). Task 5 relies on exactly these names.

- [ ] **Step 1: Write the failing tests** `tests/test_simplex_module.cpp`:

```cpp
// tests/test_simplex_module.cpp — SimplexModule allowlist/turn/confirm/notify over a fake api
#include <gtest/gtest.h>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "hades/blackboard.h"
#include "hades/launcher.h"          // MalConfig
#include "hades/module/simplex_module.h"
using namespace hades;

namespace {
struct FakeApi : SimplexApi {
  std::deque<SxEvent> events;                                    // popped per next_event
  std::vector<std::pair<long long, std::string>> sent;           // send_text calls
  std::vector<long long> accepted;                               // accept_request calls
  int reconnects = 0;
  // Empty script -> Closed (NOT Timeout) so the Rig's pump_events loop terminates; it also
  // exercises the loop-ends-on-Closed contract on every test.
  SxStatus next_event(double, SxEvent& out) override {
    if (events.empty()) return SxStatus::Closed;
    out = events.front();
    events.pop_front();
    return SxStatus::Event;
  }
  bool send_text(long long cid, const std::string& t) override {
    sent.push_back({cid, t});
    return true;
  }
  bool accept_request(long long rid) override {
    accepted.push_back(rid);
    return true;
  }
  bool reconnect() override { ++reconnects; return true; }
};

SxEvent text_ev(long long cid, const std::string& name, const std::string& text) {
  SxEvent e; e.kind = SxEvent::Kind::Text; e.contact_id = cid; e.display_name = name; e.text = text;
  return e;
}
SxEvent request_ev(long long rid, const std::string& name) {
  SxEvent e; e.kind = SxEvent::Kind::ContactRequest; e.request_id = rid; e.display_name = name;
  return e;
}
SxEvent connected_ev(long long cid, const std::string& name) {
  SxEvent e; e.kind = SxEvent::Kind::Connected; e.contact_id = cid; e.display_name = name;
  return e;
}

// Rig over the fake api. allow_contacts = "2, Vaios K". echo=true installs a plain echo agent.
struct Rig {
  Blackboard bb;
  FakeApi* api;
  std::unique_ptr<SimplexModule> mod;
  explicit Rig(bool echo = true, const std::string& extra_key = "", const std::string& extra_val = "") {
    auto a = std::make_unique<FakeApi>();
    api = a.get();
    mod = std::make_unique<SimplexModule>(std::move(a));
    Block cfg;
    cfg.kv["allow_contacts"] = "2, Vaios K";
    if (!extra_key.empty()) cfg.kv[extra_key] = extra_val;
    mod->on_start(cfg, bb);
    mod->on_attach(bb);
    if (echo)
      bb.subscribe("USER_MESSAGE", [this](const Entry& e) {
        bb.post("ASSISTANT_MESSAGE", "echo:" + e.value.get<std::string>(), "t");
      });
  }
  void pump_events() { while (mod->step_once()) {} }   // drains the script; ends on Closed
};
}  // namespace

TEST(SimplexModule, MissingOrEmptyAllowContactsThrows) {
  Blackboard bb;
  {
    SimplexModule m(std::make_unique<FakeApi>());
    Block cfg;                                        // no allow_contacts at all
    EXPECT_THROW(m.on_start(cfg, bb), MalConfig);
  }
  {
    SimplexModule m(std::make_unique<FakeApi>());
    Block cfg;
    cfg.kv["allow_contacts"] = "  ,  ";               // empty after split/trim
    EXPECT_THROW(m.on_start(cfg, bb), MalConfig);
  }
}

TEST(SimplexModule, AllowedIdDrivesTurnAndReplies) {
  Rig r;
  r.api->events.push_back(text_ev(2, "whoever", "hi"));
  r.pump_events();
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_EQ(r.api->sent[0].first, 2);
  EXPECT_EQ(r.api->sent[0].second, "echo:hi");
}

TEST(SimplexModule, AllowedDisplayNameDrivesTurn) {
  Rig r;
  r.api->events.push_back(text_ev(77, "Vaios K", "name match"));
  r.pump_events();
  ASSERT_EQ(r.api->sent.size(), 1u);
  EXPECT_EQ(r.api->sent[0].first, 77);                // reply goes to the sender's id
}

TEST(SimplexModule, NonAllowedSenderSilentlyDropped) {
  Rig r;
  bool user_msg = false;
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) { user_msg = true; });
  r.api->events.push_back(text_ev(666, "stranger", "let me in"));
  r.pump_events();
  EXPECT_FALSE(user_msg);
  EXPECT_TRUE(r.api->sent.empty());
}

TEST(SimplexModule, LongReplySplitAt4000) {
  Rig r(false);
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) {
    r.bb.post("ASSISTANT_MESSAGE", std::string(4001, 'z'), "t");
  });
  r.api->events.push_back(text_ev(2, "V", "long please"));
  r.pump_events();
  ASSERT_EQ(r.api->sent.size(), 2u);
  EXPECT_EQ(r.api->sent[0].second.size(), 4000u);
  EXPECT_EQ(r.api->sent[1].second.size(), 1u);
}

TEST(SimplexModule, ConfirmYesApprovesViaTextReply) {
  Rig r(false);
  // Turn 1: the agent asks for confirmation. Turn 2 (the CONFIRM_RESPONSE): replies.
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) {
    r.bb.post("CONFIRM_REQUEST", {{"id", "c1"}, {"prompt", "rm -rf /tmp/x — sure?"}}, "arbiter");
  });
  nlohmann::json confirm_resp;
  r.bb.subscribe("CONFIRM_RESPONSE", [&](const Entry& e) {
    confirm_resp = e.value;
    r.bb.post("ASSISTANT_MESSAGE", "done", "t");
  });
  r.api->events.push_back(text_ev(2, "V", "do the risky thing"));
  r.pump_events();
  ASSERT_EQ(r.api->sent.size(), 1u);                              // the y/N prompt text
  EXPECT_NE(r.api->sent[0].second.find("rm -rf /tmp/x"), std::string::npos);
  EXPECT_NE(r.api->sent[0].second.find("reply y"), std::string::npos);
  r.api->events.push_back(text_ev(2, "V", "  Y  "));              // case+space tolerant approve
  r.pump_events();
  ASSERT_TRUE(confirm_resp.is_object());
  EXPECT_EQ(confirm_resp.value("id", ""), "c1");
  EXPECT_TRUE(confirm_resp.value("approved", false));
  EXPECT_EQ(r.api->sent.back().second, "done");
}

TEST(SimplexModule, ConfirmAnythingElseDenies) {
  Rig r(false);
  r.bb.subscribe("USER_MESSAGE", [&](const Entry&) {
    r.bb.post("CONFIRM_REQUEST", {{"id", "c2"}, {"prompt", "sure?"}}, "arbiter");
  });
  nlohmann::json confirm_resp;
  r.bb.subscribe("CONFIRM_RESPONSE", [&](const Entry& e) {
    confirm_resp = e.value;
    r.bb.post("ASSISTANT_MESSAGE", "declined then", "t");
  });
  r.api->events.push_back(text_ev(2, "V", "risky"));
  r.pump_events();
  r.api->events.push_back(text_ev(2, "V", "actually never mind"));
  r.pump_events();
  ASSERT_TRUE(confirm_resp.is_object());
  EXPECT_FALSE(confirm_resp.value("approved", true));
}

TEST(SimplexModule, ConfirmAnswerMustComeFromSameContact) {
  Rig r(false);
  r.bb.subscribe("USER_MESSAGE", [&](const Entry& e) {
    // Only the FIRST user message triggers a confirm; the other contact's text is a normal turn.
    static bool first = true;
    if (first) {
      first = false;
      r.bb.post("CONFIRM_REQUEST", {{"id", "c3"}, {"prompt", "sure?"}}, "arbiter");
    } else {
      r.bb.post("ASSISTANT_MESSAGE", "other turn: " + e.value.get<std::string>(), "t");
    }
  });
  bool confirm_answered = false;
  r.bb.subscribe("CONFIRM_RESPONSE", [&](const Entry&) { confirm_answered = true; });
  r.api->events.push_back(text_ev(2, "V", "risky"));
  r.pump_events();
  // A DIFFERENT allowlisted contact (by name) speaks: must NOT be consumed as the confirm answer.
  r.api->events.push_back(text_ev(77, "Vaios K", "unrelated"));
  r.pump_events();
  EXPECT_FALSE(confirm_answered);
  EXPECT_EQ(r.api->sent.back().second, "other turn: unrelated");
}

TEST(SimplexModule, AutoAcceptOffLogsAndIgnoresOnAccepts) {
  {
    Rig r;                                                        // default auto_accept=false
    r.api->events.push_back(request_ev(9, "stranger"));
    r.pump_events();
    EXPECT_TRUE(r.api->accepted.empty());
  }
  {
    Rig r(true, "auto_accept", "true");
    r.api->events.push_back(request_ev(9, "stranger"));
    r.pump_events();
    ASSERT_EQ(r.api->accepted.size(), 1u);
    EXPECT_EQ(r.api->accepted[0], 9);
  }
}

TEST(SimplexModule, NotifyContactByIdAndByResolvedName) {
  {
    Rig r(true, "notify_contact", "2");
    r.bb.post("NOTIFY_USER", {{"text", "heartbeat says hi"}, {"from", "heartbeat"}}, "hb");
    r.bb.pump();
    ASSERT_EQ(r.api->sent.size(), 1u);
    EXPECT_EQ(r.api->sent[0].first, 2);
    EXPECT_EQ(r.api->sent[0].second, "heartbeat says hi");
  }
  {
    Rig r(true, "notify_contact", "Vaios K");
    // Name not yet resolvable -> delivery skipped (logged), no crash.
    r.bb.post("NOTIFY_USER", {{"text", "too early"}}, "hb");
    r.bb.pump();
    EXPECT_TRUE(r.api->sent.empty());
    // A Connected event teaches the name->id mapping; the next notify delivers.
    r.api->events.push_back(connected_ev(42, "Vaios K"));
    r.pump_events();
    r.bb.post("NOTIFY_USER", {{"text", "now it works"}}, "hb");
    r.bb.pump();
    ASSERT_EQ(r.api->sent.size(), 1u);
    EXPECT_EQ(r.api->sent[0].first, 42);
  }
}

TEST(SimplexModule, ClosedYieldsFalseForTheReconnectPath) {
  Rig r;
  EXPECT_FALSE(r.mod->step_once());        // empty script = Closed -> false (loop reconnects)
}
```

- [ ] **Step 2: CMake + run — expect FAIL.**

```cmake
target_sources(hades_tests PRIVATE tests/test_simplex_module.cpp)
```

- [ ] **Step 3: Implement.** `include/hades/module/simplex_module.h`:

```cpp
// include/hades/module/simplex_module.h — SimpleX Chat front-end app (comms-interface analogue)
//
// Reads events from a local simplex-chat daemon (via the SimplexApi seam) on its own thread and
// drives whole turns through the shared TurnGate, exactly like the Telegram front-end: lock ->
// post USER_MESSAGE -> run_until(reply|confirm) -> send_text (split at 4000). Confirms are TEXT
// y/N (SimpleX has no inline keyboards): the next message from the SAME contact answers an
// outstanding confirm. Security: allow_contacts (ids and/or exact display names) is REQUIRED
// (MalConfig without it); non-allowed senders are silently dropped; contact requests are only
// auto-accepted when auto_accept=true (name-spoof risk documented in the manifest reference).
// The thread is started EXPLICITLY (start(), from hades_main) — never by on_attach — and is
// stop+joined in the dtor (telegram precedent).
#pragma once
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <nlohmann/json.hpp>
#include "hades/module.h"
#include "hades/simplex/api.h"
#include "hades/turn_gate.h"
namespace hades {
class Blackboard;

class SimplexModule : public Module {
 public:
  SimplexModule() = default;
  explicit SimplexModule(std::unique_ptr<SimplexApi> api);   // test injection (skips the WS api)
  ~SimplexModule() override;                                 // stop + join the event thread
  std::string type() const override { return "simplex"; }
  void on_start(const Block& cfg, Blackboard& bb) override;
  void on_attach(Blackboard& bb) override;

  void set_turn_gate(TurnGate* g) { gate_ = g; }
  void set_turn_timeout_s(double s) { turn_timeout_override_s_ = s; }

  void start();          // spawn the event loop (called by hades_main when rostered)
  void wait();           // join the event thread (simplex-only roster blocks here)
  // One next_event dispatch (the loop body; public as the test seam). Returns false on
  // Closed/Error — the loop then backs off and reconnects.
  bool step_once();

 private:
  void run_loop_();
  void handle_event_(const SxEvent& ev);
  void handle_text_(const SxEvent& ev);
  void drive_turn_(long long contact_id, const nlohmann::json& post_value, const char* key);
  void send_reply_(long long contact_id, const std::string& text);
  bool allowed_(const SxEvent& ev) const;
  std::mutex& turn_mu_() { return gate_ ? gate_->mu : local_gate_.mu; }
  double effective_timeout_() const;

  std::unique_ptr<SimplexApi> api_;
  Blackboard* bb_ = nullptr;
  TurnGate* gate_ = nullptr;
  TurnGate local_gate_;
  std::set<long long> allow_ids_;
  std::set<std::string> allow_names_;
  std::map<std::string, long long> known_ids_;   // display name -> contact id (learned from events)
  bool auto_accept_ = false;
  std::string notify_contact_;                   // id-or-name; "" = no notify delivery
  double connect_timeout_s_ = 10.0;
  double poll_timeout_s_ = 25.0;                 // internal next_event wait per loop pass
  double turn_timeout_override_s_ = 0.0;

  // Turn-capture state (event thread only, under the gate while a turn runs).
  bool my_turn_ = false;
  bool got_reply_ = false;
  std::string last_reply_;
  nlohmann::json pending_confirm_;
  std::string outstanding_confirm_id_;           // confirm prompt sent, awaiting the y/N text
  long long outstanding_contact_id_ = 0;

  std::thread ev_thread_;
  std::atomic<bool> stop_{false};
  std::condition_variable stop_cv_;
  std::mutex stop_mu_;
};
}  // namespace hades
```

Append to `src/apps/simplex/simplex.cpp` (module section — mirrors telegram.cpp structure):

```cpp
// ── SimplexModule: event loop, allowlist, turn driving, text y/N confirms ─────────────────────
#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include "hades/module/simplex_module.h"
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/launcher.h"          // MalConfig
#include "hades/telegram/parse.h"    // split_message
#include "hades/timeouts.h"          // kDefaultTurnIdleTimeoutS

namespace hades {
namespace {
constexpr std::size_t kSimplexSplit = 4000;

std::string trim(const std::string& s) {
  auto ns = [](unsigned char c) { return !std::isspace(c); };
  auto b = std::find_if(s.begin(), s.end(), ns);
  auto e = std::find_if(s.rbegin(), s.rend(), ns).base();
  return (b < e) ? std::string(b, e) : std::string{};
}
bool is_yes(const std::string& raw) {
  std::string t = trim(raw);
  std::transform(t.begin(), t.end(), t.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return t == "y" || t == "yes";
}
// Numeric token (all digits) -> id; anything else -> display name. Comma-separated, trimmed.
void split_contacts(const std::string& raw, std::set<long long>& ids, std::set<std::string>& names) {
  std::stringstream ss(raw);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    tok = trim(tok);
    if (tok.empty()) continue;
    if (std::all_of(tok.begin(), tok.end(), [](unsigned char c) { return std::isdigit(c); }))
      ids.insert(std::stoll(tok));
    else
      names.insert(tok);
  }
}
}  // namespace

SimplexModule::SimplexModule(std::unique_ptr<SimplexApi> api) : api_(std::move(api)) {}

SimplexModule::~SimplexModule() {
  stop_.store(true);
  stop_cv_.notify_all();
  // NOTE: a live next_event can hold the join up to ~poll_timeout_s_ (the WS read deadline).
  if (ev_thread_.joinable()) ev_thread_.join();
}

double SimplexModule::effective_timeout_() const {
  return turn_timeout_override_s_ > 0.0 ? turn_timeout_override_s_ : kDefaultTurnIdleTimeoutS;
}

void SimplexModule::on_start(const Block& cfg, Blackboard&) {
  // allow_contacts is REQUIRED — an open bot means anyone who connects can drive the agent's
  // tools (telegram allow_users precedent). Fail fast and loud.
  if (!cfg.kv.count("allow_contacts"))
    throw MalConfig("simplex module requires allow_contacts (contact ids and/or display names)");
  split_contacts(cfg.kv.at("allow_contacts"), allow_ids_, allow_names_);
  if (allow_ids_.empty() && allow_names_.empty())
    throw MalConfig("simplex module requires a non-empty allow_contacts");
  if (cfg.kv.count("auto_accept")) set_bool_on_string(cfg.kv.at("auto_accept"), auto_accept_);
  if (cfg.kv.count("notify_contact")) notify_contact_ = trim(cfg.kv.at("notify_contact"));
  if (cfg.kv.count("connect_timeout_s")) {
    double t = 0.0;
    if (set_pos_double_on_string(cfg.kv.at("connect_timeout_s"), t)) connect_timeout_s_ = t;
  }
  if (api_) return;                               // injected (tests)
  const std::string host = cfg.kv.count("host") ? cfg.kv.at("host") : "127.0.0.1";
  int port = 5225;
  if (cfg.kv.count("port")) {
    try { port = std::stoi(cfg.kv.at("port")); } catch (...) { port = 5225; }
  }
  api_ = make_ws_simplex_api(host, port, connect_timeout_s_);
}

void SimplexModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  bb.subscribe("ASSISTANT_MESSAGE", [this](const Entry& e) {
    if (!my_turn_ || !e.value.is_string()) return;
    last_reply_ = e.value.get<std::string>();
    got_reply_ = true;
  });
  bb.subscribe("CONFIRM_REQUEST", [this](const Entry& e) {
    if (!my_turn_ || !e.value.is_object()) return;
    pending_confirm_ = e.value;
  });
  // Notify sink (heartbeat etc.): deliver to the configured contact. Name form resolves via
  // known_ids_ (learned from Connected/Text events); unresolved -> logged skip. Fail-soft.
  bb.subscribe("NOTIFY_USER", [this](const Entry& e) {
    if (!api_ || notify_contact_.empty()) return;
    std::string text;
    if (e.value.is_object()) text = e.value.value("text", "");
    else if (e.value.is_string()) text = e.value.get<std::string>();
    if (text.empty()) return;
    long long cid = 0;
    if (std::all_of(notify_contact_.begin(), notify_contact_.end(),
                    [](unsigned char c) { return std::isdigit(c); })) {
      cid = std::stoll(notify_contact_);
    } else {
      auto it = known_ids_.find(notify_contact_);
      if (it != known_ids_.end()) cid = it->second;
    }
    if (cid == 0) {
      std::cerr << "hades: simplex notify skipped (contact not yet known: " << notify_contact_ << ")\n";
      return;
    }
    try {
      if (!api_->send_text(cid, text))
        std::cerr << "hades: simplex notify send failed (contact " << cid << ")\n";
    } catch (...) { /* fail-soft */ }
  });
}

bool SimplexModule::allowed_(const SxEvent& ev) const {
  return allow_ids_.count(ev.contact_id) || allow_names_.count(ev.display_name);
}

void SimplexModule::send_reply_(long long contact_id, const std::string& text) {
  for (const auto& chunk : split_message(text, kSimplexSplit))
    if (!api_->send_text(contact_id, chunk))
      std::cerr << "hades: simplex send failed (reply dropped; history is persisted)\n";
}

void SimplexModule::drive_turn_(long long contact_id, const nlohmann::json& post_value,
                                const char* key) {
  std::lock_guard<std::mutex> lk(turn_mu_());
  my_turn_ = true;
  struct Reset { bool& mine; ~Reset() { mine = false; } } reset{my_turn_};
  got_reply_ = false;
  last_reply_.clear();
  pending_confirm_ = nullptr;
  bb_->post("TURN_ORIGIN", "human", "simplex");
  bb_->post(key, post_value, "simplex");
  const bool done = bb_->run_until(
      [this] { return got_reply_ || !pending_confirm_.is_null(); }, effective_timeout_());
  if (!done) {
    bb_->post("TURN_ABANDONED", nlohmann::json::object(), "simplex");
    bb_->pump();
    send_reply_(contact_id, "[timed out]");
    return;
  }
  if (got_reply_) {
    send_reply_(contact_id, last_reply_);
    return;
  }
  // Confirm-gated: send a y/N text prompt; the next message from this contact answers it.
  const std::string id = pending_confirm_.value("id", "");
  const std::string prompt = pending_confirm_.value("prompt", "");
  outstanding_confirm_id_ = id;
  outstanding_contact_id_ = contact_id;
  send_reply_(contact_id, (prompt.empty() ? std::string("confirm?") : prompt) +
                              "\n(reply y to approve, anything else to deny)");
}

void SimplexModule::handle_text_(const SxEvent& ev) {
  // An outstanding confirm is answered by the NEXT message from the SAME contact.
  if (!outstanding_confirm_id_.empty() && ev.contact_id == outstanding_contact_id_) {
    const std::string id = outstanding_confirm_id_;
    outstanding_confirm_id_.clear();
    drive_turn_(ev.contact_id, nlohmann::json{{"id", id}, {"approved", is_yes(ev.text)}},
                "CONFIRM_RESPONSE");
    return;
  }
  drive_turn_(ev.contact_id, nlohmann::json(ev.text), "USER_MESSAGE");
}

void SimplexModule::handle_event_(const SxEvent& ev) {
  // Learn name->id from any event carrying both (notify_contact name resolution).
  if (ev.contact_id != 0 && !ev.display_name.empty()) known_ids_[ev.display_name] = ev.contact_id;
  switch (ev.kind) {
    case SxEvent::Kind::Text:
      if (allowed_(ev)) handle_text_(ev);       // non-allowed: silently dropped
      break;
    case SxEvent::Kind::ContactRequest:
      if (auto_accept_) {
        if (!api_->accept_request(ev.request_id))
          std::cerr << "hades: simplex accept_request failed (" << ev.request_id << ")\n";
      } else {
        std::cerr << "hades: simplex contact request from \"" << ev.display_name
                  << "\" ignored (auto_accept=false; accept it in the simplex-chat CLI)\n";
      }
      break;
    case SxEvent::Kind::Connected:
      std::cerr << "hades: simplex contact connected: " << ev.display_name << "\n";
      break;
    case SxEvent::Kind::None:
      break;
  }
}

bool SimplexModule::step_once() {
  try {
    SxEvent ev;
    switch (api_->next_event(poll_timeout_s_, ev)) {
      case SxStatus::Event:
        handle_event_(ev);
        return true;
      case SxStatus::Timeout:
        return true;
      case SxStatus::Closed:
      case SxStatus::Error:
        return false;
    }
  } catch (const std::exception& e) {
    std::cerr << "hades: simplex event error: " << e.what() << "\n";
  } catch (...) {
    std::cerr << "hades: simplex event error (unknown)\n";
  }
  return true;
}

void SimplexModule::run_loop_() {
  // Initial connect + reconnect-on-drop, with an interruptible backoff.
  bool need_connect = true;
  while (!stop_.load()) {
    if (need_connect) {
      if (!api_->reconnect()) {
        // Backoff base = connect_timeout_s (spec); interruptible so the dtor never waits it out.
        std::cerr << "hades: simplex daemon unreachable; retrying\n";
        std::unique_lock<std::mutex> lk(stop_mu_);
        stop_cv_.wait_for(lk, std::chrono::duration<double>(connect_timeout_s_),
                          [this] { return stop_.load(); });
        continue;
      }
      need_connect = false;
    }
    if (!step_once()) need_connect = true;
  }
}

void SimplexModule::start() {
  if (ev_thread_.joinable()) return;   // idempotent
  ev_thread_ = std::thread([this] { run_loop_(); });
}

void SimplexModule::wait() {
  if (ev_thread_.joinable()) ev_thread_.join();
}
}  // namespace hades
```

**Implementer notes:** `test SimplexModule` never calls `reconnect()` (only `run_loop_` does — tests drive `step_once`). Add `#include <iostream>` where needed. `split_message` is declared in `hades/telegram/parse.h` with a `limit` parameter. `make_ws_simplex_api` must be declared in `api.h` (Task 3's note).

- [ ] **Step 4: Build + test.** `-R SimplexModule` → pass; full suite green.
- [ ] **Step 5: Commit.**

```bash
git add include/hades/module/simplex_module.h src/apps/simplex/simplex.cpp tests/test_simplex_module.cpp CMakeLists.txt
git commit -m "feat: SimplexModule — allowlisted DM turns, text y/N confirms, notify sink, reconnect loop"
```

---

## Task 5: Wiring, hades_main, ship (manifests + docs) + TSan

**Files:**
- Modify: `app/agent_wiring.h`, `app/agent_wiring.cpp`, `app/hades_main.cpp`, `CMakeLists.txt`
- Test: `tests/test_simplex_wiring.cpp`
- Modify: `manifests/dev.hades` (backup/restore procedure!), `docs/manifest-reference.md`, `CLAUDE.md`

**Interfaces:**
- Consumes: `SimplexModule` (Task 4) — `set_turn_gate`, `set_turn_timeout_s`, `on_start`, `on_attach`, `start()`, `wait()`.
- Produces: `Agent::simplex` (declared between `telegram` and `heartbeat`); roster factory `"simplex"`; `Simplex` block extraction; `hades_main` starts the thread + a simplex-only `wait()` branch.

- [ ] **Step 1: Write the failing wiring tests** `tests/test_simplex_wiring.cpp`:

```cpp
// tests/test_simplex_wiring.cpp — manifest roster -> SimplexModule wired with gate + config
#include <gtest/gtest.h>
#include <memory>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/launcher.h"
using namespace hades;

TEST(SimplexWiring, RosterBuildsModuleAndRequiresAllowContacts) {
  {
    Blackboard bb;
    Manifest m = parse_manifest(
        "Session\n{\n  model = m\n}\nModule = arbiter\nModule = simplex\n"
        "Simplex\n{\n  allow_contacts = 2, Vaios K\n}\n");
    Agent agent = build_agent(bb, m);
    ASSERT_NE(agent.simplex, nullptr);
  }
  {
    Blackboard bb;
    Manifest m = parse_manifest(
        "Session\n{\n  model = m\n}\nModule = arbiter\nModule = simplex\n");
    EXPECT_THROW(build_agent(bb, m), MalConfig);   // no Simplex block -> no allow_contacts
  }
}

TEST(SimplexWiring, NoRosterLeavesAgentSimplexNull) {
  Blackboard bb;
  Manifest m = parse_manifest("Session\n{\n  model = m\n}\nModule = arbiter\n");
  Agent agent = build_agent(bb, m);
  EXPECT_EQ(agent.simplex, nullptr);
}
```

(The module never opens a socket here: `on_start` builds the api lazily but `reconnect()` only runs in `run_loop_`, and `start()` is never called by wiring — no thread, no connection, telegram-precedent.)

- [ ] **Step 2: CMake + run — expect FAIL (`Agent` has no member `simplex`).**

```cmake
target_sources(hades_tests PRIVATE tests/test_simplex_wiring.cpp)
```

- [ ] **Step 3: Implement wiring.**

1. `app/agent_wiring.h`: add `#include "hades/module/simplex_module.h"` with the module includes, and in `struct Agent` insert between `telegram` and `heartbeat`:

```cpp
  // SimpleX front-end. Declared AFTER telegram and BEFORE heartbeat: heartbeat is destroyed
  // first (a tick may deliver NOTIFY_USER via this module), then simplex joins its event
  // thread while the Executor + every module an in-flight simplex turn touches are still
  // alive. Do NOT reorder.
  std::unique_ptr<SimplexModule> simplex;
```

2. `app/agent_wiring.cpp`, `wire_agent` signature: add `const Block& simplex_cfg = Block{}` after `telegram_cfg`; wire it right after the telegram wiring block (line ~441):

```cpp
  if (a.simplex) {
    a.simplex->set_turn_gate(a.gate.get());
    a.simplex->on_start(simplex_cfg, bb);
    a.simplex->on_attach(bb);
  }
```

3. Manifest overload: register the factory with the others —

```cpp
  launcher.register_factory("simplex",     []{ return std::make_unique<SimplexModule>(); });
```

take it —

```cpp
  a.simplex = take_as<SimplexModule>(launcher, "simplex");
```

extract the block next to the Telegram one —

```cpp
  const auto sx_blocks = m.of("Simplex");
  const Block simplex_cfg = sx_blocks.empty() ? Block{} : sx_blocks.front();
```

pass it in the `wire_agent` call right after `telegram_cfg`, and mirror the timeout line —

```cpp
  if (a.simplex) a.simplex->set_turn_timeout_s(turn_idle_timeout_s);
```

4. `app/hades_main.cpp`: next to the telegram start (line ~164):

```cpp
    if (agent.simplex) agent.simplex->start();
```

and extend the wait-chain (after the telegram-only branch):

```cpp
    } else if (agent.simplex) {
      std::cerr << "hades: simplex-only roster — listening (Ctrl-C to exit)\n";
      agent.simplex->wait();                                  // blocks on the event thread
```

- [ ] **Step 4: Build + full suite + TSan.**

```bash
nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure
nix develop --command cmake --build build-tsan && nix develop --command ctest --test-dir build-tsan --output-on-failure
```

(If `build-tsan/` is not configured in this checkout: `nix develop --command cmake -S . -B build-tsan -G Ninja -DCMAKE_CXX_FLAGS="-fsanitize=thread" -DCMAKE_BUILD_TYPE=RelWithDebInfo` first.) All green.

- [ ] **Step 5: dev.hades — commented example block (backup/restore procedure).**

```bash
cp manifests/dev.hades /tmp/dev.hades.user
git checkout -- manifests/dev.hades
cat >> manifests/dev.hades <<'EOF'

# --- SimpleX front-end (uncomment; needs a local `simplex-chat -p 5225` daemon with the bot
# profile + /address created once in its CLI — see docs/manifest-reference.md §16). No tokens:
# the daemon's WS API is unauthenticated-by-design, loopback-only. allow_contacts is REQUIRED
# (comma-separated local contact ids and/or exact display names). auto_accept=true is an
# explicit opt-in: with an open address, a stranger can NAME THEMSELVES like an allowlisted
# display name — keep manual acceptance (default) or allowlist by numeric id.
# Module = simplex
# Simplex
# {
#   host           = 127.0.0.1
#   port           = 5225
#   allow_contacts = 2, Vaios K
#   auto_accept    = false
#   # notify_contact = 2   # NOTIFY_USER (heartbeat) delivery target
# }
EOF
git add manifests/dev.hades
cp /tmp/dev.hades.user manifests/dev.hades
cat >> manifests/dev.hades <<'EOF'

# --- SimpleX front-end (uncomment; needs a local `simplex-chat -p 5225` daemon with the bot
# profile + /address created once in its CLI — see docs/manifest-reference.md §16). No tokens:
# the daemon's WS API is unauthenticated-by-design, loopback-only. allow_contacts is REQUIRED
# (comma-separated local contact ids and/or exact display names). auto_accept=true is an
# explicit opt-in: with an open address, a stranger can NAME THEMSELVES like an allowlisted
# display name — keep manual acceptance (default) or allowlist by numeric id.
# Module = simplex
# Simplex
# {
#   host           = 127.0.0.1
#   port           = 5225
#   allow_contacts = 2, Vaios K
#   auto_accept    = false
#   # notify_contact = 2   # NOTIFY_USER (heartbeat) delivery target
# }
EOF
```

Verify: `git diff --cached manifests/dev.hades` shows ONLY the appended commented block; `git diff manifests/dev.hades` shows only the user's pre-existing live edits (plus the same appended block in their working copy). `manifests/pi.hades` untouched this feature.

- [ ] **Step 6: docs.**
  1. `docs/manifest-reference.md`: new **§16 Simplex block** — the 6-key table (defaults per Global Constraints), the roster line (`Module = simplex`), a setup walkthrough (install the official simplex-chat CLI — aarch64 builds exist for the Pi; first run creates the profile; `/address` once; run `simplex-chat -p 5225`; find a contact's id after they connect), the security notes (unauthenticated local WS — loopback only; allowlist REQUIRED; the auto_accept name-spoof caveat; confirm = text y/N; NOTIFY_USER via `notify_contact`; both telegram+simplex rostered → notifications delivered on both), and the §2 roster table + any cross-cutting tables gain their simplex rows.
  2. `CLAUDE.md`: add a `### SimpleX front-end (shipped 2026-07-11, feat/simplex)` subsection under Current state (pattern: fourth front-end, TurnGate, in-house WS client — codec+client+api+module pieces, text y/N confirms, allowlist ids/names + manual-accept default, notify_contact sink, reconnect loop, Pi = official aarch64 CLI binary as external runtime dep) + update: the front-ends list in `Current state`, the roster gotchas (a `Simplex` bullet in Gotchas mirroring the Telegram one), the test count (record the real post-suite number), and the `Agent` teardown-order note (telegram → simplex → heartbeat tail).
- [ ] **Step 7: Full suite once more, then commit.**

```bash
nix develop --command cmake --build build && nix develop --command ctest --test-dir build --output-on-failure
git add app/agent_wiring.h app/agent_wiring.cpp app/hades_main.cpp tests/test_simplex_wiring.cpp CMakeLists.txt docs/manifest-reference.md CLAUDE.md
git commit -m "feat: wire + ship simplex front-end — roster, gate, main start/wait, docs, dev.hades example"
git status   # manifests/dev.hades: user's live edits + the commented block remain unstaged-modified; memory/facts.md untouched
```

(The dev.hades staging happened in Step 5; this commit picks up the staged copy.)

---

## Verification (end-to-end)

1. Full suite in `nix develop`: 569 baseline + ~40 new, all green; TSan suite green (new event thread).
2. Live smoke (Vaios, desktop): install simplex-chat CLI, create profile + `/address`, run `simplex-chat -p 5225`; connect from the phone app; accept the contact in the CLI; find the contact id; uncomment the `Simplex` block with your id/name; run hades; message the bot → gated turn reply; trigger a confirm-band action → y/N text prompt → `y` approves; `Heartbeat` notify with `notify_contact` set → arrives in SimpleX.
3. Pi (later): official `simplex-chat-ubuntu-2x_04-aarch64` binary + the same flow; 512 MB RAM behavior = the live validation call.

## Execution

Subagent-driven development (house process): fresh implementer per task, per-task cpp-reviewer (opus for T2/T4 — socket + thread code), controller verification pass per task (Vaios's standing request), fix loop, final whole-branch review (opus), then finishing-a-development-branch (ff-merge via `git branch -f main HEAD`, never push — no remote).
