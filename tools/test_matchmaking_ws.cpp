// Host test for port/runtime/hle/slippi_matchmaking_ws.cpp against a WebSocket service on the loopback
// interface that plays the matchmaking service's part. Nothing here talks to Slippi.
// SPDX-License-Identifier: GPL-2.0-or-later
//
//   c++ -std=c++20 -Iport/runtime/hle -Iport/third_party tools/test_matchmaking_ws.cpp \
//       port/runtime/hle/slippi_matchmaking_ws.cpp -lcurl -o /tmp/test_matchmaking_ws && /tmp/test_matchmaking_ws
#include "slippi_matchmaking_ws.h"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace slippi;
using json = nlohmann::json;
using std::chrono::milliseconds;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ---- the service's side -------------------------------------------------------------------------------

struct Server {
  int listener = -1;
  int port = 0;
  int conn = -1;
  std::vector<uint8_t> pending;

  Server() {
    listener = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    listen(listener, 4);
    socklen_t len = sizeof addr;
    getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
    port = ntohs(addr.sin_port);
  }
  ~Server() { drop(); if (listener >= 0) close(listener); }

  std::string url() const { return "ws://127.0.0.1:" + std::to_string(port) + "/v1/queue"; }
  void drop() { if (conn >= 0) { close(conn); conn = -1; } pending.clear(); }

  bool fill(int timeout_ms) {
    pollfd p = {conn, POLLIN, 0};
    if (poll(&p, 1, timeout_ms) <= 0) return false;
    uint8_t chunk[4096];
    const ssize_t n = recv(conn, chunk, sizeof chunk, 0);
    if (n <= 0) return false;
    pending.insert(pending.end(), chunk, chunk + n);
    return true;
  }

  // Accepts a connection and answers its upgrade request.
  bool accept_upgrade(int timeout_ms = 3000) {
    drop();
    pollfd p = {listener, POLLIN, 0};
    if (poll(&p, 1, timeout_ms) <= 0) return false;
    conn = accept(listener, nullptr, nullptr);
    if (conn < 0) return false;
    std::string head;
    while (head.find("\r\n\r\n") == std::string::npos) {
      if (!fill(2000)) return false;
      head.assign(pending.begin(), pending.end());
    }
    pending.clear();
    const char marker[] = "Sec-WebSocket-Key: ";
    const size_t at = head.find(marker);
    if (at == std::string::npos || head.find("GET /v1/queue HTTP/1.1") != 0) return false;
    const std::string key = head.substr(at + sizeof marker - 1, head.find("\r\n", at) - at - (sizeof marker - 1));
    const std::string reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              "Sec-WebSocket-Accept: " + mm::detail::websocket_accept(key) + "\r\n\r\n";
    return send(conn, reply.data(), reply.size(), 0) == static_cast<ssize_t>(reply.size());
  }

  void send_frame(uint8_t first_byte, const std::string& payload) {
    std::vector<uint8_t> frame = {first_byte};
    if (payload.size() < 126) frame.push_back(static_cast<uint8_t>(payload.size()));
    else { frame.push_back(126); frame.push_back(static_cast<uint8_t>(payload.size() >> 8)); frame.push_back(static_cast<uint8_t>(payload.size())); }
    frame.insert(frame.end(), payload.begin(), payload.end());
    send(conn, frame.data(), frame.size(), 0);
  }
  void send_text(const std::string& text) { send_frame(0x81, text); }

  // One frame from the client: its opcode, or -1 when none came. Client frames must be masked.
  int read_frame(std::string* payload, int timeout_ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + milliseconds(timeout_ms);
    for (;;) {
      if (pending.size() >= 6) {
        size_t length = pending[1] & 0x7F, header = 2;
        const bool masked = pending[1] & 0x80;
        if (length == 126 && pending.size() >= 4) { length = (size_t{pending[2]} << 8) | pending[3]; header = 4; }
        if (masked && pending.size() >= header + 4 + length) {
          const uint8_t* mask = &pending[header];
          payload->clear();
          for (size_t i = 0; i < length; ++i) payload->push_back(static_cast<char>(pending[header + 4 + i] ^ mask[i & 3]));
          const int opcode = pending[0] & 0x0F;
          pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(header + 4 + length));
          return opcode;
        }
        if (!masked) return -2;
      }
      const auto left = std::chrono::duration_cast<milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (left <= 0 || !fill(static_cast<int>(left))) return -1;
    }
  }
};

static bool wait_state(mm::State want, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (mm::state() == want) return true;
    std::this_thread::sleep_for(milliseconds(20));
  }
  return mm::state() == want;
}

static mm::Request request_for(const Server& server) {
  mm::Request r;
  r.mode = 1;
  r.netplay_port = 41000;
  r.lan_addr = "10.0.0.5:41000";
  r.stun[0] = {true, {203, 0, 113, 7}, 51000};
  r.stun[1] = {true, {203, 0, 113, 7}, 51000};
  r.uid = "uid-test";
  r.play_key = "key-test";
  r.user_connect_code = "TEST#123";
  r.display_name = "Tester";
  r.app_version = "3.7.0";
  r.url = server.url();
  return r;
}

// ---- tests ----------------------------------------------------------------------------------------------

static void test_helpers() {
  // RFC 6455 section 1.3's example.
  CHECK(mm::detail::websocket_accept("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

  mm::Request r;
  r.mode = 2;
  r.connect_code = {0x82, 0x60};
  r.netplay_port = 41000;
  r.lan_addr = "192.168.1.5:41000";
  r.stun[0] = {true, {1, 2, 3, 4}, 41555};
  r.stun[1] = {true, {1, 2, 3, 4}, 41555};
  r.uid = "u"; r.play_key = "k"; r.user_connect_code = "ME#1"; r.display_name = "Me";
  r.app_version = "3.7.0";
  json join = json::parse(mm::detail::build_join_json(r));
  CHECK(join["type"] == "join");
  CHECK(join["user"]["uid"] == "u" && join["user"]["playKey"] == "k");
  CHECK(join["user"]["connectCode"] == "ME#1" && join["user"]["displayName"] == "Me");
  CHECK(join["search"]["mode"] == 2 && join["search"]["connectCode"] == json::array({130, 96}));
  CHECK(join["appVersion"] == "3.7.0");
  CHECK(join["port"] == 41555);   // the STUN-mapped port, not the local one
  CHECK(join["ipAddressLan"] == "192.168.1.5:41000");
  CHECK(join["candidates"].size() == 2);
  CHECK(join["candidates"][0] == json({{"ip", "1.2.3.4"}, {"port", 41555}, {"type", "srflx"}}));
  CHECK(join["candidates"][1] == json({{"ip", "192.168.1.5"}, {"port", 41000}, {"type", "host"}}));
  CHECK(join["natType"] == "cone");
  CHECK(!join.count("debug"));

  mm::Request bare;
  bare.mode = 1;
  bare.netplay_port = 41000;
  bare.display_name = "bad \xff utf8";   // must not throw
  json plain = json::parse(mm::detail::build_join_json(bare));
  CHECK(!plain.count("candidates"));
  CHECK(plain["natType"] == "unknown" && plain["port"] == 41000);

  mm::StunObservation a{true, {1, 1, 1, 1}, 40000}, b{true, {1, 1, 1, 1}, 40001}, none;
  CHECK(std::string(mm::detail::classify_nat(a, a)) == "cone");
  CHECK(std::string(mm::detail::classify_nat(a, b)) == "symmetric");
  CHECK(std::string(mm::detail::classify_nat(a, none)) == "unknown");
}

static std::vector<uint8_t> stun_reply(const uint8_t* request, const std::vector<uint8_t>& attrs, uint16_t type = 0x0101) {
  std::vector<uint8_t> out = {static_cast<uint8_t>(type >> 8), static_cast<uint8_t>(type), static_cast<uint8_t>(attrs.size() >> 8),
                              static_cast<uint8_t>(attrs.size()), 0x21, 0x12, 0xA4, 0x42};
  out.insert(out.end(), request + 8, request + 20);
  out.insert(out.end(), attrs.begin(), attrs.end());
  return out;
}
static std::vector<uint8_t> xor_mapped(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
  const uint16_t p = port ^ 0x2112;
  return {0x00, 0x20, 0x00, 0x08, 0x00, 0x01, static_cast<uint8_t>(p >> 8), static_cast<uint8_t>(p),
          static_cast<uint8_t>(a ^ 0x21), static_cast<uint8_t>(b ^ 0x12), static_cast<uint8_t>(c ^ 0xA4), static_cast<uint8_t>(d ^ 0x42)};
}

static void test_stun() {
  uint8_t a[mm::kStunRequestLen], b[mm::kStunRequestLen];
  mm::stun_build_request(a);
  mm::stun_build_request(b);
  const uint8_t header[8] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
  CHECK(std::memcmp(a, header, 8) == 0);
  CHECK(std::memcmp(a + 8, b + 8, 12) != 0);

  mm::StunObservation seen;
  auto reply = stun_reply(a, xor_mapped(203, 0, 113, 7, 41234));
  CHECK(mm::stun_parse_response(a, reply.data(), reply.size(), &seen));
  CHECK(seen.answered && seen.ip[0] == 203 && seen.ip[1] == 0 && seen.ip[2] == 113 && seen.ip[3] == 7 && seen.port == 41234);

  // An odd-length SOFTWARE attribute (padding), a plain MAPPED-ADDRESS elsewhere, then XOR-MAPPED: XOR wins.
  std::vector<uint8_t> attrs = {0x80, 0x22, 0x00, 0x05, 'h', 'e', 'l', 'l', 'o', 0, 0, 0,
                                0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x03, 0xE8, 10, 0, 0, 1};
  const auto xored = xor_mapped(198, 51, 100, 2, 5000);
  attrs.insert(attrs.end(), xored.begin(), xored.end());
  reply = stun_reply(a, attrs);
  seen = {};
  CHECK(mm::stun_parse_response(a, reply.data(), reply.size(), &seen) && seen.ip[0] == 198 && seen.port == 5000);

  // MAPPED-ADDRESS alone is the fallback.
  reply = stun_reply(a, {0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x03, 0xE8, 10, 0, 0, 1});
  seen = {};
  CHECK(mm::stun_parse_response(a, reply.data(), reply.size(), &seen) && seen.ip[0] == 10 && seen.port == 1000);

  // Another request's reply, an error reply and a short packet are refused and leave `out` alone.
  seen = {};
  reply = stun_reply(b, xor_mapped(203, 0, 113, 7, 1));
  CHECK(!mm::stun_parse_response(a, reply.data(), reply.size(), &seen) && !seen.answered);
  reply = stun_reply(a, xor_mapped(203, 0, 113, 7, 1), 0x0111);
  CHECK(!mm::stun_parse_response(a, reply.data(), reply.size(), &seen) && !seen.answered);
  const uint8_t tiny[5] = {};
  CHECK(!mm::stun_parse_response(a, tiny, sizeof tiny, &seen));
}

static void test_match() {
  Server server;
  const uint64_t id = mm::start(request_for(server));
  CHECK(id != 0);
  CHECK(server.accept_upgrade());
  std::string text;
  CHECK(server.read_frame(&text) == 1);
  json join = json::parse(text, nullptr, false);
  CHECK(join.is_object() && join["type"] == "join" && join["user"]["connectCode"] == "TEST#123" && join["natType"] == "cone");
  CHECK(mm::state() == mm::State::Connecting);

  server.send_text(R"({"type":"joined","ticketId":"t-1","token":"tok-1"})");
  CHECK(wait_state(mm::State::Queued, 2000));

  server.send_frame(0x89, "hi");   // ping
  CHECK(server.read_frame(&text) == 0xA && text == "hi");
  server.send_text(R"({"type":"status","secondsInQueue":1.5})");
  server.send_text("not json at all");
  server.send_text(R"({"type":"something-new"})");
  CHECK(mm::state() == mm::State::Queued);

  // The match message arrives split over three frames, with a ping in the middle.
  const std::string match = R"({"type":"match","matchId":"mode.unranked-abc","isHost":true,"players":[{"uid":"x","port":1}]})";
  server.send_frame(0x01, match.substr(0, 10));
  server.send_frame(0x89, "");
  server.send_frame(0x00, match.substr(10, 30));
  server.send_frame(0x80, match.substr(40));
  CHECK(wait_state(mm::State::Matched, 2000));
  std::string result;
  CHECK(mm::take_result_json(&result) && result == match);
  CHECK(!mm::take_result_json(&result));   // each match once
  CHECK(mm::error_message().empty());
  int opcode;
  while ((opcode = server.read_frame(&text, 1000)) == 0xA) {}
  CHECK(opcode == 0x8);   // the client says goodbye
}

static void test_error_and_latest_version() {
  Server server;
  mm::start(request_for(server));
  CHECK(server.accept_upgrade());
  std::string text;
  CHECK(server.read_frame(&text) == 1);
  server.send_text(R"({"type":"error","error":"A new version has been released. Return to menu to update","latestVersion":"3.8.0"})");
  CHECK(wait_state(mm::State::Failed, 2000));
  CHECK(mm::error_message() == "A new version has been released. Return to menu to update");
  CHECK(mm::latest_version() == "3.8.0");
}

static void test_ping_cancel_and_stale_ids() {
  Server old_server, server;
  const uint64_t old_id = mm::start(request_for(old_server));
  const uint64_t id = mm::start(request_for(server));   // ends the first search
  CHECK(id == old_id + 1);
  CHECK(server.accept_upgrade());
  std::string text;
  CHECK(server.read_frame(&text) == 1);
  server.send_text(R"({"type":"joined","ticketId":"t-2","token":"tok-2"})");
  CHECK(wait_state(mm::State::Queued, 2000));

  mm::cancel(old_id);   // an old caller winding down must not end the newer search
  std::this_thread::sleep_for(milliseconds(600));
  CHECK(mm::state() == mm::State::Queued);

  // Five seconds of silence from the service: the client asks whether it is still there.
  CHECK(server.read_frame(&text, 7000) == 1 && json::parse(text, nullptr, false)["type"] == "ping");

  const auto before = std::chrono::steady_clock::now();
  mm::cancel(id);
  CHECK(std::chrono::steady_clock::now() - before < milliseconds(50));   // never waits on the network
  CHECK(server.read_frame(&text) == 1 && json::parse(text, nullptr, false)["type"] == "cancel");
  CHECK(server.read_frame(&text) == 0x8);
  CHECK(wait_state(mm::State::Idle, 2000));
}

static void test_resume() {
  Server server;
  mm::start(request_for(server));
  CHECK(server.accept_upgrade());
  std::string text;
  CHECK(server.read_frame(&text) == 1);
  server.send_text(R"({"type":"joined","ticketId":"t-3","token":"tok-3"})");
  CHECK(wait_state(mm::State::Queued, 2000));

  server.drop();   // the connection dies with a ticket in hand
  CHECK(server.accept_upgrade(5000));
  CHECK(server.read_frame(&text) == 1);
  const json resume = json::parse(text, nullptr, false);
  CHECK(resume.is_object() && resume["type"] == "resume" && resume["ticketId"] == "t-3" && resume["token"] == "tok-3");
  CHECK(mm::state() == mm::State::Queued);
  server.send_text(R"({"type":"joined","ticketId":"t-3","token":"tok-3"})");
  server.send_text(R"({"type":"match","matchId":"m-3"})");
  CHECK(wait_state(mm::State::Matched, 2000));
}

static void test_join_timeout() {
  Server server;
  mm::start(request_for(server));
  CHECK(server.accept_upgrade());
  std::string text;
  CHECK(server.read_frame(&text) == 1);   // and then the service says nothing
  CHECK(wait_state(mm::State::Failed, 7000));
  CHECK(mm::error_message() == "Failed to join mm queue");
}

static void test_no_service() {
  mm::Request r;
  {
    Server gone;   // a port nothing listens on once this is closed
    r = request_for(gone);
  }
  mm::start(r);
  CHECK(wait_state(mm::State::Failed, 8000));
  CHECK(mm::error_message() == "Failed to connect to mm server");

  r.url = "http://example.invalid/";
  mm::start(r);
  CHECK(wait_state(mm::State::Failed, 8000));
}

static void test_refused_upgrade() {
  Server server;
  mm::Request r = request_for(server);
  mm::start(r);
  for (int attempt = 0; attempt < 3; ++attempt) {   // a plain web server: the client tries three times, then gives up
    pollfd p = {server.listener, POLLIN, 0};
    if (poll(&p, 1, 4000) <= 0) break;
    const int conn = accept(server.listener, nullptr, nullptr);
    const char reply[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    char sink[2048];
    recv(conn, sink, sizeof sink, 0);
    send(conn, reply, sizeof reply - 1, 0);
    close(conn);
  }
  CHECK(wait_state(mm::State::Failed, 5000));
  CHECK(mm::error_message() == "Failed to connect to mm server");
}

int main() {
  mm::set_log([](const char* line) { std::printf("    %s\n", line); });
  const struct { const char* name; void (*run)(); } tests[] = {
      {"helpers", test_helpers}, {"stun", test_stun}, {"match", test_match},
      {"error and latest version", test_error_and_latest_version},
      {"ping, cancel, stale ids", test_ping_cancel_and_stale_ids}, {"resume", test_resume},
      {"join timeout", test_join_timeout}, {"no service", test_no_service}, {"refused upgrade", test_refused_upgrade}};
  for (const auto& t : tests) {
    const int before = failures;
    std::printf("%s\n", t.name);
    t.run();
    std::printf("  %s\n", failures == before ? "ok" : "FAILED");
  }
  mm::shutdown();
  std::printf("%s (%d failed checks)\n", failures ? "FAILED" : "all passed", failures);
  return failures ? 1 : 0;
}
