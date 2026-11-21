// C++ port of the `matchmaking` crate of project-slippi/slippi-rust-extensions (GPL-2.0, commit 934f373).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Slippi 3.7 finds opponents through a WebSocket service (wss://matchmaking.slippi.gg/v1/queue) instead of
// the ENet server earlier versions used. This is that client: the session (join, queue, match, resume after
// a dropped connection, cancel), its JSON messages, and the two STUN helpers the netplay socket uses to learn
// its public address. The surface mirrors upstream's ffi/src/matchmaking.rs.
//
// The portlibs curl on the Switch has no ws/wss support, so the WebSocket upgrade and RFC 6455 framing are
// done here over a connect-only curl handle (TLS and certificate checks stay curl's). A ws:// URL uses a
// plain socket, which is what the host test drives.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace slippi::mm {

enum class State { Idle = 0, Connecting = 1, Queued = 2, Matched = 3, Failed = 4 };   // same values as slprs_mm_state

// What one STUN server reported for the netplay socket. Left unanswered for a server that did not reply.
struct StunObservation {
  bool answered = false;
  uint8_t ip[4] = {};
  uint16_t port = 0;   // host order
};

struct Request {
  uint8_t mode = 0;                    // 0 ranked, 1 unranked, 2 direct, 3 teams, 4 party
  std::vector<uint8_t> connect_code;   // opponent or lobby code, the raw Shift-JIS bytes the game provides
  uint16_t netplay_port = 0;           // local UDP port the ENet socket is bound to
  std::string lan_addr;                // "ip:port" on the local network, or empty
  StunObservation stun[2];             // in the order the two servers were asked
  std::string uid, play_key, user_connect_code, display_name;   // upstream reads these from its user manager
  std::string app_version;             // the Slippi version this build implements
  std::string url;                     // empty: SLIPPI_MM_URL if set, else the dev service for a "dev" version, else prod
};

// Ends any earlier session, starts a worker thread and returns a non-zero session id.
uint64_t start(const Request& request);
State state();
std::string error_message();               // the text the game shows once the state is Failed; upstream wording
std::string latest_version();              // non-empty once the service has said this version is outdated
bool take_result_json(std::string* out);   // the match message exactly as the service sent it; each match once
void cancel(uint64_t session_id);          // ignored when a newer session is active; never blocks on the network
void shutdown();                           // app exit: cancel and join every worker

// Where the client reports what it is doing. Nothing private is ever passed: no uid, play key, address or JSON.
void set_log(void (*log)(const char* line));

constexpr size_t kStunRequestLen = 20;
// A binding request with a fresh transaction id. Send it from the netplay socket.
void stun_build_request(uint8_t out[kStunRequestLen]);
// The public address in a success reply to `request`. False, leaving `out` untouched, for anything else.
bool stun_parse_response(const uint8_t* request, const uint8_t* response, size_t response_len, StunObservation* out);

// ---- pieces exposed for the host test (tools/test_matchmaking_ws.cpp) ----------------------------------
namespace detail {
// "unknown", "cone" (both servers saw the same public address) or "symmetric".
const char* classify_nat(const StunObservation& first, const StunObservation& second);
// The join message for a request, as sent on a fresh connection.
std::string build_join_json(const Request& request);
// base64(SHA-1(key + RFC 6455 GUID)): what the server must answer in Sec-WebSocket-Accept.
std::string websocket_accept(const std::string& key);
}  // namespace detail

}  // namespace slippi::mm
