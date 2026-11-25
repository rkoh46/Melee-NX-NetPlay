// melee-unlocked's Multiplayer Lobby on the Switch app: its peer lobby (port/app/launcher_lobby_p2p.cpp, compiled from the fetched
// tree with patches/0142) on a thread of its own, as its launcher's lobby worker runs it (launcher_lobby.cpp work(): a tick every
// 50 ms, commands queued, a snapshot for the window), and the accepted match handed to the game. See lobby.h.
//
// What upstream's launcher does around the lobby, and how it is done here:
//   - lobby-profile.json and lobby-peer-identity.json in the lobby's folder (here <app folder>/Lobby): the same files, the same keys.
//     Its developer keys are read too: "url" (a peer address, or "dht://host:port" for a DHT entry point of one's own instead of
//     the public ones; the lobby test's local bootstrap node) and "peer_port" (a fixed UDP port).
//   - The profile: the display name (the Slippi account's unless one is typed), the Slippi connect code (the account's, not
//     editable, as upstream), a location, three mains; "build" is melee-unlocked's version and the Source Port ("1.0.5:source", as
//     upstream's Linux launcher announces it: this app runs the Source Port's engine), "open" vanilla only, "ready" once the disc is
//     there and an account is signed in.
//   - An accepted match: upstream's Windows launcher starts its game with --lobby-direct CODE --lobby-character N. This app is
//     already the game: the home screen closes and the game starts with Slippi's menus; from the main menu the game goes to
//     Training as the first main and searches Slippi Direct for the opponent's code from there, kept while the player trains
//     (features/search.cpp's Practice menu search, upstream's static recomp path through its practice matchmaking). When the
//     opponent's search meets it, the game goes to Slippi's Direct character select, locked in, and the match starts as Direct does.
//   - Presence while the game runs (upstream's lobby-game-status.json mailbox): "In game", or "In match" with this player's stocks,
//     from the simulation thread's retrace (two atomics), sent by the lobby's thread.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lobby.h"

#include "features_internal.h"
#include "frontend.h"
#include "launcher_lobby_p2p.h"
#include "netplay_bridge.h"
#include "proai/practice_state.h"
#ifdef __SWITCH__
#include "switch_threads.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <sys/stat.h>

#ifndef MU_LOBBY_BUILD
#define MU_LOBBY_BUILD "1.0.5:source"
#endif

namespace fe::lobby {
namespace {
using Json = nlohmann::json;
using launcher::lobby::PeerLobby;

constexpr const char* kCharacterNames[kCharacters] = {"Captain Falcon", "Donkey Kong", "Fox", "Mr. Game & Watch", "Kirby",
  "Bowser", "Link", "Luigi", "Mario", "Marth", "Mewtwo", "Ness", "Peach", "Pikachu", "Ice Climbers", "Jigglypuff", "Samus",
  "Yoshi", "Zelda", "Sheik", "Falco", "Young Link", "Dr. Mario", "Roy", "Pichu", "Ganondorf"};

struct State {
  std::mutex mutex;
  std::condition_variable wake;
  bool started = false, stop = false;
  std::string dir, account_name, account_code;
  bool ready = false;
  Json config = Json::object();        // lobby-profile.json as read and written (upstream's keys)
  std::deque<std::pair<std::string, Json>> commands;
  Json snapshot = Json{{"phase", "offline"}};
  std::deque<Match> matches;
  std::thread thread;
} g;

// Presence from the simulation thread: 0 Online (no game yet), 1 In game, 2 In match; the local player's stocks in a match.
std::atomic<int> g_presence{0}, g_stocks{-1};

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss; ss << in.rdbuf();
  return ss.str();
}
void save_config_locked() {
  ::mkdir(g.dir.c_str(), 0777);
  std::ofstream out(g.dir + "/lobby-profile.json", std::ios::binary | std::ios::trunc);
  if (out) out << g.config.dump(2);
}
// The profile the lobby announces (upstream's profile_config in launcher_lobby_ui.inl, the Slippi build's fields).
Json profile_json_locked() {
  Json p = Json::object();
  std::string name = g.config.value("name", std::string());
  if (name.empty()) name = g.account_name;
  p["name"] = name.substr(0, 32);
  p["code"] = g.account_code;
  p["location"] = g.config.value("location", std::string());
  p["build"] = MU_LOBBY_BUILD;
  p["mains"] = g.config.value("mains", Json::array({2, 20, 9}));
  p["ready"] = g.ready;
  p["open"] = Json::array({"vanilla"});
  p["hosted_ok"] = !g.account_code.empty();   // a Slippi account: the Direct match can be started
  return p;
}
void publish(Json snap) {
  std::lock_guard<std::mutex> lock(g.mutex);
  g.snapshot = std::move(snap);
}
void set_phase(const std::string& phase, const std::string& error = {}) {
  std::lock_guard<std::mutex> lock(g.mutex);
  g.snapshot["phase"] = phase;
  if (!error.empty()) g.snapshot["error"] = error; else g.snapshot.erase("error");
}

// At the process's end (atexit): the lobby leaves the public roster and its thread ends before the statics it uses go away.
void stop_worker() {
  {
    std::lock_guard<std::mutex> lock(g.mutex);
    if (g.stop) return;
    g.stop = true;
  }
  g.wake.notify_all();
  if (g.thread.joinable()) g.thread.join();
}

// The lobby's thread: upstream's work() loop, peer mode only.
void work() {
  std::unique_ptr<PeerLobby> peer;
  bool online_wanted = false;
  std::string notice; unsigned notice_seq = 0, shown_seq = 0;
  std::set<std::string> launched, requests_logged, chat_logged;
  bool exit_hook = false;
  const auto add_notice = [&](const std::string& text) { if (!text.empty()) { notice = text; ++notice_seq; } };
  for (;;) {
    std::deque<std::pair<std::string, Json>> batch;
    Json profile; std::string dir, url; int port = 0;
    {
      std::unique_lock<std::mutex> lock(g.mutex);
      g.wake.wait_for(lock, std::chrono::milliseconds(peer ? 50 : 500), [] { return g.stop || !g.commands.empty(); });
      if (g.stop) break;
      batch.swap(g.commands);
      profile = profile_json_locked();
      dir = g.dir; url = g.config.value("url", std::string()); port = g.config.value("peer_port", 0);
    }
    for (auto& c : batch) {
      try {
        if (c.first == "__online") {
          if (profile.value("code", std::string()).empty()) {
            set_phase("error", "Online play needs a Slippi account: sign in on the Play page.");
            say("lobby: Go Online refused: no Slippi account on this console");
            continue;
          }
          if (!peer) {
            set_phase("starting");
            say("lobby: going online (%s)", url.rfind("dht://", 0) == 0 ? "the DHT entry point in lobby-profile.json" : url.empty() ? "the public DHT" : "a peer address in lobby-profile.json");
            peer = std::make_unique<PeerLobby>(dir, url, port);
            // Stopped before the statics the lobby has touched by now are destroyed (atexit runs in reverse order).
            if (!exit_hook) { exit_hook = true; std::atexit(stop_worker); }
          }
          peer->join(profile);
          online_wanted = true;
          add_notice("You're online. Select a player to send a match request.");
          say("lobby: online on UDP port %d, id %s", peer->port(), peer->id().substr(0, 8).c_str());
        } else if (c.first == "__leave") {
          online_wanted = false;
          if (peer) peer->command("leave");
          say("lobby: left the public lobby");
        } else if (c.first == "__profile") {
          if (peer) peer->update_profile(profile);
        } else if (peer) {
          peer->command(c.first, c.second);
          say("lobby: %s", c.first.c_str());
        }
      } catch (const std::exception& e) {
        add_notice(e.what());
        say("lobby: %s: %s", c.first.c_str(), e.what());
        if (c.first == "__online") {
          set_phase("error", e.what());
          if (peer && !peer->visible()) online_wanted = false;
          if (!peer) online_wanted = false;
        }
      }
    }
    if (!peer) continue;
    try {
      const int presence = g_presence.load(), stocks = g_stocks.load();
      Json status = {{"status", presence == 2 ? "In match" : presence == 1 ? "In game" : "Online"}, {"stocks", Json::array()}};
      if (presence == 2 && stocks >= 0 && stocks <= 99) status["stocks"].push_back(stocks);
      peer->presence(status);
      peer->tick();
      Json state = peer->state();
      // Each of the lobby's own notices once (a refusal, an unanswered request, a delivery).
      const unsigned seq = state.value("notice_seq", 0u);
      if (seq != shown_seq) {
        shown_seq = seq;
        const std::string text = state.value("notice", std::string());
        add_notice(text);
        if (!text.empty()) say("lobby: notice: %s", text.c_str());
      }
      Json pings = Json::object();
      for (const auto& p : peer->pings()) pings[p.first] = p.second;
      Json match;
      while (peer->take_launch(match)) {
        const std::string id = match.value("request", std::string());
        if (id.empty() || !launched.insert(id).second) continue;
        // The code goes to the game as the opponent (upstream's checks in launcher_linux_lobby.inl lobby_launches).
        const std::string code = match.value("code", std::string());
        bool valid = !code.empty() && code.size() <= 18 && code.find('#') != std::string::npos;
        for (char ch : code) if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '#')) valid = false;
        if (match.count("p2p") || !valid) {
          add_notice("The match could not start: the opponent's code is not valid.");
          peer->command("available");
          say("lobby: an accepted match could not start (%s)", match.count("p2p") ? "a peer-to-peer setup" : "the code is not valid");
          continue;
        }
        Match m;
        m.request = id; m.code = code; m.name = match.value("name", code);
        m.character = std::clamp(match.value("character", 2), 0, kCharacters - 1);
        say("lobby: match accepted against %s (%s): Slippi Direct as %s", m.name.c_str(), m.code.c_str(), character_name(m.character));
        add_notice("Match accepted. Starting Slippi Direct against " + m.name + "...");
        std::lock_guard<std::mutex> lock(g.mutex);
        g.matches.push_back(m);
      }
      const int before = [&] { std::lock_guard<std::mutex> lock(g.mutex); return int(g.snapshot.value("players", Json::array()).size()); }();
      {   // chat lines from other players, logged by sender only (never the text)
        for (const auto& m : state.value("messages", Json::array()))
          if (chat_logged.insert(m.value("id", std::string())).second && m.value("sender", std::string()) != peer->id())
            say("lobby: chat message from %s", m.value("name", std::string("?")).c_str());
      }
      state["phase"] = state["self"].value("visible", false) ? "online" : "offline";
      state["pings"] = pings;
      state["notice"] = notice; state["notice_seq"] = notice_seq;
      state["ready"] = profile.value("ready", false);
      state["account"] = profile.value("code", std::string());
      state["port"] = peer->port();
      const int now_players = int(state.value("players", Json::array()).size());
      if (now_players != before) say("lobby: %d player%s in the lobby%s", now_players, now_players == 1 ? "" : "s", now_players ? "" : " (looking)");
      for (const auto& r : state.value("requests", Json::array())) {
        const std::string key = r.value("id", std::string()) + ":" + r.value("state", std::string());
        if (requests_logged.insert(key).second)
          say("lobby: request %s %s %s (%s)", r.value("id", std::string()).substr(0, 8).c_str(), r.value("to", std::string()) == peer->id() ? "from" : "to",
              r["peer"].value("name", std::string("?")).c_str(), r.value("state", std::string()).c_str());
      }
      publish(std::move(state));
    } catch (const std::exception& e) {
      say("lobby: tick: %s", e.what());
    }
  }
  if (peer) { try { peer->command("leave"); for (int i = 0; i < 10; ++i) { peer->tick(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); } } catch (...) {} }
}

// ---- the hand-over to the game (simulation thread) ----
struct Handoff {
  std::mutex mutex;
  bool pending = false;
  Match match;
} g_handoff;
enum class Step { Off, Menu, Training, Searching, Connected, Failed };
Step step = Step::Off;
Match active;
uint32_t ticks = 0, tries = 0;
std::chrono::steady_clock::time_point search_since;
int local_player = -1;
std::mutex hud_mutex;
std::string hud_text;

void set_hud(const std::string& text) { std::lock_guard<std::mutex> lock(hud_mutex); hud_text = text; }

void reply(uint8_t command, const uint8_t* r, uint32_t size) {
  if (command != 0xB3 || size < 4) return;
  if (r[0] == 4) local_player = r[3] < 4 ? r[3] : -1;    // CONNECTION_SUCCESS: this console's player
  else if (r[0] == 0 || r[0] == 5) local_player = -1;
}

void retrace(const features::Retrace& info) {
  // Presence for the lobby's thread: In game, or In match with this player's stocks (upstream's publish_lobby_status).
  const bool in_match = local_player >= 0 && info.fighting && info.state;
  g_presence.store(in_match ? 2 : 1);
  g_stocks.store(in_match ? int(info.state->player[local_player].stocks) : -1);
  if (step == Step::Off) {
    std::lock_guard<std::mutex> lock(g_handoff.mutex);
    if (!g_handoff.pending) return;
    g_handoff.pending = false; active = g_handoff.match; step = Step::Menu; ticks = 0; tries = 0;
    say("lobby: the game starts the lobby match: Training as %s, then Slippi Direct for %s", character_name(active.character), active.code.c_str());
    set_hud("Lobby match: going to Training to search for " + active.name);
  }
  switch (step) {
    case Step::Menu:
      // The main menu (or the title screen) up for a second, as --feature-test training does it.
      ticks = info.scene_major == 1 || info.scene_major == 24 ? ticks + 1 : 0;
      if (ticks == 60) {
        if (features::training_go(active.character, 0, -1, 0, false)) { step = Step::Training; ticks = 0; say("lobby: into Training as %s", character_name(active.character)); }
        else if (++tries < 5) ticks = 0;
        else { step = Step::Failed; say("lobby: the game could not go to Training; the lobby match was not started"); set_hud(""); }
      }
      break;
    case Step::Training:
      ticks = info.scene_major == 0x1C && info.scene_minor == 2 ? ticks + 1 : 0;
      if (ticks == 120) {
        proai::search_ask(2, active.code.c_str(), 0);
        step = Step::Searching; ticks = 0; search_since = std::chrono::steady_clock::now();
        say("lobby: Slippi Direct search for %s (%s) from Training", active.code.c_str(), active.name.c_str());
        set_hud("Lobby match: searching Slippi Direct for " + active.name + " (" + active.code + ")");
      }
      break;
    case Step::Searching: {
      const netplay_bridge::SearchInfo s = netplay_bridge::search_info();
      ++ticks;
      if (s.state == 4) {
        step = Step::Connected; set_hud("");
        say("lobby: connected to %s: Slippi Direct's character select", s.opponent.empty() ? active.name.c_str() : s.opponent.c_str());
        break;
      }
      // The search did not start (the Practice menu's refusal): asked again a second later, a few times.
      if (ticks % 60 == 0 && s.state != 1 && s.state != 2 && s.state != 3) {
        const proai::SearchView v = proai::search_view();
        if (!v.searching && ++tries < 5) { proai::search_ask(2, active.code.c_str(), 0); say("lobby: the search had not started (%s); asked again", v.note); }
      }
      // Upstream's lobby boot gives the match 90 seconds to start.
      if (std::chrono::steady_clock::now() - search_since > std::chrono::seconds(90)) {
        proai::search_stop();
        step = Step::Failed; set_hud("");
        say("lobby: no Direct connection with %s in 90 s; the search stopped (the player stays in Training)", active.code.c_str());
      }
      break;
    }
    default: break;
  }
}

void hud(std::vector<features::HudLine>& out) {
  std::lock_guard<std::mutex> lock(hud_mutex);
  if (!hud_text.empty()) out.push_back({hud_text, 0xFFE070FFu});
}
}  // namespace

const char* character_name(int id) { return id >= 0 && id < kCharacters ? kCharacterNames[id] : "?"; }

void start(const std::string& directory, const std::string& account_name, const std::string& account_code, bool ready) {
  std::lock_guard<std::mutex> lock(g.mutex);
  g.account_name = account_name; g.account_code = account_code; g.ready = ready;
  if (g.started) return;
  g.started = true; g.dir = directory;
  ::mkdir(g.dir.c_str(), 0777);
  const Json cfg = Json::parse(read_file(g.dir + "/lobby-profile.json"), nullptr, false);
  g.config = cfg.is_object() ? cfg : Json::object();
  g.config["mode"] = "peer";
  if (!g.config.count("mains") || !g.config["mains"].is_array() || g.config["mains"].empty()) g.config["mains"] = Json::array({2, 20, 9});
  // The identity before the thread starts (upstream calls identity_key first): a damaged file is reported by the lobby itself.
  const std::string key = launcher::lobby::identity_key(g.dir);
  say("lobby: folder %s, identity %s", g.dir.c_str(), key.empty() ? "not readable" : (key.substr(0, 8) + "...").c_str());
#ifdef __SWITCH__
  switch_set_pthread_cores(0, 0x3);   // off the game's core 2: the lobby keeps running while the game plays
#endif
  g.thread = std::thread(work);   // it lives as long as the app (presence while the game runs); stop_worker ends it
#ifdef __SWITCH__
  switch_set_pthread_cores(-1, 0);
#endif
  std::atexit(stop_worker);
}
bool started() { std::lock_guard<std::mutex> lock(g.mutex); return g.started; }
Profile profile() {
  std::lock_guard<std::mutex> lock(g.mutex);
  Profile p;
  p.name = g.config.value("name", std::string());
  p.location = g.config.value("location", std::string());
  for (const auto& m : g.config.value("mains", Json::array())) if (m.is_number_integer()) p.mains.push_back(m.get<int>());
  return p;
}
void set_profile(const Profile& p) {
  std::lock_guard<std::mutex> lock(g.mutex);
  g.config["name"] = p.name; g.config["location"] = p.location;
  Json mains = Json::array();
  for (int m : p.mains) if (m >= 0 && m < kCharacters && std::find(mains.begin(), mains.end(), m) == mains.end() && mains.size() < 3) mains.push_back(m);
  if (mains.empty()) mains.push_back(2);
  g.config["mains"] = mains;
  save_config_locked();
  g.commands.emplace_back("__profile", Json::object());
  g.wake.notify_all();
}
void go_online() { std::lock_guard<std::mutex> lock(g.mutex); g.commands.emplace_back("__online", Json::object()); g.wake.notify_all(); }
void leave() { std::lock_guard<std::mutex> lock(g.mutex); g.commands.emplace_back("__leave", Json::object()); g.wake.notify_all(); }
void command(const std::string& action, const Json& data) {
  std::lock_guard<std::mutex> lock(g.mutex);
  g.commands.emplace_back(action, data);
  g.wake.notify_all();
}
Json snapshot() { std::lock_guard<std::mutex> lock(g.mutex); return g.snapshot; }
bool take_match(Match* out) {
  std::lock_guard<std::mutex> lock(g.mutex);
  if (g.matches.empty()) return false;
  *out = g.matches.front(); g.matches.pop_front();
  return true;
}
void hand_to_game(const Match& m) {
  std::lock_guard<std::mutex> lock(g_handoff.mutex);
  g_handoff.pending = true; g_handoff.match = m;
}
}  // namespace fe::lobby

namespace fe::features {
// registry.cpp (weak): the hand-over and the presence, in the services program only.
void register_lobby() {
  add_retrace(fe::lobby::retrace);
  add_reply(fe::lobby::reply);
  add_hud(fe::lobby::hud);
}
}  // namespace fe::features
