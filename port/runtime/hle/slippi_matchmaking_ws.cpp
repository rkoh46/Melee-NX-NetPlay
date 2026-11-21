// C++ port of the `matchmaking` crate of project-slippi/slippi-rust-extensions (GPL-2.0, commit 934f373):
// matchmaking/src/client.rs (the session), protocol.rs (the messages) and stun.rs.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_matchmaking_ws.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace slippi::mm {
namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using json = nlohmann::json;   // this nlohmann sorts keys; the service reads fields by name, so order is free

const char kProdUrl[] = "wss://matchmaking.slippi.gg/v1/queue";
const char kDevUrl[] = "wss://matchmaking-dev.slippi.gg/v1/queue";
const char kUrlEnvVar[] = "SLIPPI_MM_URL";   // a local service started with authentication disabled

constexpr int kConnectTimeoutMs = 5000;
constexpr int kHandshakeReadTimeoutMs = 10000;
constexpr int kPollReadTimeoutMs = 250;
constexpr int kConnectAttempts = 3;
// How long after losing a queued ticket's connection we keep trying to resume. The service drops a silent
// ticket after thirty seconds.
constexpr milliseconds kResumeWindow{15000};
constexpr milliseconds kRetryDelay{1000};
// How long a connection may go without the service acknowledging its join or resume.
constexpr milliseconds kJoinTimeout{5000};
// The service sends a status message every few seconds while a ticket is queued. After this long without any
// frame we send a ping, and after kLivenessTimeout the connection counts as lost: a network that drops
// silently would otherwise leave the search running while the service had long dropped the ticket.
constexpr milliseconds kPingAfter{5000};
constexpr milliseconds kLivenessTimeout{15000};
constexpr size_t kMaxMessage = 1 << 20;   // nothing the service sends is near this; a guard against garbage

std::atomic<void (*)(const char*)> g_log{nullptr};

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
void logf(const char* fmt, ...) {
  void (*sink)(const char*) = g_log.load();
  if (!sink) return;
  char line[256];
  int used = std::snprintf(line, sizeof line, "[Matchmaking] ");
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line + used, sizeof line - static_cast<size_t>(used), fmt, args);
  va_end(args);
  sink(line);
}

// ---- SHA-1 and base64, for Sec-WebSocket-Accept only ---------------------------------------------------

void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  std::vector<uint8_t> msg(data, data + len);
  msg.push_back(0x80);
  while (msg.size() % 64 != 56) msg.push_back(0);
  const uint64_t bits = static_cast<uint64_t>(len) * 8;
  for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>(bits >> (i * 8)));
  auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
  for (size_t block = 0; block < msg.size(); block += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      const uint8_t* p = &msg[block + static_cast<size_t>(i) * 4];
      w[i] = (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
    }
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
      const uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  for (int i = 0; i < 5; ++i)
    for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<uint8_t>(h[i] >> (24 - j * 8));
}

std::string base64(const uint8_t* data, size_t len) {
  static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < len; i += 3) {
    const uint32_t n = (uint32_t{data[i]} << 16) | (i + 1 < len ? uint32_t{data[i + 1]} << 8 : 0) |
                       (i + 2 < len ? uint32_t{data[i + 2]} : 0);
    out += table[(n >> 18) & 63];
    out += table[(n >> 12) & 63];
    out += i + 1 < len ? table[(n >> 6) & 63] : '=';
    out += i + 2 < len ? table[n & 63] : '=';
  }
  return out;
}

// Unpredictable enough that a stale or foreign reply is not taken for ours; not a secret.
uint64_t random64() {
  static std::atomic<uint64_t> counter{0};
  static const uint64_t seed = [] {
    uint64_t s = static_cast<uint64_t>(Clock::now().time_since_epoch().count());
    s ^= static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()) << 1;
    s ^= reinterpret_cast<uintptr_t>(&counter);
    return s;
  }();
  uint64_t z = seed + (counter.fetch_add(1) + 1) * 0x9E3779B97F4A7C15ull +
               static_cast<uint64_t>(Clock::now().time_since_epoch().count());
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

// ---- the byte stream under the WebSocket ---------------------------------------------------------------

struct Shared {
  std::atomic<int> state{static_cast<int>(State::Idle)};
  std::atomic<bool> cancel{false};
  std::atomic<bool> finished{false};
  std::mutex mutex;   // guards result_json, has_result and error
  std::string result_json;
  bool has_result = false;
  std::string error;

  void set_state(State s) { state.store(static_cast<int>(s)); }
  bool cancelled() const { return cancel.load(); }
  void fail(const std::string& message) {
    logf("%s", message.c_str());
    { std::lock_guard<std::mutex> lock(mutex); error = message; }
    set_state(State::Failed);
  }
};

class Transport {
 public:
  virtual ~Transport() = default;
  // Bytes read (> 0), 0 when nothing arrived within timeout_ms, -1 when the connection is gone.
  virtual int recv(uint8_t* out, size_t size, int timeout_ms) = 0;
  virtual bool send_all(const uint8_t* data, size_t size) = 0;
};

bool wait_fd(int fd, bool for_write, int timeout_ms) {
  struct pollfd p = {};
  p.fd = fd;
  p.events = for_write ? POLLOUT : POLLIN;
  return poll(&p, 1, timeout_ms) > 0;
}

// A plain socket, for ws:// URLs: a local service or the host test.
class PlainTransport : public Transport {
 public:
  ~PlainTransport() override { if (fd_ >= 0) ::close(fd_); }

  bool connect(const std::string& host, int port, const Shared& shared, std::string* error) {
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;   // netplay is IPv4, so the address the service observes must be the IPv4 one
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* list = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &list) != 0 || !list) {
      *error = "resolving the service's name failed";
      return false;
    }
    *error = "no IPv4 address for the service";
    for (struct addrinfo* a = list; a && fd_ < 0; a = a->ai_next) {
      if (shared.cancelled()) break;
      const int fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
      if (fd < 0) continue;
      fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
      int rc = ::connect(fd, a->ai_addr, a->ai_addrlen);
      if (rc != 0 && errno == EINPROGRESS && wait_fd(fd, true, kConnectTimeoutMs)) {
        int so_error = 0;
        socklen_t len = sizeof so_error;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
        rc = so_error == 0 ? 0 : -1;
      }
      if (rc != 0) { ::close(fd); *error = "the service did not accept the connection"; continue; }
      int one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
      fd_ = fd;
    }
    freeaddrinfo(list);
    return fd_ >= 0;
  }

  int recv(uint8_t* out, size_t size, int timeout_ms) override {
    if (!wait_fd(fd_, false, timeout_ms)) return 0;
    const ssize_t n = ::recv(fd_, out, size, 0);
    if (n > 0) return static_cast<int>(n);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    return -1;
  }

  bool send_all(const uint8_t* data, size_t size) override {
    size_t sent = 0;
    while (sent < size) {
#ifdef MSG_NOSIGNAL
      const ssize_t n = ::send(fd_, data + sent, size - sent, MSG_NOSIGNAL);
#else
      const ssize_t n = ::send(fd_, data + sent, size - sent, 0);
#endif
      if (n > 0) { sent += static_cast<size_t>(n); continue; }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) && wait_fd(fd_, true, kConnectTimeoutMs)) continue;
      return false;
    }
    return true;
  }

 private:
  int fd_ = -1;
};

void ensure_curl_global() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

int abort_when_cancelled(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  return static_cast<const Shared*>(user)->cancelled() ? 1 : 0;
}

// TLS through curl with CURLOPT_CONNECT_ONLY: curl resolves, connects, does the TLS handshake and checks the
// certificate against the system's store (the console's own on the Switch), then hands over the byte stream.
class CurlTransport : public Transport {
 public:
  ~CurlTransport() override { if (curl_) curl_easy_cleanup(curl_); }

  bool connect(const std::string& host, int port, const Shared& shared, std::string* error) {
    ensure_curl_global();
    curl_ = curl_easy_init();
    if (!curl_) { *error = "curl_easy_init failed"; return false; }
    const std::string url = "https://" + host + ":" + std::to_string(port) + "/";
    char message[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_CONNECT_ONLY, 1L);
    // The upgrade below is HTTP/1.1 text. Without this curl offers HTTP/2 in the TLS handshake, a server that
    // takes it then treats our bytes as an ordinary request, and the answer never reaches curl_easy_recv.
    curl_easy_setopt(curl_, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(kConnectTimeoutMs));
    curl_easy_setopt(curl_, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl_, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl_, CURLOPT_ERRORBUFFER, message);
    curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 0L);   // so a cancel ends a connect in progress
    curl_easy_setopt(curl_, CURLOPT_XFERINFOFUNCTION, abort_when_cancelled);
    curl_easy_setopt(curl_, CURLOPT_XFERINFODATA, const_cast<Shared*>(&shared));
    const CURLcode result = curl_easy_perform(curl_);
    curl_easy_setopt(curl_, CURLOPT_ERRORBUFFER, nullptr);
    if (result != CURLE_OK) {
      // curl's own text can name the address it tried; the code's meaning is enough for the log.
      *error = curl_easy_strerror(result);
      return false;
    }
    curl_socket_t fd = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(curl_, CURLINFO_ACTIVESOCKET, &fd) != CURLE_OK || fd == CURL_SOCKET_BAD) {
      *error = "curl gave no socket";
      return false;
    }
    fd_ = static_cast<int>(fd);
    return true;
  }

  int recv(uint8_t* out, size_t size, int timeout_ms) override {
    // Ask curl first: TLS may hold decrypted bytes the socket no longer shows as readable.
    for (int pass = 0; pass < 2; ++pass) {
      size_t got = 0;
      const CURLcode result = curl_easy_recv(curl_, out, size, &got);
      if (result == CURLE_OK) return got > 0 ? static_cast<int>(got) : -1;   // 0 bytes with OK: the peer closed
      if (result != CURLE_AGAIN) return -1;
      if (pass == 0 && !wait_fd(fd_, false, timeout_ms)) return 0;
    }
    return 0;
  }

  bool send_all(const uint8_t* data, size_t size) override {
    size_t sent = 0;
    while (sent < size) {
      size_t n = 0;
      const CURLcode result = curl_easy_send(curl_, data + sent, size - sent, &n);
      if (result == CURLE_OK) { sent += n; continue; }
      if (result == CURLE_AGAIN && wait_fd(fd_, true, kConnectTimeoutMs)) continue;
      return false;
    }
    return true;
  }

 private:
  CURL* curl_ = nullptr;
  int fd_ = -1;
};

// ---- RFC 6455 over a Transport --------------------------------------------------------------------------

struct Url {
  bool secure = true;
  std::string host;
  int port = 443;
  std::string path = "/";
};

bool parse_url(const std::string& url, Url* out) {
  size_t rest;
  if (url.compare(0, 6, "wss://") == 0) { out->secure = true; rest = 6; }
  else if (url.compare(0, 5, "ws://") == 0) { out->secure = false; rest = 5; }
  else return false;
  const size_t slash = url.find('/', rest);
  std::string authority = url.substr(rest, slash == std::string::npos ? std::string::npos : slash - rest);
  out->path = slash == std::string::npos ? "/" : url.substr(slash);
  out->port = out->secure ? 443 : 80;
  const size_t colon = authority.rfind(':');
  if (colon != std::string::npos) {
    const int port = std::atoi(authority.c_str() + colon + 1);
    if (port <= 0 || port > 65535) return false;
    out->port = port;
    authority.resize(colon);
  }
  out->host = authority;
  return !out->host.empty();
}

enum class Frame { Text, Close, Other, Timeout, Error };

class WebSocket {
 public:
  explicit WebSocket(std::unique_ptr<Transport> transport) : transport_(std::move(transport)) {}

  // The HTTP/1.1 Upgrade exchange. The reply must be 101 with the Sec-WebSocket-Accept that belongs to our key.
  bool handshake(const Url& url, std::string* error) {
    uint8_t nonce[16];
    for (int i = 0; i < 2; ++i) {
      const uint64_t r = random64();
      std::memcpy(nonce + i * 8, &r, 8);
    }
    const std::string key = base64(nonce, sizeof nonce);
    const bool default_port = url.port == (url.secure ? 443 : 80);
    const std::string request = "GET " + url.path + " HTTP/1.1\r\nHost: " + url.host +
                                (default_port ? "" : ":" + std::to_string(url.port)) +
                                "\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
                                "Sec-WebSocket-Key: " + key + "\r\n\r\n";
    if (!transport_->send_all(reinterpret_cast<const uint8_t*>(request.data()), request.size())) {
      *error = "sending the upgrade request failed";
      return false;
    }
    const Clock::time_point deadline = Clock::now() + milliseconds(kHandshakeReadTimeoutMs);
    std::string head;
    size_t end = std::string::npos;
    while ((end = head.find("\r\n\r\n")) == std::string::npos) {
      if (Clock::now() >= deadline || head.size() > 16384) { *error = "no answer to the upgrade request"; return false; }
      uint8_t chunk[2048];
      const int n = transport_->recv(chunk, sizeof chunk, kPollReadTimeoutMs);
      if (n < 0) { *error = "connection closed during the upgrade"; return false; }
      head.append(reinterpret_cast<const char*>(chunk), static_cast<size_t>(n));
    }
    buffer_.assign(head.begin() + static_cast<std::ptrdiff_t>(end) + 4, head.end());   // frames that came with it
    head.resize(end);
    if (head.compare(0, 12, "HTTP/1.1 101") != 0) {
      *error = "the service refused the upgrade (" + head.substr(0, std::min<size_t>(head.find("\r\n"), 40)) + ")";
      return false;
    }
    if (header_value(head, "sec-websocket-accept") != detail::websocket_accept(key)) {
      *error = "the service's upgrade answer does not match our key";
      return false;
    }
    return true;
  }

  bool send_text(const std::string& text) { return send_frame(0x1, reinterpret_cast<const uint8_t*>(text.data()), text.size()); }

  // Says goodbye. The connection is dropped right after, as tungstenite's close-and-drop does upstream.
  void close() {
    if (!closed_) { closed_ = true; const uint8_t code[2] = {0x03, 0xE8}; send_frame(0x8, code, 2); }
  }

  // One message. Pings are answered here; a text message split over several frames is put together.
  Frame read(std::string* text, int timeout_ms) {
    const Clock::time_point deadline = Clock::now() + milliseconds(timeout_ms);
    for (;;) {
      Frame got;
      if (take_frame(text, &got)) return got;
      const auto left = std::chrono::duration_cast<milliseconds>(deadline - Clock::now()).count();
      if (left <= 0) return Frame::Timeout;
      uint8_t chunk[4096];
      const int n = transport_->recv(chunk, sizeof chunk, static_cast<int>(left));
      if (n < 0) return Frame::Error;
      buffer_.insert(buffer_.end(), chunk, chunk + n);
    }
  }

 private:
  static std::string header_value(const std::string& head, const char* lower_name) {
    size_t pos = 0;
    while ((pos = head.find("\r\n", pos)) != std::string::npos) {
      pos += 2;
      const size_t colon = head.find(':', pos), line_end = std::min(head.find("\r\n", pos), head.size());
      if (colon == std::string::npos || colon > line_end) continue;
      std::string name = head.substr(pos, colon - pos);
      for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (name != lower_name) continue;
      size_t a = colon + 1, b = line_end;
      while (a < b && (head[a] == ' ' || head[a] == '\t')) ++a;
      while (b > a && (head[b - 1] == ' ' || head[b - 1] == '\t')) --b;
      return head.substr(a, b - a);
    }
    return "";
  }

  // A client's frames are always masked (RFC 6455 section 5.3).
  bool send_frame(uint8_t opcode, const uint8_t* payload, size_t size) {
    std::vector<uint8_t> frame;
    frame.reserve(size + 14);
    frame.push_back(static_cast<uint8_t>(0x80 | opcode));
    if (size < 126) frame.push_back(static_cast<uint8_t>(0x80 | size));
    else if (size <= 0xFFFF) {
      frame.push_back(0x80 | 126);
      frame.push_back(static_cast<uint8_t>(size >> 8));
      frame.push_back(static_cast<uint8_t>(size));
    } else {
      frame.push_back(0x80 | 127);
      for (int i = 7; i >= 0; --i) frame.push_back(static_cast<uint8_t>(static_cast<uint64_t>(size) >> (i * 8)));
    }
    const uint64_t r = random64();
    uint8_t mask[4];
    std::memcpy(mask, &r, 4);
    frame.insert(frame.end(), mask, mask + 4);
    for (size_t i = 0; i < size; ++i) frame.push_back(payload[i] ^ mask[i & 3]);
    return transport_->send_all(frame.data(), frame.size());
  }

  // Takes one whole frame off the buffer if there is one. True when it produced something to report.
  bool take_frame(std::string* text, Frame* out) {
    for (;;) {
      if (buffer_.size() < 2) return false;
      const uint8_t b0 = buffer_[0], b1 = buffer_[1];
      const bool fin = b0 & 0x80, masked = b1 & 0x80;
      const uint8_t opcode = b0 & 0x0F;
      uint64_t length = b1 & 0x7F;
      size_t header = 2;
      if (length == 126) {
        if (buffer_.size() < 4) return false;
        length = (uint64_t{buffer_[2]} << 8) | buffer_[3];
        header = 4;
      } else if (length == 127) {
        if (buffer_.size() < 10) return false;
        length = 0;
        for (int i = 0; i < 8; ++i) length = (length << 8) | buffer_[2 + i];
        header = 10;
      }
      if ((b0 & 0x70) || length > kMaxMessage) { *out = Frame::Error; return true; }   // no extension was agreed
      uint8_t mask[4] = {};
      if (masked) {
        if (buffer_.size() < header + 4) return false;
        std::memcpy(mask, &buffer_[header], 4);
        header += 4;
      }
      if (buffer_.size() < header + length) return false;
      std::vector<uint8_t> payload(buffer_.begin() + static_cast<std::ptrdiff_t>(header),
                                   buffer_.begin() + static_cast<std::ptrdiff_t>(header + length));
      buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(header + length));
      if (masked) for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i & 3];

      if (opcode == 0x8) {   // close: answer it once
        close();
        *out = Frame::Close;
        return true;
      }
      if (opcode == 0x9) {   // ping: answer with the same payload
        send_frame(0xA, payload.data(), payload.size());
        *out = Frame::Other;
        return true;
      }
      if (opcode == 0xA) { *out = Frame::Other; return true; }
      if (opcode == 0x1 || opcode == 0x2) {
        partial_.assign(payload.begin(), payload.end());
        partial_is_text_ = opcode == 0x1;
        in_message_ = !fin;
      } else if (opcode == 0x0 && in_message_) {
        if (partial_.size() + payload.size() > kMaxMessage) { *out = Frame::Error; return true; }
        partial_.insert(partial_.end(), payload.begin(), payload.end());
        in_message_ = !fin;
      } else {
        *out = Frame::Error;
        return true;
      }
      if (in_message_) continue;   // the rest of this message may already be in the buffer
      if (partial_is_text_) { text->assign(partial_.begin(), partial_.end()); *out = Frame::Text; }
      else *out = Frame::Other;
      partial_.clear();
      return true;
    }
  }

  std::unique_ptr<Transport> transport_;
  std::vector<uint8_t> buffer_;    // bytes received and not yet a whole frame
  std::vector<uint8_t> partial_;   // a message whose final frame has not come
  bool partial_is_text_ = false;
  bool in_message_ = false;
  bool closed_ = false;
};

enum class Connect { Ok, Cancelled, Failed };

Connect connect(const std::string& url_text, const Shared& shared, std::unique_ptr<WebSocket>* out, std::string* error) {
  Url url;
  if (!parse_url(url_text, &url)) { *error = "the matchmaking URL is not a ws:// or wss:// URL"; return Connect::Failed; }
  if (shared.cancelled()) return Connect::Cancelled;
  std::unique_ptr<Transport> transport;
  bool connected;
  if (url.secure) {
    auto t = std::make_unique<CurlTransport>();
    connected = t->connect(url.host, url.port, shared, error);
    transport = std::move(t);
  } else {
    auto t = std::make_unique<PlainTransport>();
    connected = t->connect(url.host, url.port, shared, error);
    transport = std::move(t);
  }
  if (shared.cancelled()) return Connect::Cancelled;
  if (!connected) return Connect::Failed;
  auto socket = std::make_unique<WebSocket>(std::move(transport));
  if (!socket->handshake(url, error)) return shared.cancelled() ? Connect::Cancelled : Connect::Failed;
  *out = std::move(socket);
  return Connect::Ok;
}

// ---- the session ----------------------------------------------------------------------------------------

std::mutex g_latest_mutex;
std::string g_latest_version;

// A string field of a JSON object, or "" when it is missing or not a string. Never throws.
std::string text_field(const json& object, const char* key) {
  const auto it = object.find(key);
  return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string dump(const json& value) { return value.dump(-1, ' ', false, json::error_handler_t::replace); }

enum class Ended { Done, Lost, JoinTimeout };

struct Session {
  std::shared_ptr<Shared> shared;
  std::string url;
  std::string join;                 // the join message, built when the session started
  bool has_ticket = false;          // ticket id and token once the service has accepted us
  std::string ticket_id, token;
  bool resuming = false;            // a connection dropped with a ticket in hand
  Clock::time_point resume_deadline;
  bool acknowledged = false;        // the current connection's join or resume was acknowledged

  // Sleeps in small steps so a cancel is noticed. True if cancelled.
  bool sleep_unless_cancelled(milliseconds total) {
    const Clock::time_point start = Clock::now();
    while (Clock::now() - start < total) {
      if (shared->cancelled()) { shared->set_state(State::Idle); return true; }
      std::this_thread::sleep_for(milliseconds(50));
    }
    return false;
  }

  // Whether a failed connect ends the session: past the resume window when resuming, after a few attempts
  // when first joining. Fails the session when it does.
  bool give_up(int connect_failures) {
    const bool stop = resuming ? Clock::now() >= resume_deadline : connect_failures >= kConnectAttempts;
    if (stop) shared->fail(has_ticket ? "Lost connection to the mm server" : "Failed to connect to mm server");
    return stop;
  }

  void run() {
    int connect_failures = 0;
    for (;;) {
      if (shared->cancelled()) { shared->set_state(State::Idle); return; }

      std::unique_ptr<WebSocket> socket;
      std::string error;
      const Connect connected = connect(url, *shared, &socket, &error);
      if (connected == Connect::Cancelled) { shared->set_state(State::Idle); return; }
      if (connected == Connect::Failed) {
        logf("Connect failed: %s", error.c_str());
        ++connect_failures;
        if (give_up(connect_failures) || sleep_unless_cancelled(kRetryDelay)) return;
        continue;
      }
      connect_failures = 0;

      const std::string first = has_ticket ? dump(json{{"type", "resume"}, {"ticketId", ticket_id}, {"token", token}}) : join;
      if (!socket->send_text(first)) {
        logf("Failed to send first message");
        socket->close();
        ++connect_failures;
        if (give_up(connect_failures) || sleep_unless_cancelled(kRetryDelay)) return;
        continue;
      }

      const Ended ended = serve(*socket);
      if (ended == Ended::Done) return;
      if (ended == Ended::JoinTimeout) { shared->fail("Failed to join mm queue"); return; }
      // Lost.
      if (!has_ticket) { shared->fail("Lost connection to the mm server"); return; }
      if (!resuming) {
        resuming = true;
        resume_deadline = Clock::now() + kResumeWindow;
      } else if (Clock::now() >= resume_deadline) {
        shared->fail("Lost connection to the mm server");
        return;
      }
      logf("Connection lost, resuming ticket");
      if (sleep_unless_cancelled(kRetryDelay)) return;
    }
  }

  // Runs one connection until the session ends or the connection drops.
  Ended serve(WebSocket& socket) {
    const Clock::time_point opened = Clock::now();
    Clock::time_point last_frame = opened;
    bool pinged = false;
    acknowledged = false;

    for (;;) {
      if (shared->cancelled()) {
        socket.send_text(R"({"type":"cancel"})");
        socket.close();
        shared->set_state(State::Idle);
        return Ended::Done;
      }

      std::string text;
      switch (socket.read(&text, kPollReadTimeoutMs)) {
        case Frame::Text: {
          last_frame = Clock::now();
          pinged = false;
          Ended ended;
          if (handle(text, socket, &ended)) return ended;
          break;
        }
        case Frame::Close:
          logf("Server closed connection");
          return Ended::Lost;
        case Frame::Other:   // binary, ping and pong frames carry nothing for us but prove the service is there
          last_frame = Clock::now();
          pinged = false;
          break;
        case Frame::Timeout:
          break;
        case Frame::Error:
          logf("Read failed");
          return Ended::Lost;
      }

      if (!acknowledged && Clock::now() - opened >= kJoinTimeout) {
        logf("Service did not acknowledge the connection in time");
        socket.close();
        return has_ticket ? Ended::Lost : Ended::JoinTimeout;
      }

      const auto idle = Clock::now() - last_frame;
      if (idle >= kLivenessTimeout) {
        logf("Nothing from the service for %d s, connection lost",
             static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(idle).count()));
        socket.close();
        return Ended::Lost;
      }
      if (idle >= kPingAfter && !pinged) {
        pinged = true;
        socket.send_text(R"({"type":"ping"})");
      }
    }
  }

  // Handles one server message. True when the connection is done, with why in *ended.
  bool handle(const std::string& text, WebSocket& socket, Ended* ended) {
    const json message = json::parse(text, nullptr, false);
    if (message.is_discarded() || !message.is_object() || !message.count("type") || !message["type"].is_string()) {
      logf("Ignoring malformed message");
      return false;
    }
    const std::string kind = message["type"].get<std::string>();

    if (kind == "joined") {
      const auto id = message.find("ticketId"), tok = message.find("token");
      if (id != message.end() && id->is_string() && tok != message.end() && tok->is_string()) {
        logf("Ticket accepted");
        has_ticket = true;
        ticket_id = id->get<std::string>();
        token = tok->get<std::string>();
        acknowledged = true;
        resuming = false;   // the service took us back, so the next drop gets a full resume window of its own
        shared->set_state(State::Queued);
      }
      return false;
    }
    if (kind == "status") return false;
    if (kind == "match") {
      const auto id = message.find("matchId");
      if (id != message.end() && id->is_string()) {
        const auto host = message.find("isHost");
        logf("Match found: %s (host: %s)", id->get<std::string>().c_str(),
             host != message.end() && host->is_boolean() && host->get<bool>() ? "true" : "false");
      } else {
        logf("Match message unreadable");
      }
      {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->result_json = text;
        shared->has_result = true;
      }
      shared->set_state(State::Matched);
      socket.close();
      *ended = Ended::Done;
      return true;
    }
    if (kind == "error") {
      std::string reason = "Received an error from the mm server";
      const auto err = message.find("error");
      if (err != message.end() && err->is_string()) {
        reason = err->get<std::string>();
        // The service knows a newer build exists. Remember it so the player can be told which.
        const std::string latest = text_field(message, "latestVersion");
        if (!latest.empty()) {
          std::lock_guard<std::mutex> lock(g_latest_mutex);
          g_latest_version = latest;
        }
      }
      shared->fail(reason);
      socket.close();
      *ended = Ended::Done;
      return true;
    }
    return false;
  }
};

// One session runs at a time. Workers are not joined on cancel: one stuck in a connect or an upgrade would
// hold the caller up for seconds, and it touches nothing but its own shared state once flagged. They are
// kept until they finish so shutdown() can wait for them.
struct Worker {
  std::thread thread;
  std::shared_ptr<Shared> shared;
};

std::mutex g_mutex;
std::shared_ptr<Shared> g_current = std::make_shared<Shared>();
std::vector<Worker> g_workers;
uint64_t g_session_id = 0;

void cancel_current_locked() { g_current->cancel.store(true); }

void reap_finished_locked() {
  for (size_t i = 0; i < g_workers.size();) {
    if (g_workers[i].shared->finished.load()) {
      g_workers[i].thread.join();   // it has left run(); this returns at once
      g_workers.erase(g_workers.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
}

std::string resolve_url(const Request& request) {
  if (!request.url.empty()) return request.url;
  const char* env = std::getenv(kUrlEnvVar);
  if (env && *env) return env;
  return request.app_version.find("dev") != std::string::npos ? kDevUrl : kProdUrl;
}

}  // namespace

namespace detail {

const char* classify_nat(const StunObservation& a, const StunObservation& b) {
  if (!a.answered || !b.answered) return "unknown";
  return std::memcmp(a.ip, b.ip, 4) == 0 && a.port == b.port ? "cone" : "symmetric";
}

std::string build_join_json(const Request& request) {
  // The NAT is classified from what two servers saw. The addresses themselves stay out of the log.
  const char* nat_type = classify_nat(request.stun[0], request.stun[1]);

  json candidates = json::array();
  uint16_t port = request.netplay_port;
  for (const StunObservation& seen : request.stun) {
    if (!seen.answered) continue;
    char ip[16];
    std::snprintf(ip, sizeof ip, "%u.%u.%u.%u", seen.ip[0], seen.ip[1], seen.ip[2], seen.ip[3]);
    candidates.push_back({{"ip", ip}, {"port", seen.port}, {"type", "srflx"}});
    port = seen.port;   // opponents send to the STUN-mapped port when it is known
    break;              // the first answer only
  }
  const size_t colon = request.lan_addr.rfind(':');
  if (colon != std::string::npos) {
    const std::string digits = request.lan_addr.substr(colon + 1);
    char* end = nullptr;
    const unsigned long lan_port = std::strtoul(digits.c_str(), &end, 10);
    if (!digits.empty() && end && *end == '\0' && digits.find_first_not_of("0123456789") == std::string::npos && lan_port <= 65535)
      candidates.push_back({{"ip", request.lan_addr.substr(0, colon)}, {"port", lan_port}, {"type", "host"}});
  }

  json code = json::array();
  for (uint8_t byte : request.connect_code) code.push_back(byte);   // raw Shift-JIS bytes; the service decodes them

  json join;
  join["type"] = "join";
  join["user"] = {{"uid", request.uid}, {"playKey", request.play_key}, {"connectCode", request.user_connect_code},
                  {"displayName", request.display_name}};
  join["search"] = {{"mode", request.mode}, {"connectCode", code}};
  join["appVersion"] = request.app_version;
  join["port"] = port;
  join["ipAddressLan"] = request.lan_addr;
  if (!candidates.empty()) join["candidates"] = candidates;
  join["natType"] = nat_type;
  return dump(join);
}

std::string websocket_accept(const std::string& key) {
  const std::string input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t digest[20];
  sha1(reinterpret_cast<const uint8_t*>(input.data()), input.size(), digest);
  return base64(digest, sizeof digest);
}

}  // namespace detail

void set_log(void (*log)(const char* line)) { g_log.store(log); }

uint64_t start(const Request& request) {
  Session session;
  session.url = resolve_url(request);
  session.join = detail::build_join_json(request);
  logf("NAT type: %s", detail::classify_nat(request.stun[0], request.stun[1]));

  std::lock_guard<std::mutex> lock(g_mutex);
  cancel_current_locked();
  reap_finished_locked();

  ++g_session_id;
  auto shared = std::make_shared<Shared>();
  shared->set_state(State::Connecting);
  g_current = shared;
  session.shared = shared;
  logf("Starting session");

  Worker worker;
  worker.shared = shared;
  worker.thread = std::thread([session = std::move(session)]() mutable {
    try {
      session.run();
    } catch (...) {
      session.shared->fail("Matchmaking stopped unexpectedly");
    }
    session.shared->finished.store(true);
  });
  g_workers.push_back(std::move(worker));
  return g_session_id;
}

State state() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return static_cast<State>(g_current->state.load());
}

std::string error_message() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::lock_guard<std::mutex> inner(g_current->mutex);
  return g_current->error;
}

std::string latest_version() {
  std::lock_guard<std::mutex> lock(g_latest_mutex);
  return g_latest_version;
}

bool take_result_json(std::string* out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::lock_guard<std::mutex> inner(g_current->mutex);
  if (!g_current->has_result) return false;
  if (out) *out = std::move(g_current->result_json);
  g_current->result_json.clear();
  g_current->has_result = false;
  return true;
}

void cancel(uint64_t session_id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (session_id == g_session_id) cancel_current_locked();
}

void shutdown() {
  std::vector<Worker> workers;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    cancel_current_locked();
    for (Worker& w : g_workers) w.shared->cancel.store(true);
    workers.swap(g_workers);
  }
  for (Worker& w : workers) w.thread.join();   // bounded: every wait in a worker is, and all were told to stop
}

// ---- STUN (RFC 5389): just enough to learn the public address of a UDP socket ---------------------------

namespace {
constexpr uint32_t kMagicCookie = 0x2112A442u;
constexpr uint16_t kBindingRequest = 0x0001, kBindingSuccess = 0x0101;
constexpr uint16_t kAttrMappedAddress = 0x0001, kAttrXorMappedAddress = 0x0020;
constexpr uint8_t kFamilyIpv4 = 0x01;

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

bool decode_address(const uint8_t* value, size_t len, bool xored, StunObservation* out) {
  if (len < 8 || value[1] != kFamilyIpv4) return false;
  uint16_t port = be16(value + 2);
  uint32_t ip = (uint32_t{value[4]} << 24) | (uint32_t{value[5]} << 16) | (uint32_t{value[6]} << 8) | value[7];
  if (xored) {
    port ^= static_cast<uint16_t>(kMagicCookie >> 16);
    ip ^= kMagicCookie;
  }
  out->answered = true;
  out->ip[0] = static_cast<uint8_t>(ip >> 24);
  out->ip[1] = static_cast<uint8_t>(ip >> 16);
  out->ip[2] = static_cast<uint8_t>(ip >> 8);
  out->ip[3] = static_cast<uint8_t>(ip);
  out->port = port;
  return true;
}
}  // namespace

void stun_build_request(uint8_t out[kStunRequestLen]) {
  out[0] = kBindingRequest >> 8;
  out[1] = kBindingRequest & 0xFF;
  out[2] = out[3] = 0;   // no attributes
  out[4] = 0x21; out[5] = 0x12; out[6] = 0xA4; out[7] = 0x42;
  const uint64_t a = random64(), b = random64();
  for (int i = 0; i < 8; ++i) out[8 + i] = static_cast<uint8_t>(a >> (56 - i * 8));
  for (int i = 0; i < 4; ++i) out[16 + i] = static_cast<uint8_t>(b >> (56 - i * 8));
}

bool stun_parse_response(const uint8_t* request, const uint8_t* response, size_t response_len, StunObservation* out) {
  if (!request || !response || !out || response_len < kStunRequestLen) return false;
  if (be16(response) != kBindingSuccess) return false;
  const size_t length = be16(response + 2);
  const uint8_t cookie[4] = {0x21, 0x12, 0xA4, 0x42};
  if (std::memcmp(response + 4, cookie, 4) != 0 || std::memcmp(response + 8, request + 8, 12) != 0) return false;
  const uint8_t* body = response + 20;
  const size_t body_len = std::min(response_len, 20 + length) - 20;

  StunObservation fallback, found;
  size_t offset = 0;
  while (offset + 4 <= body_len) {
    const uint16_t attr_type = be16(body + offset);
    const size_t attr_len = be16(body + offset + 2);
    const size_t value_start = offset + 4, value_end = value_start + attr_len;
    if (value_end > body_len) break;
    if (attr_type == kAttrXorMappedAddress) {
      if (decode_address(body + value_start, attr_len, true, &found)) { *out = found; return true; }
    } else if (attr_type == kAttrMappedAddress) {
      if (!fallback.answered) decode_address(body + value_start, attr_len, false, &fallback);
    }
    offset = value_end + ((4 - (attr_len % 4)) % 4);   // attributes are padded to four-byte boundaries
  }
  if (!fallback.answered) return false;
  *out = fallback;
  return true;
}

}  // namespace slippi::mm
