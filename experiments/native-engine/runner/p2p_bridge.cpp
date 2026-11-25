// melee-unlocked's own peer-to-peer match (P2P Direct: its mu_net rollback session, encrypted, no Slippi account) behind the
// native engine's slippi_command, for net_host's --p2p-* mode. Adapted from melee-unlocked's port/app/source_p2p.cpp (its
// Source Port host's glue, GPL-2.0-or-later): the same options, the same session descriptor (so a melee-unlocked PC and this
// build hold the same one and the handshake's digests agree), the same answers to the game's session commands (inputs, match
// state, game report, series state, cleanup). What differs is the host around it: logging, the time sync's speed and the end
// of the run go through hooks net_host gives (Setup below), state saves and loads stay with net_host's snapshot store as in
// every other netplay mode, and melee-unlocked's early inputs (a sampler of its Windows host) are off.
// Compiled into the netplay object with mu_net (port/runtime/mu_net of the fetched tree, copied by ../net_sources.sh) and
// Monocypher over frontend/src/lobby_shim's Win32 names.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "netplay_bridge.h"
#include "rollback_session.h"

#include <array>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#ifndef MELEE_PORT_VERSION
#error "MELEE_PORT_VERSION: melee-unlocked's version, the build id both sides of a peer-to-peer match compare"
#endif

namespace netplay_bridge {
namespace {
// The game's session commands (melee-unlocked's host/netplay_state.h).
constexpr uint8_t kInputs = 0xB0, kMatchState = 0xB3, kReportGame = 0xBD, kCleanup = 0xBA, kSeriesState = 0xC0;
constexpr uint32_t kMatchStateSize = 1030, kMatchStateError = 357;
constexpr uint8_t kStateError = 5, kInputsDisconnected = 3;
bool is_session_command(uint8_t cmd) { return (cmd >= 0xB0 && cmd <= 0xC4) || (cmd >= 0xD5 && cmd <= 0xD8) || (cmd >= 0xE3 && cmd <= 0xE5); }
// The legal stages a game after the first is drawn from (source_p2p.cpp kLegalStages, docs/mu-net-protocol.md 14.2).
const uint16_t kLegalStages[6] = {0x1F, 0x20, 0x1C, 0x08, 0x02, 0x03};

struct Options {
  int local_port = -1;
  std::vector<mu_net::Endpoint> peers;
  int slot = 0;
  uint8_t character[2] = {2, 2};
  uint8_t color[2] = {0, 0};
  int stage = 0x1F;
  uint32_t seed = 0;
  int delay = 2;
  std::string identity_path = "p2p_identity.json";
  std::string result_path;
  bool has_expected = false;
  mu_net::Bytes32 expected{};
  std::string names[2] = {"P1", "P2"};
  int connect_seconds = 0;
  int games = 1;
};
struct State {
  Options opt;
  P2pHooks hooks;
  mu_net::RollbackSession* session = nullptr;   // never deleted: stopped by p2p_shutdown, and the process outlives it
  bool closed = false, connected_logged = false, over_logged = false, played = false;
  std::atomic<int> exit_code{-1};
  mu_net::SessionState last_state = mu_net::SessionState::Idle;
  std::atomic<double> speed{1.0};
  double speed_applied = 1.0;
};
State& g() { static State s; return s; }

void say(const char* format, ...) __attribute__((format(printf, 1, 2)));
void say(const char* format, ...) {
  char line[1024];
  va_list args; va_start(args, format); std::vsnprintf(line, sizeof line, format, args); va_end(args);
  if (g().hooks.log) g().hooks.log(line); else std::fprintf(stderr, "%s\n", line);
}
bool refuse(const char* name, const char* expected) { say("p2p: %s %s", name, expected); return false; }
bool parse_number(const char* text, int base, long low, long high, long* out) {
  if (!text || !*text) return false;
  char* end = nullptr;
  const long v = std::strtol(text, &end, base);
  if (*end || v < low || v > high) return false;
  *out = v;
  return true;
}
bool parse_fighter(const std::string& text, uint8_t* character, uint8_t* color) {
  const size_t slash = text.find('/');
  long id = 0, shade = 0;
  if (!parse_number(text.substr(0, slash).c_str(), 0, 0, 255, &id)) return false;
  if (slash != std::string::npos && !parse_number(text.substr(slash + 1).c_str(), 0, 0, 255, &shade)) return false;
  *character = uint8_t(id); *color = uint8_t(shade);
  return true;
}
// The content and rules both sides must share (source_p2p.cpp content_identity): the retail disc, and rules profile 2, the
// game library built with the Slippi layer, which is the one this engine's game image is.
mu_net::SessionDescriptor build_descriptor(const Options& o) {
  mu_net::SessionDescriptor d;
  d.route = mu_net::Route::P2PDirect;
  d.build_id = MELEE_PORT_VERSION;
  static const char kContent[] = "retail-1.02";
  d.content_hash.fill(0);
  std::memcpy(d.content_hash.data(), kContent, sizeof kContent - 1);
  d.rules_profile = 2;
  for (int i = 0; i < 2; ++i) {
    d.players[i].present = true;
    d.players[i].character = o.character[i];
    d.players[i].color = o.color[i];
    d.players[i].name = o.names[i];
  }
  d.stage = uint16_t(o.stage);
  d.rng_seed = o.seed;
  d.input_delay = uint8_t(o.delay);
  const uint8_t id[16] = {'c', 'l', 'i', 0, uint8_t(o.seed >> 24), uint8_t(o.seed >> 16), uint8_t(o.seed >> 8), uint8_t(o.seed),
                          o.character[0], o.color[0], o.character[1], o.color[1], uint8_t(o.stage >> 8), uint8_t(o.stage), uint8_t(o.delay), 0};
  std::memcpy(d.match_id.data(), id, sizeof id);
  return d;
}
void apply_speed() {
  State& s = g();
  const double speed = s.speed.load(std::memory_order_relaxed);
  if (speed == s.speed_applied) return;
  s.speed_applied = speed;
  if (s.hooks.set_speed) s.hooks.set_speed(speed);
}
// The session has nothing more for the game (source_p2p.cpp session_done, the build with the Slippi layer): the run ends,
// 0 after a game, 2 when none was played. net_host stops the host when it sees the code.
void session_done() {
  State& s = g();
  if (s.exit_code.load() >= 0) return;
  say("p2p: leaving the game (%s)", s.played ? "the session's game is over" : "no match was played");
  s.exit_code.store(s.played ? 0 : 2);
}
void publish() {
  using mu_net::SessionState;
  State& s = g();
  const SessionState state = s.session->state();
  if (state == SessionState::Playing) s.played = true;
  if (state == s.last_state) return;
  s.last_state = state;
  const bool path_up = state == SessionState::Handshaking || state == SessionState::Verifying || state == SessionState::Ready || state == SessionState::Playing;
  if (path_up && !s.connected_logged) {
    s.connected_logged = true;
    say("p2p: connected to the other player (local UDP port %u)", unsigned(s.session->local_port()));
  }
  if (state == SessionState::Ready) say("p2p: both sides hold the same session descriptor: the match starts");
  if (state == SessionState::Failed) {
    say("p2p: failed: %s", s.session->failure_text().c_str());
    if (!s.played) session_done();
  }
}
std::string result_path(const std::string& base, uint32_t game) {
  if (game <= 1) return base;
  const size_t slash = base.find_last_of("/\\"), dot = base.rfind('.');
  const std::string tag = ".g" + std::to_string(game);
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return base + tag;
  return base.substr(0, dot) + tag + base.substr(dot);
}
void write_result() {
  State& s = g();
  if (s.opt.result_path.empty()) return;
  mu_net::GameResult result;
  if (!s.session->result(result)) return;
  const uint32_t game = s.session->game_index();
  const std::string path = result_path(s.opt.result_path, game);
  std::string error;
  if (s.session->write_result_file(path, &error))
    say("p2p: result file written: %s (game %u, %d input frames, the other side's inputs %s)", path.c_str(), unsigned(game), result.transcript_frames,
        result.peer_agreement > 0 ? "agree" : result.peer_agreement == 0 ? "DISAGREE" : "not compared yet");
  else
    say("p2p: result file not written: %s", error.c_str());
}
void advance_series() {
  State& s = g();
  if (s.session->next_game() != mu_net::NextGame::Agreed) return;
  write_result();
  if (!s.session->begin_next_game()) return;
  const mu_net::SessionDescriptor d = s.session->descriptor();
  say("p2p: game %u of the session: stage %u, seed %08X", unsigned(s.session->game_index()), unsigned(d.stage), d.rng_seed);
}
void close_session() {
  State& s = g();
  if (!s.session || s.closed) return;
  write_result();
  s.session->stop();
  s.closed = true;
  s.speed.store(1.0);
  apply_speed();
  publish();
}
void over_reply(std::vector<uint8_t>& reply) {
  static const char text[] = "The match is over";
  reply.assign(kMatchStateSize, 0);
  reply[0] = kStateError;
  std::memcpy(reply.data() + kMatchStateError, text, sizeof text);
}
}  // namespace

bool p2p_option(const std::string& name, const char* value) {
  Options& o = g().opt;
  const std::string text = value ? value : "";
  long n = 0;
  if (name == "--p2p-port") {
    if (!parse_number(value, 10, 0, 65535, &n)) return refuse("--p2p-port", "<local UDP port, 0 to 65535>");
    o.local_port = int(n);
  } else if (name == "--p2p-peer") {
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || !parse_number(text.c_str() + colon + 1, 10, 1, 65535, &n))
      return refuse("--p2p-peer", "<dotted IPv4 address>:<port>");
    if (int(o.peers.size()) >= mu_net::kMaxCandidates) return refuse("--p2p-peer", "was given too many times");
    mu_net::Endpoint e; e.address = text.substr(0, colon); e.port = uint16_t(n);
    o.peers.push_back(e);
  } else if (name == "--p2p-slot") {
    if (!parse_number(value, 10, 0, 1, &n)) return refuse("--p2p-slot", "<0|1>");
    o.slot = int(n);
  } else if (name == "--p2p-chars") {
    const size_t colon = text.find(':');
    if (colon == std::string::npos || !parse_fighter(text.substr(0, colon), &o.character[0], &o.color[0]) ||
        !parse_fighter(text.substr(colon + 1), &o.character[1], &o.color[1]))
      return refuse("--p2p-chars", "<character id>[/<color>]:<character id>[/<color>]");
  } else if (name == "--p2p-stage") {
    if (!parse_number(value, 0, 0, 0xFFFF, &n)) return refuse("--p2p-stage", "<stage id>");
    o.stage = int(n);
  } else if (name == "--p2p-seed") {
    char* end = nullptr;
    const unsigned long seed = std::strtoul(text.c_str(), &end, 16);
    if (text.empty() || *end || text.size() > 10) return refuse("--p2p-seed", "<hex, up to 8 digits>");
    o.seed = uint32_t(seed);
  } else if (name == "--p2p-delay") {
    if (!parse_number(value, 10, mu_net::kMinInputDelay, mu_net::kMaxInputDelay, &n)) return refuse("--p2p-delay", "<1 to 15 frames>");
    o.delay = int(n);
  } else if (name == "--p2p-identity") {
    if (text.empty()) return refuse("--p2p-identity", "<file>");
    o.identity_path = text;
  } else if (name == "--p2p-expect") {
    if (!mu_net::from_hex(text.data(), text.size(), o.expected.data(), o.expected.size()))
      return refuse("--p2p-expect", "<64 hex digits: the other player's identity key>");
    o.has_expected = true;
  } else if (name == "--p2p-result") {
    if (text.empty()) return refuse("--p2p-result", "<file>");
    o.result_path = text;
  } else if (name == "--p2p-names") {
    const size_t colon = text.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size() || colon > mu_net::kMaxNameBytes || text.size() - colon - 1 > mu_net::kMaxNameBytes)
      return refuse("--p2p-names", "<slot 0 name>:<slot 1 name> (at most 31 bytes each)");
    o.names[0] = text.substr(0, colon); o.names[1] = text.substr(colon + 1);
  } else if (name == "--p2p-connect-seconds") {
    if (!parse_number(value, 10, 1, 600, &n)) return refuse("--p2p-connect-seconds", "<1 to 600>");
    o.connect_seconds = int(n);
  } else if (name == "--p2p-games") {
    if (!parse_number(value, 10, 0, long(mu_net::kMaxSessionGames), &n)) return refuse("--p2p-games", "<1 to 127, or 0 for no limit>");
    o.games = int(n);
  } else return refuse(name.c_str(), "is not a peer-to-peer option");
  return true;
}
bool p2p_requested() { return g().opt.local_port >= 0 && !g().opt.peers.empty(); }
int p2p_local_slot() { return g().opt.slot; }
int p2p_character() { return g().opt.character[g().opt.slot]; }

bool p2p_start(const P2pHooks& hooks, std::string* error) {
  State& s = g();
  s.hooks = hooks;
  if (!p2p_requested()) { if (error) *error = "a peer-to-peer match needs both --p2p-port and --p2p-peer"; return false; }
  if (s.session) return true;
  const Options& o = s.opt;
  mu_net::PeerIdentity identity;
  std::string why;
  const mu_net::IdentityFile loaded = mu_net::load_or_create_identity(o.identity_path, identity, &why);
  if (loaded == mu_net::IdentityFile::Failed) { if (error) *error = "the identity file " + o.identity_path + " cannot be used: " + why; return false; }
  say("p2p: identity %s (%s %s)", identity.id().c_str(), loaded == mu_net::IdentityFile::Created ? "created" : "from", o.identity_path.c_str());
  s.session = new mu_net::RollbackSession;
  mu_net::SessionCallbacks callbacks;
  callbacks.log = [](const char* line) { say("p2p: %s", line); };
  callbacks.set_emulation_speed = [](double speed) { g().speed.store(speed, std::memory_order_relaxed); };
  mu_net::SessionOptions session_options;
  if (o.connect_seconds > 0) session_options.connect_limit_us = uint64_t(o.connect_seconds) * 1000000;
  session_options.max_games = uint32_t(o.games);
  session_options.early_inputs = false;   // melee-unlocked's early-input sampler is part of its Windows host
  session_options.stage_pool.assign(std::begin(kLegalStages), std::end(kLegalStages));
  s.session->configure(callbacks, session_options);
  s.session->set_identity(identity);
  identity.wipe();
  mu_net::EndpointInfo endpoint;
  endpoint.local_port = uint16_t(o.local_port);
  endpoint.candidates = o.peers;
  endpoint.has_expected_peer = o.has_expected;
  endpoint.expected_peer = o.expected;
  const mu_net::SessionDescriptor descriptor = build_descriptor(o);
  std::string peers;
  for (const mu_net::Endpoint& e : o.peers) peers += (peers.empty() ? "" : ", ") + e.address + ":" + std::to_string(e.port);
  say("p2p: slot %d, characters %u/%u and %u/%u, stage %d, seed %08X, delay %d, games %d, UDP port %d, peer %s, build %s", o.slot,
      unsigned(o.character[0]), unsigned(o.color[0]), unsigned(o.character[1]), unsigned(o.color[1]), o.stage, o.seed, o.delay, o.games,
      o.local_port, peers.c_str(), MELEE_PORT_VERSION);
  // The lobby held this port a moment ago (the game takes over the lobby's own port): two seconds for it to come free.
  bool started = s.session->start(descriptor, o.slot, endpoint);
  for (int attempt = 0; !started && attempt < 10 && o.local_port > 0 && s.session->failure_text().find("port could not be opened") != std::string::npos; ++attempt) {
    if (attempt == 0) say("p2p: UDP port %d is busy, trying again for 2 s", o.local_port);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    started = s.session->start(descriptor, o.slot, endpoint);
  }
  if (!started) {
    if (error) *error = "the session did not start: " + s.session->failure_text();
    s.closed = true;
    return false;
  }
  publish();
  return true;
}

bool p2p_handle(uint8_t cmd, const uint8_t* payload, uint32_t size, std::vector<uint8_t>* reply_out) {
  State& s = g();
  if (!s.session) return false;
  std::vector<uint8_t>& reply = *reply_out;
  apply_speed();
  switch (cmd) {
    case kMatchState:
      if (s.closed) { over_reply(reply); session_done(); return true; }
      advance_series();
      {
        std::string why;
        if (s.session->state() == mu_net::SessionState::Ended && s.session->next_game(&why) == mu_net::NextGame::Over) {
          if (!s.over_logged) { s.over_logged = true; say("p2p: the session is over: %s", why.c_str()); }
          over_reply(reply); publish(); session_done();
          return true;
        }
      }
      s.session->match_state_reply(reply);
      publish();
      return true;
    case kSeriesState: {
      const mu_net::NextGame next = s.closed ? mu_net::NextGame::Over : s.session->next_game();
      reply.assign(1, uint8_t(next == mu_net::NextGame::Waiting || next == mu_net::NextGame::Agreed ? 1 : 0));
      publish();
      return true;
    }
    case kInputs:
      if (s.closed) { reply.assign(1, kInputsDisconnected); return true; }
      s.session->on_inputs(payload, size, reply);
      publish();
      return true;
    case kReportGame: {
      s.session->on_game_end(payload, size);
      const mu_net::SessionCounters c = s.session->counters();
      say("p2p: game end: %d input frames confirmed, %llu waits, %llu frames stalled, %llu checksums compared, %llu mismatched%s", c.transcript_frames,
          (unsigned long long)c.waits, (unsigned long long)c.stalls, (unsigned long long)c.checksums_compared, (unsigned long long)c.checksum_mismatches,
          c.desync ? ", DESYNC" : "");
      write_result();
      publish();
      return true;
    }
    case kCleanup:
      close_session();
      session_done();
      return true;
    default:
      return is_session_command(cmd);   // the other session commands (search, chat, ranked) have nothing behind them here
  }
}
int p2p_exit_request() { return g().exit_code.load(); }
void p2p_report() {
  State& s = g();
  if (!s.session) return;
  const mu_net::SessionCounters c = s.session->counters();
  say("p2p: session: %llu checksums compared, %llu mismatched%s, ping %u ms, %d of the other side's frames, %llu packets refused",
      (unsigned long long)c.checksums_compared, (unsigned long long)c.checksum_mismatches, c.desync ? ", DESYNC" : "", unsigned(c.ping_us / 1000),
      c.remote_frame, (unsigned long long)c.packets_rejected);
}
void p2p_shutdown() { close_session(); }
// Before another run in the same process (the app back on its home screen, then another match): a new session from new options.
// The stopped session object is left as it is (its threads are joined by stop()).
void p2p_reset() {
  close_session();
  State& s = g();
  s.opt = Options{}; s.session = nullptr; s.closed = false; s.connected_logged = false; s.over_logged = false; s.played = false;
  s.exit_code.store(-1); s.last_state = mu_net::SessionState::Idle; s.speed.store(1.0); s.speed_applied = 1.0;
}
}  // namespace netplay_bridge
