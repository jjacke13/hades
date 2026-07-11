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
