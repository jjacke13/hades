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
