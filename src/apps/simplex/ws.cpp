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
    // MSG_NOSIGNAL: a daemon that dropped mid-send must fail the write (EPIPE), not raise
    // SIGPIPE — this module must not depend on the tool runner's process-wide SIGPIPE ignore.
    const ssize_t n = ::send(fd_, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
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
