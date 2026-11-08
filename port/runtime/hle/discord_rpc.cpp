// See discord_rpc.h.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "discord_rpc.h"
#include "host.h"
#include "slippi_login.h"
#include <nlohmann/json.hpp>
#include "json_safe.h"
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <pthread/qos.h>
#endif
#if !defined(_WIN32)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace slippi::discord {
namespace {
using json = nlohmann::json;
constexpr const char* kSlippiUrl = "https://slippi.gg/";
constexpr const char* kRankNames[20] = {"", "Bronze 1", "Bronze 2", "Bronze 3", "Silver 1", "Silver 2", "Silver 3", "Gold 1", "Gold 2", "Gold 3",
                                        "Platinum 1", "Platinum 2", "Platinum 3", "Diamond 1", "Diamond 2", "Diamond 3", "Master 1", "Master 2", "Master 3", "Grandmaster"};

enum class Phase { Menus, Searching, Opponent, Game };
struct State {
  std::string name, code, rank; float rating = 0.0f; bool show_rank = true;
  Phase phase = Phase::Menus;
  std::string mode, opponent; int opponent_rank = 0; int local_port = -1;
  int stage = -1, players = 0, port[4] = {}, character[4] = {}, stocks[4] = {}, wins[4] = {};
  std::string display[4], match_id, set_id;
  uint32_t game_number = 0, tiebreak = 0;
  bool ended = false;
  long long started = 0;
};
std::mutex g_mutex;
State g_state;
bool g_dirty = true;
std::string g_app_id;
std::atomic<bool> g_running{false}, g_connected{false};
std::thread g_thread;
int g_fd = -1;

std::string lower_key(std::string s) { for (char& c : s) c = c == ' ' ? '_' : (char)std::tolower((unsigned char)c); return s; }
// The dashboard says "Platinum II"; the artwork is named after "Platinum 2". Empty for Pending/Unranked.
std::string tier_name(const std::string& rank) {
  if (rank == "Grandmaster") return rank;
  for (const char* t : {"Bronze", "Silver", "Gold", "Platinum", "Diamond", "Master"}) {
    const std::string prefix = std::string(t) + " ";
    if (rank.rfind(prefix, 0) != 0) continue;
    const std::string n = rank.substr(prefix.size());
    if (n == "I" || n == "1") return prefix + "1";
    if (n == "II" || n == "2") return prefix + "2";
    if (n == "III" || n == "3") return prefix + "3";
  }
  return "";
}
// The stage artwork is keyed by Melee's internal stage id; the replay stream carries the external one.
int external_to_internal(int e) {
  switch (e) {
    case 2: return 12; case 3: return 16; case 4: return 2; case 5: return 4; case 6: return 8; case 7: return 14; case 8: return 10; case 9: return 20;
    case 10: return 18; case 11: return 3; case 12: return 5; case 13: return 6; case 14: return 7; case 15: return 9; case 16: return 11; case 17: return 13;
    case 18: return 21; case 19: return 24; case 20: return 25; case 22: return 15; case 23: return 17; case 24: return 19; case 25: return 22;
    case 27: return 27; case 28: return 28; case 29: return 29; case 30: return 30; case 31: return 36; case 32: return 37;
  }
  return 0;
}
std::string stage_asset(int e) { const int i = external_to_internal(e); return i ? "stage" + std::to_string(i) : "questionmark"; }
std::string character_asset(int c) { return c >= 0 && c <= 25 ? "char" + std::to_string(c) : "questionmark"; }
std::string mode_title(const std::string& m) {
  if (m == "ranked") return "Ranked"; if (m == "unranked") return "Unranked"; if (m == "direct") return "Direct"; if (m == "teams") return "Teams";
  return m;
}
std::string mode_from_match(const std::string& id) {
  if (id.find("mode.unranked") != std::string::npos) return "Unranked";
  if (id.find("mode.ranked") != std::string::npos) return "Ranked";
  if (id.find("mode.direct") != std::string::npos) return "Direct";
  if (id.find("mode.teams") != std::string::npos) return "Teams";
  return id.empty() ? "" : "Online";
}
std::string ascii(const uint8_t* p, size_t n) {   // connect-code style names; non-ASCII (Shift-JIS) bytes are dropped
  std::string out;
  for (size_t i = 0; i < n && p[i]; ++i) if (p[i] >= 0x20 && p[i] < 0x7F) out += (char)p[i];
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}
uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

std::pair<std::string, std::string> player_badge(const State& s) {
  const std::string tier = s.show_rank ? tier_name(s.rank) : "";
  if (tier.empty()) return {"", "Slippi Online"};   // no Slippi logo: Discord shows the application's own icon instead
  return {lower_key(tier), tier + " \xC2\xB7 " + std::to_string((int)std::lround(s.rating))};
}
json build(const State& s) {
  json a, assets;
  json buttons = json::array({json{{"label", "Get Slippi"}, {"url", kSlippiUrl}}});
  if (s.code.find('#') != std::string::npos) {
    std::string slug = s.code; for (char& c : slug) c = c == '#' ? '-' : (char)std::tolower((unsigned char)c);
    buttons.push_back(json{{"label", "View Slippi Profile"}, {"url", std::string(kSlippiUrl) + "user/" + slug}});
  }
  const auto badge = player_badge(s);
  switch (s.phase) {
    case Phase::Menus:
      a["details"] = "Slippi Online"; a["state"] = "In menus";
      if (!badge.first.empty()) assets = {{"large_image", badge.first}, {"large_text", badge.second}};
      break;
    case Phase::Searching:
      a["details"] = s.mode.empty() ? std::string("In queue") : "In queue - " + mode_title(s.mode);
      a["state"] = "Searching for an opponent";
      a["party"] = {{"size", {1, 2}}};
      if (!badge.first.empty()) assets = {{"large_image", badge.first}, {"large_text", badge.second}};
      break;
    case Phase::Opponent: {
      a["details"] = "Opponent found";
      a["state"] = s.opponent.empty() ? std::string("Connecting...") : "vs " + s.opponent;
      a["party"] = {{"size", {2, 2}}};
      if (s.show_rank && s.opponent_rank >= 1 && s.opponent_rank <= 19) assets = {{"large_image", lower_key(kRankNames[s.opponent_rank])}, {"large_text", kRankNames[s.opponent_rank]}};
      else if (!badge.first.empty()) assets = {{"large_image", badge.first}, {"large_text", badge.second}};
      break;
    }
    case Phase::Game: {
      const std::string mode = mode_from_match(s.match_id);
      std::string details = mode.empty() ? std::string("Offline") : mode;
      if (!mode.empty() && s.game_number) details += " - Game " + std::to_string(s.game_number);
      if (s.tiebreak) details += " (tiebreak)";
      if (!mode.empty() && s.ended && s.players == 2) details += " - Set " + std::to_string(s.wins[s.port[0]]) + "-" + std::to_string(s.wins[s.port[1]]);
      a["details"] = details;
      auto name = [&](int i) { return !s.display[i].empty() ? s.display[i] : std::string(slippi::login::character_name(s.character[i])); };
      if (s.players != 2) a["state"] = "In game";
      else if (s.ended) a["state"] = name(0) + " vs " + name(1) + " - Game over";
      else a["state"] = name(0) + " " + std::to_string(s.stocks[s.port[0]]) + " - " + std::to_string(s.stocks[s.port[1]]) + " " + name(1);
      int me = 0;
      for (int i = 0; i < s.players; ++i) if (s.port[i] == s.local_port) me = i;
      assets = {{"large_image", stage_asset(s.stage)}, {"large_text", slippi::login::stage_name(s.stage)}};
      if (s.players > 0) { assets["small_image"] = character_asset(s.character[me]); assets["small_text"] = slippi::login::character_name(s.character[me]); }
      if (s.started) a["timestamps"] = {{"start", s.started}};
      if (!mode.empty()) a["party"] = {{"size", {2, 2}}};
      break;
    }
  }
  if (assets.is_object() && !assets.empty()) a["assets"] = assets;
  a["buttons"] = buttons;
  return a;
}
void touch_state(const std::function<void(State&)>& f) { std::lock_guard<std::mutex> lock(g_mutex); f(g_state); g_dirty = true; }

#if !defined(_WIN32)
bool send_frame(uint32_t opcode, const std::string& payload) {
  uint8_t header[8];
  const uint32_t len = (uint32_t)payload.size();
  std::memcpy(header, &opcode, 4); std::memcpy(header + 4, &len, 4);
  if (::send(g_fd, header, 8, 0) != 8) return false;
  size_t off = 0;
  while (off < payload.size()) {
    const ssize_t n = ::send(g_fd, payload.data() + off, payload.size() - off, 0);
    if (n <= 0) return false;
    off += (size_t)n;
  }
  return true;
}
void drain_replies() {   // replies are informational; errors (a bad activity, a revoked application) are logged once each
  static std::string last_error;
  uint8_t buf[8192];
  ssize_t n;
  while ((n = ::recv(g_fd, buf, sizeof buf, MSG_DONTWAIT)) > 8) {
    const std::string body((const char*)buf + 8, (size_t)n - 8);
    if (body.find("\"evt\":\"ERROR\"") != std::string::npos && body != last_error) { last_error = body; host::log("discord: %.300s", body.c_str()); }
  }
}
bool try_connect() {
  const char* tmp = std::getenv("TMPDIR");
  std::string dirs[2] = {tmp ? std::string(tmp) : std::string(), "/tmp/"};
  for (std::string& d : dirs) {
    if (d.empty()) continue;
    if (d.back() != '/') d += '/';
    for (int i = 0; i < 10; ++i) {
      const std::string path = d + "discord-ipc-" + std::to_string(i);
      sockaddr_un addr{}; addr.sun_family = AF_UNIX;
      if (path.size() >= sizeof addr.sun_path) continue;
      std::strcpy(addr.sun_path, path.c_str());
      const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd < 0) continue;
#if defined(SO_NOSIGPIPE)
      const int one = 1; ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);   // Discord quitting must not kill the game
#endif
      if (::connect(fd, (sockaddr*)&addr, sizeof addr) != 0) { ::close(fd); continue; }
      g_fd = fd;
      if (!send_frame(0, json{{"v", 1}, {"client_id", g_app_id}}.dump())) { ::close(g_fd); g_fd = -1; continue; }
      g_connected.store(true);
      { std::lock_guard<std::mutex> lock(g_mutex); g_dirty = true; }
      host::log("discord: connected to %s", path.c_str());
      return true;
    }
  }
  return false;
}
void disconnect() {
  if (g_fd >= 0) { send_frame(2, "{}"); ::close(g_fd); g_fd = -1; }
  if (g_connected.exchange(false)) host::log("discord: disconnected");
}
void run() {
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
  using clock = std::chrono::steady_clock;
  auto last_send = clock::now() - std::chrono::seconds(60), last_try = last_send;
  while (g_running.load()) {
    const auto now = clock::now();
    if (!g_connected.load()) {
      if (now - last_try >= std::chrono::seconds(10)) { last_try = now; try_connect(); }
    } else {
      drain_replies();
      json activity; bool send = false;
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        // Discord accepts about five activity updates per 20 s: at most one every 2 s, and a refresh each minute.
        if ((g_dirty && now - last_send >= std::chrono::seconds(2)) || now - last_send >= std::chrono::seconds(60)) { activity = build(g_state); g_dirty = false; send = true; }
      }
      if (send) {
        static std::string last_logged;
        // Presence may carry no images at all (no rank to show): read them without assuming the object exists.
        const nlohmann::json activity_assets = activity.is_object() && activity.count("assets") && activity.at("assets").is_object() ? activity.at("assets") : nlohmann::json::object();
        const std::string summary = jget(activity, "details", "") + " | " + jget(activity, "state", "") + " | " + jget(activity_assets, "large_image", "") + " " + jget(activity_assets, "small_image", "");
        if (summary != last_logged) { last_logged = summary; host::log("discord: presence %s", summary.c_str()); }
        const json args = {{"pid", (int)::getpid()}, {"activity", activity}};
        const std::string frame = json{{"cmd", "SET_ACTIVITY"}, {"args", args}, {"nonce", std::to_string(now.time_since_epoch().count())}}.dump();
        if (!send_frame(1, frame)) disconnect(); else last_send = now;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  disconnect();
}
#else
void run() {}
#endif
}  // namespace

void start(bool show_rank, const std::string& application_id_override) {
#if defined(__APPLE__) && !TARGET_OS_OSX
  (void)show_rank; (void)application_id_override;   // iOS has no Discord IPC socket to talk to
#else
  if (g_running.load()) return;
  g_app_id = application_id_override.empty() ? kApplicationId : application_id_override;
  touch_state([&](State& s) { s.show_rank = show_rank; s.phase = Phase::Menus; });
  g_running.store(true);
  g_thread = std::thread(run);
  host::log("discord: rich presence on (application %s%s)", g_app_id.c_str(), show_rank ? ", rank shown" : "");
#endif
}
void stop() {
  if (!g_running.exchange(false)) return;
  if (g_thread.joinable()) g_thread.join();
}
bool connected() { return g_connected.load(); }

void set_player(const std::string& name, const std::string& code, const std::string& rank, float rating) {
  touch_state([&](State& s) { s.name = name; s.code = code; s.rank = rank; s.rating = rating; });
}
void set_menus() { touch_state([](State& s) { s.phase = Phase::Menus; s.mode.clear(); s.opponent.clear(); s.opponent_rank = 0; s.local_port = -1; }); }
void set_searching(const std::string& mode) { touch_state([&](State& s) { s.phase = Phase::Searching; s.mode = mode; s.opponent.clear(); s.opponent_rank = 0; }); }
void set_opponent(const std::string& mode, const std::string& opponent, int rank) {
  touch_state([&](State& s) { s.phase = Phase::Opponent; s.mode = mode; s.opponent = opponent; s.opponent_rank = rank; });
}
void set_local_port(int port) { touch_state([&](State& s) { s.local_port = port; }); }

void on_replay_event(const uint8_t* d, size_t n) {
  if (!g_running.load() || n < 1) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  State& s = g_state;
  switch (d[0]) {
    case 0x36: {   // Game Start
      if (n < 0x2A0) return;
      s.phase = Phase::Game; s.ended = false; s.players = 0;
      s.stage = be16(d + 0x13);
      for (int i = 0; i < 4; ++i) {
        if (d[0x66 + 0x24 * i] == 3) continue;   // empty port
        const int k = s.players++;
        s.port[k] = i; s.character[k] = d[0x65 + 0x24 * i]; s.stocks[i] = d[0x67 + 0x24 * i];
        s.display[k] = ascii(d + 0x1A5 + 0x1F * i, 31);
      }
      s.match_id = n >= 0x2F1 ? ascii(d + 0x2BE, 51) : "";
      s.game_number = n >= 0x2F5 ? be32(d + 0x2F1) : 0;
      s.tiebreak = n >= 0x2F9 ? be32(d + 0x2F5) : 0;
      if (s.match_id.empty() || s.match_id != s.set_id) { std::memset(s.wins, 0, sizeof s.wins); s.set_id = s.match_id; }
      s.started = (long long)std::time(nullptr);
      g_dirty = true;
      break;
    }
    case 0x38: {   // Post-Frame Update: live stocks
      if (n < 0x22 || s.phase != Phase::Game || s.ended) return;
      const int port = d[0x5];
      if (port > 3 || d[0x6]) return;   // followers (Nana) share the leader's stocks
      if (s.stocks[port] != d[0x21]) { s.stocks[port] = d[0x21]; g_dirty = true; }
      break;
    }
    case 0x39: {   // Game End: placements drive the set score
      if (s.phase != Phase::Game || s.ended) return;
      s.ended = true;
      if (n >= 7) for (int i = 0; i < 4; ++i) if ((int8_t)d[0x3 + i] == 0) ++s.wins[i];
      g_dirty = true;
      break;
    }
  }
}
}  // namespace slippi::discord
