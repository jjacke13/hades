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
