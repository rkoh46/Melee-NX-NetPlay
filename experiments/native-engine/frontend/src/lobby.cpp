// melee-unlocked's Multiplayer Lobby on the Switch app: its peer lobby (port/app/launcher_lobby_p2p.cpp, compiled from the fetched
// tree as it is) on a thread of its own, as its launcher's lobby worker runs it (launcher_lobby.cpp work(): a tick every 50 ms,
// commands queued, a snapshot for the window), the accepted match handed to the game, and the launcher's local match history.
// See lobby.h.
//
// What upstream's launcher does around the lobby, and how it is done here:
//   - lobby-profile.json, lobby-peer-identity.json and lobby-history.json in the lobby's folder (here <app folder>/Lobby): the same
//     files, the same keys. Its developer keys are read too: "url" (a peer address, or "dht://host:port" for a DHT entry point of
//     one's own instead of the public ones; the lobby test's local bootstrap node) and "peer_port" (a fixed UDP port).
//   - The profile: the display name (the Slippi account's unless one is typed), the Slippi connect code (the account's, not
//     editable, as upstream), a location, three mains, the history's wins and losses; "build" is melee-unlocked's version and the
//     Source Port ("1.0.5:source", as upstream's Linux launcher announces it: this app runs the Source Port's engine), "open"
//     vanilla, "ready" once the disc is there and an account is signed in. Its options: auto reject invites, the request sound and
//     its volume (auto_reject_invites, request_sound, request_volume).
//   - A Slippi Direct match (the hosted kind): upstream's Windows launcher starts its game with --lobby-direct CODE
//     --lobby-character N, the game plays one match and ends, and the launcher shows again. Here the home screen closes and the
//     game starts with Slippi's menus; from the main menu it goes to Training as the first main and searches Slippi Direct for the
//     opponent's code from there (features/search.cpp's Practice menu search, upstream's static recomp path); when the searches
//     meet, Slippi's Direct select, locked in, and the match. When that match is over (or no connection came in 90 s, upstream's
//     deadline) the run ends and the app goes back to its home screen on this page (play_main.cpp's visits).
//   - A peer-to-peer match (melee-unlocked's own, mu_net): upstream's launcher closes the lobby (the game takes over its UDP port),
//     starts its game with p2p_arguments(...) and opens the lobby again on the same port afterwards. Here the same: the lobby is
//     held, the game runs the runner's --p2p-* mode with those arguments (net_host.cpp, runner/p2p_bridge.cpp), and when the
//     session's game is over the app is back on this page and the lobby opens again.
//   - The match history (upstream's record_match and p2p_outcome): the game's own end-of-game report (command 0xBD) of a lobby
//     match: a game ended with a winner counts as a win or a loss, anything else as incomplete.
//   - Presence while the game runs (upstream's lobby-game-status.json mailbox): "In game", or "In match" with this player's stocks,
//     from the simulation thread's retrace (two atomics), sent by the lobby's thread.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lobby.h"

#include "features_internal.h"
#include "frontend.h"
#include "launcher_lang.h"
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
#include <ctime>
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

namespace linux_host { void request_stop(int code); }

namespace fe::lobby {
namespace {
using Json = nlohmann::json;
using launcher::lobby::PeerLobby;

constexpr const char* kCharacterNames[kCharacters] = {"Captain Falcon", "Donkey Kong", "Fox", "Mr. Game & Watch", "Kirby",
  "Bowser", "Link", "Luigi", "Mario", "Marth", "Mewtwo", "Ness", "Peach", "Pikachu", "Ice Climbers", "Jigglypuff", "Samus",
  "Yoshi", "Zelda", "Sheik", "Falco", "Young Link", "Dr. Mario", "Roy", "Pichu", "Ganondorf"};

struct State {
  std::mutex mutex;
  std::condition_variable wake, changed;
  bool started = false, stop = false;
  std::string dir, account_name, account_code;
  bool ready = false;
  Json config = Json::object();        // lobby-profile.json as read and written (upstream's keys)
  Json history = Json::array();        // lobby-history.json, newest first
  std::deque<std::pair<std::string, Json>> commands;
  Json snapshot = Json{{"phase", "offline"}};
  std::deque<Match> matches;
  bool hold_wanted = false, held = false;
  std::thread thread;
  // The lobby match of this run: handed to the game, recorded in the history, and whether the app goes home after it.
  bool active = false, return_home = false, p2p_session = false, came_back = false;
  Match current;
  int games_recorded = 0;
  std::vector<std::string> p2p_args;
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
void save_history_locked() {
  const std::string path = g.dir + "/lobby-history.json", temp = path + ".tmp";
  { std::ofstream out(temp, std::ios::binary | std::ios::trunc); if (!out) return; out << g.history.dump(2); }
  std::rename(temp.c_str(), path.c_str());
}
std::pair<int, int> record_locked(const std::string& code = {}) {
  int wins = 0, losses = 0;
  for (const auto& game : g.history) {
    if (!code.empty() && game.value("code", std::string()) != code) continue;
    const std::string r = game.value("result", std::string());
    wins += r == "win"; losses += r == "loss";
  }
  return {wins, losses};
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
  const auto record = record_locked();
  p["wins"] = record.first; p["losses"] = record.second;
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
// The game's arguments line of p2p_arguments split into words (double quotes group a path with spaces).
std::vector<std::string> split_arguments(const std::string& line) {
  std::vector<std::string> out; std::string word; bool quoted = false, any = false;
  for (char c : line) {
    if (c == '"') { quoted = !quoted; any = true; continue; }
    if (c == ' ' && !quoted) { if (any) out.push_back(word); word.clear(); any = false; continue; }
    word += c; any = true;
  }
  if (any) out.push_back(word);
  return out;
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
  bool online_wanted = false, exit_hook = false;
  int held_port = 0;
  std::string notice; unsigned notice_seq = 0, shown_seq = 0;
  std::set<std::string> launched, requests_logged, chat_logged, rooms_logged, rejected;
  const auto add_notice = [&](const std::string& text) { if (!text.empty()) { notice = text; ++notice_seq; } };
  const auto make_peer = [&](const std::string& dir, const std::string& url, int port) {
    peer = std::make_unique<PeerLobby>(dir, url, port);
    // Stopped before the statics the lobby has touched by now are destroyed (atexit runs in reverse order).
    if (!exit_hook) { exit_hook = true; std::atexit(stop_worker); }
  };
  for (;;) {
    std::deque<std::pair<std::string, Json>> batch;
    Json profile; std::string dir, url; int port = 0; bool hold = false, auto_reject = false;
    {
      std::unique_lock<std::mutex> lock(g.mutex);
      g.wake.wait_for(lock, std::chrono::milliseconds(peer ? 50 : 500), [] { return g.stop || !g.commands.empty() || g.hold_wanted != g.held; });
      if (g.stop) break;
      profile = profile_json_locked();
      dir = g.dir; url = g.config.value("url", std::string()); port = g.config.value("peer_port", 0);
      auto_reject = g.config.value("auto_reject_invites", false);
      hold = g.hold_wanted;
      if (!(hold && g.held)) batch.swap(g.commands);
    }
    // Upstream's close_lobby / open_lobby: the game takes over this UDP port for a peer-to-peer match.
    if (hold && !g.held) {
      held_port = 0;
      if (peer) {
        held_port = peer->port();
        try {
          peer->presence({{"status", "In game"}, {"stocks", Json::array()}}); peer->presence_now();
          for (const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(600); std::chrono::steady_clock::now() < until;
               std::this_thread::sleep_for(std::chrono::milliseconds(20))) peer->tick();
        } catch (...) {}
        peer.reset();
        say("lobby: closed for a match: the game takes UDP port %d", held_port);
      }
      { std::lock_guard<std::mutex> lock(g.mutex); g.held = true; g.snapshot = Json{{"phase", "held"}}; }
      g.changed.notify_all();
      continue;
    }
    if (hold) continue;
    if (!hold && g.held) {
      if (held_port) {
        std::string problem;
        for (int attempt = 0; attempt < 10 && !peer; ++attempt) {
          try { make_peer(dir, url, held_port); } catch (const std::exception& e) { problem = e.what(); std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
        }
        if (!peer) { try { make_peer(dir, url, port); } catch (const std::exception& e) { problem = e.what(); } }
        try { if (peer) { if (online_wanted) peer->join(profile); else peer->update_profile(profile); } } catch (const std::exception& e) { problem = e.what(); }
        say("lobby: open again on UDP port %d%s%s", peer ? peer->port() : 0, problem.empty() ? "" : ": ", problem.c_str());
      }
      { std::lock_guard<std::mutex> lock(g.mutex); g.held = false; }
      g.changed.notify_all();
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
            make_peer(dir, url, port);
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
          if (peer) { if (online_wanted) peer->join(profile); else peer->update_profile(profile); }
        } else if (peer) {
          peer->command(c.first, c.second);
          say("lobby: %s", c.first.c_str());
        }
      } catch (const std::exception& e) {
        add_notice(e.what());
        say("lobby: %s: %s", c.first.c_str(), e.what());
        if (c.first == "__online") {
          set_phase("error", e.what());
          if (!peer || !peer->visible()) online_wanted = false;
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
      const std::string self = peer->id();
      // Each of the lobby's own notices once (a refusal, an unanswered request, a delivery).
      const unsigned seq = state.value("notice_seq", 0u);
      if (seq != shown_seq) {
        shown_seq = seq;
        const std::string text = state.value("notice", std::string());
        add_notice(text);
        if (!text.empty()) say("lobby: notice: %s", text.c_str());
      }
      // Auto reject invites (upstream's process_invites): every incoming request is cancelled at once, the player told.
      if (auto_reject)
        for (const auto& r : state.value("requests", Json::array())) {
          const std::string id = r.value("id", std::string());
          if (r.value("state", std::string()) != "pending" || r.value("to", std::string()) != self || !rejected.insert(id).second) continue;
          try { peer->command("cancel", {{"request", id}}); } catch (...) {}
          add_notice("Match request automatically declined.");
          say("lobby: match request from %s automatically declined (auto reject invites)", r["peer"].value("name", std::string("?")).c_str());
        }
      Json pings = Json::object();
      for (const auto& p : peer->pings()) pings[p.first] = p.second;
      Json match;
      while (peer->take_launch(match)) {
        const std::string id = match.value("request", std::string());
        if (id.empty() || !launched.insert(id).second) continue;
        Match m;
        m.request = id; m.code = match.value("code", std::string()); m.name = match.value("name", m.code);
        m.build = match.value("build", std::string());
        m.character = std::clamp(match.value("character", 2), 0, kCharacters - 1);
        if (match.count("p2p")) {
          // Upstream's checks before anything reaches a path or the game's arguments: the request id names the result file, half
          // of the setup came from the other player (p2p_arguments checks every field again).
          const bool plain_id = id.size() == 32 && std::all_of(id.begin(), id.end(), [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); });
          ::mkdir((dir + "/p2p-results").c_str(), 0777);
          m.result_file = dir + "/p2p-results/" + id + ".json";
          const std::string line = plain_id ? launcher::lobby::p2p_arguments(match["p2p"], dir + "/lobby-peer-identity.json", m.result_file) : std::string();
          if (line.empty()) {
            add_notice(launcher::lang::tr("lobby.p2p.setup_failed"));
            peer->command("available");
            say("lobby: an accepted peer-to-peer match could not start: its setup does not hold");
            continue;
          }
          m.p2p = true; m.args = split_arguments(line);
          say("lobby: peer-to-peer match accepted against %s: slot %d, stage %d, the game takes UDP port %d", m.name.c_str(),
              match["p2p"].value("slot", -1), match["p2p"].value("stage", -1), match["p2p"].value("port", 0));
          add_notice(launcher::lang::tr("lobby.p2p.starting", {{"name", m.name}}));
        } else {
          // The code goes to the game as the opponent (upstream's checks in launcher_linux_lobby.inl lobby_launches).
          bool valid = !m.code.empty() && m.code.size() <= 18 && m.code.find('#') != std::string::npos;
          for (char ch : m.code) if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '#')) valid = false;
          if (!valid) {
            add_notice("The match could not start: the opponent's code is not valid.");
            peer->command("available");
            say("lobby: an accepted match could not start: the code is not valid");
            continue;
          }
          say("lobby: match accepted against %s (%s): Slippi Direct as %s", m.name.c_str(), m.code.c_str(), character_name(m.character));
          add_notice("Match accepted. Starting Slippi Direct against " + m.name + "...");
        }
        std::lock_guard<std::mutex> lock(g.mutex);
        g.matches.push_back(m);
      }
      const int before = [&] { std::lock_guard<std::mutex> lock(g.mutex); return int(g.snapshot.value("players", Json::array()).size()); }();
      for (const auto& m : state.value("messages", Json::array()))   // chat lines from other players, logged by sender only
        if (chat_logged.insert(m.value("id", std::string())).second && m.value("sender", std::string()) != self)
          say("lobby: chat message from %s", m.value("name", std::string("?")).c_str());
      for (const auto& room : state.value("private", Json::array())) {   // private rooms: their state and messages, never the text
        const std::string key = room.value("id", std::string()) + ":" + room.value("state", std::string());
        if (rooms_logged.insert(key).second) say("lobby: private chat with %s: %s", room.value("name", std::string("?")).c_str(), room.value("state", std::string()).c_str());
        for (const auto& m : room.value("messages", Json::array()))
          if (chat_logged.insert(m.value("id", std::string())).second && m.value("sender", std::string()) != self)
            say("lobby: private message from %s", m.value("name", std::string("?")).c_str());
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
          say("lobby: request %s %s %s (%s%s)", r.value("id", std::string()).substr(0, 8).c_str(), r.value("to", std::string()) == self ? "from" : "to",
              r["peer"].value("name", std::string("?")).c_str(), r.value("state", std::string()).c_str(),
              r.value("transport", std::string()) == launcher::lobby::kP2pTransport ? ", peer to peer" : "");
      }
      publish(std::move(state));
    } catch (const std::exception& e) {
      say("lobby: tick: %s", e.what());
    }
  }
  if (peer) { try { peer->command("leave"); for (int i = 0; i < 10; ++i) { peer->tick(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); } } catch (...) {} }
}

// ---- the hand-over to the game (simulation thread) ----
enum class Step { Off, Menu, Training, Searching, Connected, Played, Done };
Step step = Step::Off;
Match active;
uint32_t ticks = 0, tries = 0;
std::chrono::steady_clock::time_point search_since;
int local_player = -1;
bool was_fighting = false;
std::mutex hud_mutex;
std::string hud_text;

void set_hud(const std::string& text) { std::lock_guard<std::mutex> lock(hud_mutex); hud_text = text; }
// The run ends and the app goes back to its home screen on the lobby page (upstream: the game process ends, its launcher shows).
void end_run(int code, const char* why) {
  { std::lock_guard<std::mutex> lock(g.mutex); g.return_home = true; }
  say("lobby: %s: back to the lobby", why);
  set_hud("");
  step = Step::Done;
  linux_host::request_stop(code);
}

void reply(uint8_t command, const uint8_t* r, uint32_t size) {
  if (command != 0xB3 || size < 4) return;
  if (r[0] == 4) local_player = r[3] < 4 ? r[3] : -1;    // CONNECTION_SUCCESS: this console's player
  else if ((r[0] == 0 || r[0] == 5) && step != Step::Played) local_player = -1;
}

// The game's end-of-game report (0xBD, 368 bytes: melee-unlocked mu_net result_outbox.cpp's layout: winner at 13, the end method at
// 14). Upstream counts a game ended normally (method 2, GAME!) with a known winner; anything else is incomplete.
void exchange(uint8_t command, const uint8_t* payload, uint32_t size, const uint8_t*, uint32_t, int32_t, bool resim) {
  if (command != 0xBD || resim || !payload || size < 15) return;
  bool lobby_match, p2p;
  { std::lock_guard<std::mutex> lock(g.mutex); lobby_match = g.active; p2p = g.p2p_session; }
  if (!lobby_match) return;
  const int winner = int8_t(payload[13]), method = payload[14];
  const int local = p2p ? netplay_bridge::p2p_local_slot() : local_player;
  const char* outcome = method == 2 && winner >= 0 && winner < 4 && local >= 0 ? (winner == local ? "win" : "loss") : "incomplete";
  char when[32] = {};
  const std::time_t now = std::time(nullptr); std::tm t{}; localtime_r(&now, &t);
  std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", &t);
  {
    std::lock_guard<std::mutex> lock(g.mutex);
    const int game = ++g.games_recorded;
    g.history.insert(g.history.begin(), Json{{"id", g.current.request + "/" + std::to_string(game)}, {"opponent", g.current.name},
                                             {"code", g.current.code}, {"build", g.current.build}, {"result", outcome}, {"when", when},
                                             {"game", game}, {"transport", g.p2p_session ? launcher::lobby::kP2pTransport : launcher::lobby::kHostedTransport}});
    if (g.history.size() > 500) g.history.erase(g.history.begin() + 500, g.history.end());
    save_history_locked();
    g.commands.emplace_back("__profile", Json::object());   // the record announced (upstream: own_profile with wins and losses)
  }
  g.wake.notify_all();
  say("lobby: history: %s against %s (winner %d, this player %d, end method %d)", outcome, g.current.name.c_str(), winner, local, method);
  if (step == Step::Connected || step == Step::Played) step = Step::Played;
}

void retrace(const features::Retrace& info) {
  // Presence for the lobby's thread: In game, or In match with this player's stocks (upstream's publish_lobby_status).
  const int local = g.p2p_session ? netplay_bridge::p2p_local_slot() : local_player;
  const bool in_match = local >= 0 && local < 6 && info.fighting && info.state && (g.p2p_session || local_player >= 0);
  g_presence.store(in_match ? 2 : 1);
  g_stocks.store(in_match ? int(info.state->player[local].stocks) : -1);
  switch (step) {
    case Step::Off: return;
    case Step::Menu:
      // The main menu (or the title screen) up for a second, as --feature-test training does it.
      ticks = info.scene_major == 1 || info.scene_major == 24 ? ticks + 1 : 0;
      if (ticks == 60) {
        if (features::training_go(active.character, 0, -1, 0, false)) { step = Step::Training; ticks = 0; say("lobby: into Training as %s", character_name(active.character)); }
        else if (++tries < 5) ticks = 0;
        else end_run(2, "the game could not go to Training; the lobby match was not started");
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
        step = Step::Connected; set_hud(""); was_fighting = false;
        say("lobby: connected to %s: Slippi Direct's character select", s.opponent.empty() ? active.name.c_str() : s.opponent.c_str());
        break;
      }
      if (ticks % 60 == 0 && s.state != 1 && s.state != 2 && s.state != 3) {
        const proai::SearchView v = proai::search_view();
        if (!v.searching && ++tries < 5) { proai::search_ask(2, active.code.c_str(), 0); say("lobby: the search had not started (%s); asked again", v.note); }
      }
      // Upstream's lobby boot gives the match 90 seconds to start, then the game ends and the launcher shows again.
      if (std::chrono::steady_clock::now() - search_since > std::chrono::seconds(90)) {
        proai::search_stop();
        end_run(2, ("no Direct connection with " + active.code + " in 90 s").c_str());
      }
      break;
    }
    case Step::Connected:
    case Step::Played:
      // Upstream's native lobby boot is one match: once it has been played and the fighters stop, the run ends.
      // Only Slippi's online match scene (8/2) counts: the Training match the search ran from is behind this point.
      if (info.fighting && info.scene_major == 8 && info.scene_minor == 2) was_fighting = true;
      else if (was_fighting && info.scene_major == 8 && info.scene_minor != 2) end_run(0, "the lobby match is over");
      break;
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
  const Json history = Json::parse(read_file(g.dir + "/lobby-history.json"), nullptr, false);
  if (history.is_array()) g.history = history;
  // The identity before the thread starts (upstream calls identity_key first): a damaged file is reported by the lobby itself.
  const std::string key = launcher::lobby::identity_key(g.dir);
  say("lobby: folder %s, identity %s, %zu games in the history", g.dir.c_str(), key.empty() ? "not readable" : (key.substr(0, 8) + "...").c_str(), g.history.size());
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
  p.auto_reject = g.config.value("auto_reject_invites", false);
  p.request_sound = g.config.value("request_sound", true);
  p.request_volume = std::clamp(g.config.value("request_volume", 65), 0, 100);
  return p;
}
void set_profile(const Profile& p) {
  std::lock_guard<std::mutex> lock(g.mutex);
  g.config["name"] = p.name; g.config["location"] = p.location;
  Json mains = Json::array();
  for (int m : p.mains) if (m >= 0 && m < kCharacters && std::find(mains.begin(), mains.end(), m) == mains.end() && mains.size() < 3) mains.push_back(m);
  if (mains.empty()) mains.push_back(2);
  g.config["mains"] = mains;
  g.config["auto_reject_invites"] = p.auto_reject;
  g.config["request_sound"] = p.request_sound;
  g.config["request_volume"] = std::clamp(p.request_volume, 0, 100);
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
Json history() { std::lock_guard<std::mutex> lock(g.mutex); return g.history; }
std::pair<int, int> record(const std::string& code) { std::lock_guard<std::mutex> lock(g.mutex); return record_locked(code); }
bool take_match(Match* out) {
  std::lock_guard<std::mutex> lock(g.mutex);
  if (g.matches.empty()) return false;
  *out = g.matches.front(); g.matches.pop_front();
  return true;
}
void hand_to_game(const Match& m) {
  if (m.p2p) {
    // Upstream's close_lobby: the lobby answers a little longer, says "In game", and lets the socket go before the game binds it.
    std::unique_lock<std::mutex> lock(g.mutex);
    g.hold_wanted = true;
    g.wake.notify_all();
    g.changed.wait_for(lock, std::chrono::seconds(3), [] { return g.held; });
  }
  std::lock_guard<std::mutex> lock(g.mutex);
  g.active = true; g.return_home = false; g.p2p_session = m.p2p; g.current = m; g.games_recorded = 0;
  g.p2p_args = m.p2p ? m.args : std::vector<std::string>{};
  if (!m.p2p) { step = Step::Menu; ticks = 0; tries = 0; active = m; local_player = -1; }
  say("lobby: the game starts the lobby match against %s: %s", m.name.c_str(), m.p2p ? "melee-unlocked's peer-to-peer match" :
      ("Training as " + std::string(character_name(m.character)) + ", then Slippi Direct for " + m.code).c_str());
  if (!m.p2p) set_hud("Lobby match: going to Training to search for " + m.name);
}
bool session_args(std::vector<std::string>* args) {
  std::lock_guard<std::mutex> lock(g.mutex);
  if (!g.active || !g.p2p_session || g.p2p_args.empty()) return false;
  *args = g.p2p_args; g.p2p_args.clear();
  return true;
}
// After a run: true when it was a lobby match that is over (the app goes back to its home screen). The lobby opens again.
bool take_return_home() {
  std::unique_lock<std::mutex> lock(g.mutex);
  const bool back = g.active && (g.return_home || g.p2p_session);
  if (!g.active) return false;
  g.came_back = back;
  g.active = false; g.return_home = false; g.p2p_session = false;
  step = Step::Off; local_player = -1;
  g_presence.store(0); g_stocks.store(-1);
  g.hold_wanted = false;
  g.commands.emplace_back("available", Json::object());   // upstream's launcher after its game: the accepted request is done
  g.wake.notify_all();
  g.changed.wait_for(lock, std::chrono::seconds(4), [] { return !g.held; });
  return back;
}
bool returning() { std::lock_guard<std::mutex> lock(g.mutex); const bool r = g.came_back; g.came_back = false; return r; }
}  // namespace fe::lobby

namespace fe::features {
// registry.cpp (weak): the hand-over, the history and the presence, in the services program only.
void register_lobby() {
  add_retrace(fe::lobby::retrace);
  add_reply(fe::lobby::reply);
  add_exchange(fe::lobby::exchange);
  add_hud(fe::lobby::hud);
}
}  // namespace fe::features
