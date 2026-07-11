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
