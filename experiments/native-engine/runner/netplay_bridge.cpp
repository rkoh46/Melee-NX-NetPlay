// See netplay_bridge.h. Compiled against the app's own headers (port/runtime/...).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "netplay_bridge.h"
#include "slippi_online.h"
#include "slippi_net.h"
#include "slippi_matchmaking_ws.h"
#include "slippi_report.h"
#include "slippi_http.h"
#include "discord_rpc.h"
#include "exi_slippi.h"
#include "host.h"
#include "gx_core.h"
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <sys/stat.h>

namespace {
uint32_t (*g_retrace_count)() = nullptr; void (*g_log_time)(uint64_t) = nullptr; void (*g_set_speed)(double) = nullptr;
std::string (*g_replay_path)() = nullptr;
// Slippi's time sync: the emulation speed it last asked for (game thread; read by the host's pacing and the sound output).
std::atomic<double> g_speed{1.0};
// Set by the netplay thread (each pad acknowledgement) and the game thread, read by the game, render (HUD) and netplay threads.
std::atomic<int> g_ping_ms{0}; std::atomic<bool> g_ping_seen{false};
}

// ---- what the app's netplay code expects from the rest of the app -------------------------------
namespace host {
Options options;
void log(const char* format, ...) {
  char line[1024];
  va_list args; va_start(args, format); std::vsnprintf(line, sizeof line, format, args); va_end(args);
  const auto t = std::chrono::steady_clock::now();
  std::fprintf(stderr, "[net] %s\n", line);
  if (g_log_time) g_log_time(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count()));
}
// The app's device keeps Dolphin-style savestates of guest memory. The native engine's state is
// saved by the runner's own store (commands 0xB1/0xB2 never reach the device), so these only have
// to keep the device's unused savestate objects empty.
static uint8_t g_nothing[16];
uint8_t* ptr(uint32_t, uint32_t) { return g_nothing; }
uint32_t rd32(uint32_t) { return 0; }
uint32_t retrace_count() { return g_retrace_count ? g_retrace_count() : 0; }
double now_seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void sim_cost_add(int, double) {}
// Slippi's time sync nudges the frame rate (and sound follows it); the host paces by it (Setup::set_speed).
void set_emulation_speed(double speed) {
  if (g_speed.exchange(speed, std::memory_order_relaxed) != speed && g_set_speed) g_set_speed(speed);
}
void set_online_ping_ms(int ms) { g_ping_ms.store(ms, std::memory_order_relaxed); g_ping_seen.store(true, std::memory_order_relaxed); }
int online_ping_ms() { return g_ping_ms.load(std::memory_order_relaxed); }
void input_mark_match_start() {}
void notify_local(const std::string&, const std::string&) {}
}
namespace gx { void gpu_sync_all() {} }
namespace slippi {
// Only the reporter's thread asks (patches/net-0008); the host's answer may wait for its replay writer.
const std::string& last_replay_path() { static std::string path; path = g_replay_path ? g_replay_path() : std::string(); return path; }
namespace discord {
void set_menus() {} void set_local_port(int) {} void set_searching(const std::string&) {}
void set_opponent(const std::string&, const std::string&, int) {}
}
#ifndef MU_NET_SERVICES
namespace report {
// No HTTP in this build: every request fails at once without touching the network.
bool http_request(const char*, const std::string&, const std::string&, const std::string&, const char*, int* status, std::string*, std::string* error) {
  if (status) *status = 0;
  if (error) *error = "HTTP is not part of this test build";
  return false;
}
void init(const std::string&, const std::string&) {}
void log_game(const GameReport&) {}
void match_status(const std::string&, const std::string&, const std::string&, const std::string&, bool) {}
void fetch_user_rank(const std::string&) {}
void fetch_match_result(const std::string&, const std::string&, const std::string&) {}
void shutdown() {}
RankFetchStatus rank_info(RankInfo*) { return RankFetchStatus{}; }
}
// The matchmaking service client: never used with a local peer, and never allowed to start.
namespace mm {
uint64_t start(const Request&) { return 0; }
State state() { return State::Failed; }
std::string error_message() { return "matchmaking is not part of this test build"; }
std::string latest_version() { return {}; }
bool take_result_json(std::string*) { return false; }
void cancel(uint64_t) {}
void shutdown() {}
void set_log(void (*)(const char*)) {}
void stun_build_request(uint8_t out[kStunRequestLen]) { std::memset(out, 0, kStunRequestLen); }
bool stun_parse_response(const uint8_t*, const uint8_t*, size_t, StunObservation*) { return false; }
}
#endif   // MU_NET_SERVICES: the real services are the app's slippi_matchmaking_ws.cpp, slippi_report.cpp, slippi_http_curl.cpp
}

namespace netplay_bridge {
bool has_services() {
#ifdef MU_NET_SERVICES
  return true;
#else
  return false;
#endif
}
const char* ranked_refusal() { return "Ranked is not available in this build. Play Unranked, Direct or Teams."; }

static bool start_services(const Setup& setup, std::string* error) {
  if (!has_services()) { if (error) *error = "this build has no online services (build with MU_NET_SERVICES=1)"; return false; }
  if (setup.fake_user) {
    mkdir(setup.user_dir.c_str(), 0777);
    if (FILE* out = std::fopen((setup.user_dir + "/user.json").c_str(), "w")) {
      std::fprintf(out, "{\"uid\":\"mock-test-%d\",\"playKey\":\"mock-test-only\",\"connectCode\":\"MOCK#%03d\",\"displayName\":\"Mock %d\"}\n",
                   setup.local_index, setup.local_index + 1, setup.local_index + 1);
      std::fclose(out);
    }
  }
  slippi::Matchmaking::local_peer.enabled = false;
  auto& config = slippi::online::config();
  config.offline = false; config.user_dir = setup.user_dir; config.delay = setup.delay_frames; config.delay_auto = setup.delay_auto;
  host::options.cache_dir = setup.user_dir; host::options.iso = setup.iso_path;
  slippi::online::init();
  return slippi::online::available();
}

bool start(const Setup& setup, std::string* error) {
  g_retrace_count = setup.retrace_count; g_log_time = setup.log_time; g_set_speed = setup.set_speed; g_replay_path = setup.replay_path;
  if (setup.services) return start_services(setup, error);
  mkdir(setup.user_dir.c_str(), 0777);
  // A made-up identity: the device wants a user.json before it will search. Nothing reads it but
  // this process, and the "play key" opens nothing.
  const std::string path = setup.user_dir + "/user.json";
  if (FILE* out = std::fopen(path.c_str(), "w")) {
    std::fprintf(out, "{\"uid\":\"local-test-%d\",\"playKey\":\"local-test-only\",\"connectCode\":\"TEST#%03d\",\"displayName\":\"Local %d\"}\n",
                 setup.local_index, setup.local_index + 1, setup.local_index + 1);
    std::fclose(out);
  } else { if (error) *error = "cannot write " + path; return false; }
  auto& peer = slippi::Matchmaking::local_peer;
  peer.enabled = true; peer.local_index = setup.local_index; peer.local_port = setup.local_port;
  peer.remote_ip = setup.remote_ip; peer.remote_port = setup.remote_port;
  auto& config = slippi::online::config();
  config.offline = false; config.user_dir = setup.user_dir; config.delay = setup.delay_frames; config.delay_auto = setup.delay_auto;
  host::options.cache_dir = setup.user_dir;
  slippi::online::init();
  if (!slippi::online::available()) { if (error) *error = "the app's online module is not available in this build"; return false; }
  return true;
}
void shutdown() { slippi::online::shutdown(); }
bool handle(uint8_t command, const uint8_t* payload, uint32_t size, std::vector<uint8_t>* reply) {
  return slippi::online::handle(command, payload, size, *reply);
}
bool request_match(int mode, const std::string& connect_code, uint8_t character, uint8_t color) {
  if (mode <= 0 || mode > 4) return false;                  // Ranked is refused; see ranked_refusal()
  std::vector<uint8_t> ignored;
  // CMD_SET_MATCH_SELECTIONS: team, character, colour, character chosen, stage (2 bytes), stage
  // option 3 (random from the allowed list), mode, alternate stage flag.
  const uint8_t selections[9] = {0, character, color, 1, 0, 0, 3, uint8_t(mode), 0};
  slippi::online::handle(0xB5, selections, sizeof selections, ignored);
  uint8_t find[19] = {uint8_t(mode)};                       // mode, then the opponent's code (Shift-JIS; codes are ASCII)
  std::memcpy(find + 1, connect_code.data(), std::min<size_t>(18, connect_code.size()));
  ignored.clear();
  slippi::online::handle(0xB4, find, sizeof find, ignored);
  return true;
}
uint64_t rollbacks_seen_by_device() { return slippi::online::rollback_count(); }
void set_online_prefs(const OnlinePrefs& prefs) {
  auto& config = slippi::online::config();
  config.direct_pause = prefs.direct_pause;
  config.port_colour[0] = prefs.port_colour[0]; config.port_colour[1] = prefs.port_colour[1];
  config.shield_online = prefs.shield_online;
  for (int i = 0; i < 3; ++i) config.blocked_codes[i] = prefs.skip[i];
  config.nx_build_message = !prefs.as_slippi;
  config.pick_previews = prefs.pick_previews;
}
uint32_t port_colour_bits() { return slippi::online::port_colour_bits(); }
uint32_t shield_color(int slot) { return slippi::online::shield_color(slot); }
SearchInfo search_info() {
  const slippi::online::SearchInfo s = slippi::online::search_info();
  SearchInfo out;
  out.state = s.state; out.mode = s.mode; out.locked = s.locked; out.character = s.character; out.color = s.color;
  out.kept = s.kept; out.remote_ready = s.remote_ready; out.elapsed_ms = s.elapsed_ms; out.error = s.error; out.opponent = s.opponent;
  out.code = s.code; out.remote_character = s.remote_character; out.remote_color = s.remote_color;
  return out;
}
void keep_search(bool kept) { slippi::online::keep_search(kept); }
void set_chat(int mode, bool pick_previews) {
  auto& config = slippi::online::config();
  config.chat = std::clamp(mode, 0, 2);
  config.pick_previews = pick_previews;
}
int local_slot() { return slippi::online::local_player_slot(); }
void set_auto_lcancel(bool offered) { slippi::online::config().auto_lcancel_direct = offered; }
bool auto_lcancel_agreed() { return slippi::online::auto_lcancel_agreed(); }
bool search_kept() { return slippi::online::search_info().kept; }
void end_kept_search(const char* why) { slippi::online::end_kept_search(why); }
FrameTrace frame_trace() { FrameTrace t; slippi::online::frame_trace(&t.skip, &t.advanced, &t.offset_us); return t; }
double emulation_speed() { return g_speed.load(std::memory_order_relaxed); }
int ping_ms() { return g_ping_seen.load(std::memory_order_relaxed) ? g_ping_ms.load(std::memory_order_relaxed) : -1; }
void set_delay(int frames, bool automatic) {
  auto& config = slippi::online::config();
  config.delay_auto = automatic;
  if (frames >= 1 && frames <= 9) config.delay = frames;
}
}
