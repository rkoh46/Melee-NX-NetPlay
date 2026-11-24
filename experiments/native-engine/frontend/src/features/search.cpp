// Play while you search, and Training after online, as melee-unlocked does them (its port/runtime/hle/native_practice.cpp: the
// kept search, kept_go_training / kept_tick / kept_go_back, and the session watch that goes to Training when an online session
// ends; its slippi_online.cpp search_away). The netplay object's side is patches/net-0005 (slippi::online::keep_search).
//
// Play while you search (settings.ini play_while_searching, on by default; the Online page's row). On Slippi's character
// select with an Unranked search running and the fighter locked in, hold L and R (two thirds of a second, no A, Start or Z):
// the game goes to an offline Training match with the locked-in fighter while the search goes on. The game sees no online
// session meanwhile (play mode's option words are the player's own, the offline tools work), so whatever is played is plain
// offline play. A line at the top says how long the search has run. When an opponent is found: "Opponent found", three seconds,
// then back to Slippi's character select through the main menu (upstream's way back on its Source Port), still locked in, and
// the match starts as it would have. L and R held again in Training go back at once. If the search stops (an error, the
// service closed it), the line says so and the player stays in Training. Upstream keeps Slippi's Unranked only (its Direct and
// Teams keep is for its own service); so does this.
//
// Warm-up room (settings.ini warmup, on by default; upstream's slippi_warmup.h and warmup_park, its WARM UP button or L+R).
// Connected in Direct or Teams and not locked in: hold L and R on the select and the game goes to Training (with the fighter
// last locked in this session, else Training's own) while the connection stays up. When every other player has locked in:
// "Opponent is ready", three seconds, then back to the select to lock in. L and R held in Training go back at once. If the
// connection is lost meanwhile the player stays where they are. Upstream also tells its own builds "warming up" (message 0xE9,
// shown on their select); a Slippi player just sees this player not locked in yet, as when someone is slow to pick, so it is
// not sent here.
//
// Search from Training (upstream's Practice menu Search page, native_practice.cpp start_search): in an offline Training match
// the Practice menu's Search page starts Slippi Unranked, or Direct or Teams by a code typed on its on-screen keyboard. The
// search is asked for as the online menus would ask (netplay_bridge::request_match: the locked-in pick is the Training
// fighter) and kept at once, so it runs while the player keeps training, exactly as a search kept from the select: the closed
// menu shows the search time, "Stop searching" ends it, and when the opponent is connected the game goes back to Slippi's
// character select for that mode (Direct's first-match state reset as upstream does), still locked in. Services program only
// (the others have no matchmaking); a mod's added fighter is refused, as upstream refuses it.
//
// Training after online (upstream's keys): online_training_after 1: when a Direct or Teams session ends (the other player left,
// Z held, the connection dropped) after at least one match, straight into a Training match with this player's fighter against
// the last opponent's fighter (Unranked excluded, as upstream: it ends a session after each game there). online_return_training
// 1: to Training's character select instead, in every mode. Both off by default, as upstream.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "features_internal.h"
#include "frontend.h"
#include "netplay_bridge.h"                // runner: search_info, keep_search (patches/net-0005)
#include "proai/practice_state.h"          // the Practice menu's Search page (render thread)

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace fe::features {
namespace {
using Clock = std::chrono::steady_clock;
constexpr uint32_t kOnlineMajor = 8, kMainMenuMajor = 1, kTrainingMajor = 0x1C;
constexpr int kUnranked = 1;
constexpr uint64_t kCountdownMs = 3000;      // "Opponent found", then back to the select (upstream kKeepCountdownMs)
constexpr uint64_t kSafeWaitMs = 20000;      // found: the longest wait for a safe moment (kKeepSafeWaitMs)
constexpr uint64_t kGoingMs = 15000;         // on the way back: the select must be up by then (kKeepGoingMs)
constexpr uint32_t kHold = 40;               // L+R held this many frames

bool play_while_searching = true, warmup = true, training_after = false, return_training = false;
int quickchat = 0;                           // 0 on, 1 Direct only, 2 off (Slippi's and upstream's quickchat)
bool pick_previews = true;
int applied_chat = -1; int applied_previews = -1;

uint64_t now_ms() { return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count()); }

enum class Stage { Off, Away, Found, Going };
struct Kept {
  Stage stage = Stage::Off;
  bool warmup = false;                       // the warm-up room (a connection kept), not a search
  int mode = 1;                              // the online mode to go back to
  int character = -1, color = 0, port = 0;   // the locked-in pick and the controller that held L+R
  uint64_t since_ms = 0, found_ms = 0, going_ms = 0, safe_wait_ms = 0;
  uint32_t via_menu = 0;                     // the retrace the way back reached the main menu
  bool left_online = false;
  bool from_menu = false;                    // started from the Practice menu's Search page in Training
  std::string opponent;
} kept;

// The Practice menu's Search page (render thread asks, simulation thread acts).
std::mutex ask_mutex;
struct Ask { int mode = 0; std::string code; int port = 0; bool stop = false; } ask;
std::mutex view_mutex;
fe::proai::SearchView view;
std::string refusal;
uint32_t hold = 0; bool armed = false;
uint32_t prev_major = 0xFFFFFFFF, prev_minor = 0xFFFFFFFF, steady = 0;

std::mutex hud_mutex;
std::string hud_text, note_text;
uint32_t note_until = 0;

void note(const std::string& text, uint32_t frames) {
  std::lock_guard<std::mutex> lock(hud_mutex);
  note_text = text; note_until = retrace() + frames;
}

bool setting(const std::string& key, const std::string& value) {
  const bool on = value != "0";
  if (key == "play_while_searching") play_while_searching = on;
  else if (key == "warmup") warmup = on;
  else if (key == "quickchat") quickchat = std::clamp(std::atoi(value.c_str()), 0, 2);
  else if (key == "pick_previews") pick_previews = on;
  else if (key == "online_training_after") training_after = on;
  else if (key == "online_return_training") return_training = on;
  else return false;
  return true;
}

bool request_major(uint32_t major) {
  MuGameApi* g = game();
  if (!g || !g->practice) return false;
  int32_t a[1] = {int32_t(major)};
  return g->practice(MU_PRACTICE_REQUEST_MAJOR, a, 1) == 0;
}
void set_online_mode(int mode) {
  MuGameApi* g = game();
  if (!g || !g->practice) return;
  int32_t a[1] = {mode};
  g->practice(MU_PRACTICE_SET_ONLINE_MODE, a, 1);
}

// L and R both in (the buttons; no A, Start or Z) on any controller, as upstream reads its hold.
bool lr_held(int* port) {
  const MuPadStatus* p = pads_raw();
  if (!p) return false;
  for (int i = 0; i < 4; ++i)
    if (p[i].err == 0 && (p[i].button & 0x0060u) == 0x0060u && !(p[i].button & (0x0100u | 0x1000u | 0x0010u))) { if (port) *port = i; return true; }
  return false;
}

void end(const std::string& why, bool cleanup) {
  say("search: kept search over: %s", why.c_str());
  if (cleanup) netplay_bridge::end_kept_search(why.c_str());
  else netplay_bridge::keep_search(false);
  kept = Kept{};
}

void go_back(const char* why) {
  say("search: back to the %s select (%s) from scene %u/%u at retrace %u", kept.warmup ? "online" : "Unranked", why, scene_major(), scene_minor(), retrace());
  kept.stage = Stage::Going;
  kept.going_ms = now_ms();
  // The character select starts from the locked-in fighter (upstream write_event_backup), in Unranked.
  if (MuGameApi* g = game(); g && g->practice && kept.character >= 0) {
    int32_t a[3] = {kept.character, kept.color, kept.port};
    g->practice(MU_PRACTICE_SET_EVENT_BACKUP, a, 3);
  }
  if (kept.mode == 2) if (MuGameApi* g = game(); g && g->practice) { int32_t none[1] = {}; g->practice(MU_PRACTICE_DIRECT_FIRST_MATCH, none, 0); }
  set_online_mode(kept.mode);
  // From a match scene through the main menu first, as upstream's Source Port goes back.
  if (scene_major() == kOnlineMajor) return;
  kept.via_menu = 0;
  request_major(kMainMenuMajor);
}

void begin(const netplay_bridge::SearchInfo& s, int port) {
  kept = Kept{};
  kept.stage = Stage::Away;
  kept.character = s.character; kept.color = s.color; kept.port = port;
  kept.since_ms = now_ms();
  netplay_bridge::keep_search(true);
  if (!training_go(s.character, s.color, -1, 0, true)) { end("the game could not be asked for Training", true); return; }
  say("search: kept search: Training with fighter %d (costume %d) while the Unranked search goes on (%u s searched so far)", s.character, s.color,
      s.elapsed_ms / 1000);
}

void begin_warmup(const netplay_bridge::SearchInfo& s, int port, int character, int color) {
  kept = Kept{};
  kept.stage = Stage::Away;
  kept.warmup = true;
  kept.mode = s.mode;
  kept.character = character; kept.color = color; kept.port = port;
  kept.since_ms = now_ms();
  kept.opponent = s.opponent;
  netplay_bridge::keep_search(true);
  if (!training_go(character, color, -1, 0, true)) { netplay_bridge::keep_search(false); kept = Kept{}; return; }
  say("search: warm-up: Training with fighter %d while the %s connection stays up", character, s.mode == 3 ? "Teams" : s.mode == 2 ? "Direct" : "Unranked (test)");
}

const char* mode_name(int mode) { return mode == 2 ? "Direct" : mode == 3 ? "Teams" : mode == 4 ? "Party" : "Unranked"; }

// The Practice menu's Unranked / Direct / Teams (upstream start_search): the Training fighter is the pick, the search kept at once.
bool begin_from_training(const netplay_bridge::SearchInfo& s, int mode, const std::string& code, int port) {
  refusal.clear();
  const auto refuse = [](const char* why) { refusal = why; say("search: Practice menu search refused: %s", why); return false; };
  if (!netplay_bridge::has_services()) return refuse("This program has no matchmaking: search from the app's Online Play");
  if (online() || replay_playing() || vanilla()) return refuse("Search from an offline Training match");
  if (scene_major() != kTrainingMajor || scene_minor() != 2) return refuse("Search from an offline Training match");
  if (kept.stage != Stage::Off || (s.state != 0 && s.state != 5)) return refuse("A search is already running");
  int character = -1, color = 0;
  if (MuGameApi* g = game(); g && g->practice)
    for (int slot = 0; slot < 4 && character < 0; ++slot) {
      int32_t a[9] = {slot};
      if (g->practice(MU_PRACTICE_GET_PLAYER, a, 9) == 0 && a[1] != 3 && a[3] == 0) { character = a[2]; color = a[4]; }
    }
  if (character < 0) return refuse("No fighter of yours was found in this match");
  if (character >= 26) return refuse("Added fighters go online from Online Play > Direct: pick them on its character select");
  if (!netplay_bridge::request_match(mode, code, uint8_t(character), uint8_t(color))) return refuse("Unable to start matchmaking");
  netplay_bridge::keep_search(true);
  kept = Kept{};
  kept.stage = Stage::Away;
  kept.mode = mode;
  kept.character = character; kept.color = color; kept.port = port;
  kept.since_ms = now_ms();
  kept.from_menu = true;
  say("search: Practice menu: %s search started%s%s with fighter %d (costume %d), Training goes on", mode_name(mode), code.empty() ? "" : " for ",
      code.c_str(), character, color);
  return true;
}

void publish_view(const netplay_bridge::SearchInfo& s) {
  fe::proai::SearchView v;
  const bool training = scene_major() == kTrainingMajor && scene_minor() == 2 && !replay_playing() && !vanilla();
  const bool searching = kept.stage != Stage::Off && !kept.warmup;
  v.searching = searching;
  v.available = s.state >= 0 && netplay_bridge::has_services() && (training || searching);
  v.can_start = v.available && !searching && kept.stage == Stage::Off && (s.state == 0 || s.state == 5) && !online();
  v.mode = mode_name(searching ? kept.mode : 1);
  v.elapsed_ms = searching ? s.elapsed_ms : 0;
  v.state = s.state;
  if (searching && kept.stage != Stage::Away) std::snprintf(v.opponent, sizeof v.opponent, "%s", kept.opponent.empty() ? "your opponent" : kept.opponent.c_str());
  std::snprintf(v.note, sizeof v.note, "%s", refusal.c_str());
  std::lock_guard<std::mutex> lock(view_mutex);
  view = v;
}

// Tests only: --feature-test unranked-as-direct treats Unranked as Direct for the warm-up and online_training_after (the loopback
// stand-in pairs Unranked searches; Direct needs a connect code typed on the name entry screen).
bool unranked_as_direct() { static const bool on = feature_test("unranked-as-direct"); return on; }

// ---- Training after online: the session watch ----
struct Session {
  bool connected = false;
  int mode = -1, character = -1, color = 0;
  int opponent = -1, opponent_color = 0;
  uint32_t matches = 0;
  bool was_fighting = false;
} session;

void watch_session(const Retrace& info, const netplay_bridge::SearchInfo& s) {
  const bool connected = s.state == 4 && (!s.kept || kept.warmup);   // a warm-up keeps the session; a kept search is none yet
  if (connected) {
    session.connected = true;
    session.mode = s.mode;
    if (s.locked && s.character >= 0) { session.character = s.character; session.color = s.color; }
    if (info.fighting && !session.was_fighting) ++session.matches;
    // The opponent's fighter, for Training's CPU afterwards (once a second during a match; read only).
    if (info.fighting && info.retrace % 60 == 0) {
      if (MuGameApi* g = game(); g && g->practice) {
        const int local = int(netplay_bridge::local_slot());
        for (int slot = 0; slot < 4; ++slot) {
          if (slot == local) continue;
          int32_t a[9] = {slot};
          if (g->practice(MU_PRACTICE_GET_PLAYER, a, 9) == 0 && a[1] != 3 && a[2] >= 0 && a[2] < 26) { session.opponent = a[2]; session.opponent_color = a[4]; break; }
        }
      }
    }
    session.was_fighting = info.fighting;
    return;
  }
  if (!session.connected) return;
  const Session ended = session;
  session = Session{};
  if (ended.matches == 0 || info.scene_major != kOnlineMajor || replay_playing()) return;
  if (training_after && (ended.mode != kUnranked || unranked_as_direct()) && ended.character >= 0) {
    say("search: online session over after %u match(es) (mode %d); into Training with fighter %d against fighter %d (online_training_after)", ended.matches,
        ended.mode, ended.character, ended.opponent);
    training_go(ended.character, ended.color, ended.opponent, ended.opponent_color, true);
  } else if (return_training) {
    say("search: online session over after %u match(es) (mode %d); to Training's character select (online_return_training)", ended.matches, ended.mode);
    request_major(kTrainingMajor);
  }
}

void on_retrace_body(const Retrace& info) {
  const uint32_t major = info.scene_major, minor = info.scene_minor;
  steady = major == prev_major && minor == prev_minor ? steady + 1 : 0;
  prev_major = major; prev_minor = minor;
  const netplay_bridge::SearchInfo s = netplay_bridge::search_info();
  if (s.state < 0) return;                                          // no netplay object running
  {
    Ask a;
    { std::lock_guard<std::mutex> lock(ask_mutex); std::swap(a, ask); }
    if (a.stop && kept.stage != Stage::Off && !kept.warmup) { note("Search stopped", 240); end("the player stopped the search (Practice menu)", true); }
    else if (a.mode > 0 && begin_from_training(s, a.mode, a.code, a.port)) return;   // the search's state is read again next retrace
    // --feature-test practice-search:MODE[:CODE] (verification runs): the Search page's start, two seconds into the Training match.
    static bool test_done = false; static uint32_t training_ticks = 0;
    std::string v;
    if (!test_done && feature_test("practice-search", &v)) {
      training_ticks = major == kTrainingMajor && minor == 2 ? training_ticks + 1 : 0;
      if (training_ticks == 120) {
        test_done = true;
        const size_t colon = v.find(':');
        const std::string m = v.substr(0, colon), code = colon == std::string::npos ? std::string() : v.substr(colon + 1);
        say("search: test: the Practice menu's %s", m.c_str());
        if (begin_from_training(s, m == "direct" ? 2 : m == "teams" ? 3 : 1, code, 0)) return;
      }
    }
  }
  if (applied_chat != quickchat || applied_previews != int(pick_previews)) {
    applied_chat = quickchat; applied_previews = pick_previews;
    netplay_bridge::set_chat(quickchat, pick_previews);
    say("search: quick chat %s, live pick previews %s", quickchat == 0 ? "on" : quickchat == 1 ? "in Direct only" : "off", pick_previews ? "on" : "off");
  }
  watch_session(info, s);
  int port = 0;
  const bool lr = lr_held(&port);
  if (!lr) armed = true;
  hold = lr && armed ? hold + 1 : 0;
  const uint64_t now = now_ms();

  if (kept.stage == Stage::Off) {
    // On the select, searching, locked in, Unranked: the hold keeps the search and goes to Training.
    const bool possible = play_while_searching && major == kOnlineMajor && minor == 0 && (s.state == 1 || s.state == 2) && s.locked &&
                          s.mode == kUnranked && s.character >= 0 && !replay_playing();
    const bool test_unranked = unranked_as_direct();
    const bool warm = warmup && major == kOnlineMajor && minor == 0 && s.state == 4 && (s.mode == 2 || s.mode == 3 || (test_unranked && s.mode == kUnranked)) &&
                      !s.locked && !replay_playing();
    if (possible && hold == kHold) { hold = 0; armed = false; begin(s, port); }
    else if (warm && hold == kHold) { hold = 0; armed = false; begin_warmup(s, port, session.character, session.color); }
    std::lock_guard<std::mutex> lock(hud_mutex);
    hud_text.clear();
    return;
  }
  if (kept.stage != Stage::Going && !s.kept) { end("the search was ended elsewhere", false); return; }
  if (kept.warmup && (kept.stage == Stage::Away || kept.stage == Stage::Found) && s.state != 4) {
    note("Your opponent left; the warm-up is over", 600);
    end("the connection was lost during the warm-up", true);
    return;
  }
  if (!kept.warmup && (kept.stage == Stage::Away || kept.stage == Stage::Found)) {
    if (s.state == 5 || !s.error.empty()) { note("Search stopped: " + (s.error.empty() ? std::string("the service closed it") : s.error), 600); end("the search stopped: " + s.error, true); return; }
    if (s.state == 0) { note("Search stopped", 300); end("the search is not running", true); return; }
  }
  if (kept.stage == Stage::Away || kept.stage == Stage::Found) {
    if (major != kOnlineMajor) kept.left_online = true;
    if (major == kOnlineMajor && kept.left_online) {
      say("search: kept search: the player is back on the online screens by themselves");
      netplay_bridge::keep_search(false);
      kept = Kept{};
      return;
    }
  }
  if (kept.stage == Stage::Away && (kept.warmup ? s.remote_ready : s.state == 4)) {
    kept.stage = Stage::Found;
    kept.found_ms = now;
    kept.opponent = s.opponent;
    say("search: %s (%s) in scene %u/%u at retrace %u", kept.warmup ? "warm-up: everyone else locked in" : "kept search: opponent found", s.opponent.c_str(),
        major, minor, info.retrace);
  }
  if (kept.stage == Stage::Found && now - kept.found_ms >= kCountdownMs) {
    if (steady >= 20 && !training_entering()) go_back(kept.warmup ? "everyone else locked in" : "opponent found");
    else {
      if (!kept.safe_wait_ms) kept.safe_wait_ms = now;
      if (now - kept.safe_wait_ms > kSafeWaitMs) { note("The match was found, but the game could not go back in time", 600); end("found, but the player could not be brought back in time", true); return; }
    }
  }
  if (kept.stage == Stage::Going) {
    if (major == kMainMenuMajor && !kept.via_menu) kept.via_menu = info.retrace;
    if (kept.via_menu && major == kMainMenuMajor && info.retrace - kept.via_menu == 20) {
      say("search: %s: on the main menu, now the online select (mode %d)", kept.warmup ? "warm-up" : "kept search", kept.mode);
      set_online_mode(kept.mode);
      request_major(kOnlineMajor);
    }
    if (major == kOnlineMajor && minor == 0) {
      say("search: %s: on the online select again at retrace %u (search state %d, locked in %d)", kept.warmup ? "warm-up" : "kept search", info.retrace, s.state,
          s.locked ? 1 : 0);
      netplay_bridge::keep_search(false);
      kept = Kept{};
      std::lock_guard<std::mutex> lock(hud_mutex);
      hud_text.clear();
      return;
    }
    if (now - kept.going_ms > kGoingMs) { note("The character select did not come back; the search is over", 600); end("the select never came back", true); return; }
  }
  if ((kept.stage == Stage::Away || kept.stage == Stage::Found) && hold == kHold) {
    hold = 0; armed = false;
    go_back(kept.stage == Stage::Found ? "L+R held: go now" : "L+R held: back to the search");
  }
  // The line at the top (upstream's search banner, as text).
  char line[200];
  const uint32_t secs = s.elapsed_ms / 1000;
  const uint64_t left = kept.stage == Stage::Found && now - kept.found_ms < kCountdownMs ? (kCountdownMs - (now - kept.found_ms) + 999) / 1000 : 0;
  if (kept.warmup) {
    const uint32_t away = uint32_t((now - kept.since_ms) / 1000);
    if (kept.stage == Stage::Away)
      std::snprintf(line, sizeof line, "Warming up: %s is still picking  %u:%02u   (hold L+R to go back)", kept.opponent.empty() ? "your opponent" : kept.opponent.c_str(),
                    away / 60, away % 60);
    else if (kept.stage == Stage::Found) std::snprintf(line, sizeof line, "Opponent is ready: back to the select in %llu", (unsigned long long)left);
    else std::snprintf(line, sizeof line, "Going back to the select");
  } else if (kept.stage == Stage::Away)
    std::snprintf(line, sizeof line, "Searching for an Unranked opponent  %u:%02u   (hold L+R to go back)", secs / 60, secs % 60);
  else if (kept.stage == Stage::Found)
    std::snprintf(line, sizeof line, "Opponent found%s%s: back to the match in %llu", kept.opponent.empty() ? "" : ": ", kept.opponent.c_str(), (unsigned long long)left);
  else std::snprintf(line, sizeof line, "Opponent found: going back to the match");
  std::lock_guard<std::mutex> lock(hud_mutex);
  hud_text = line;
}

void on_retrace(const Retrace& info) {
  on_retrace_body(info);
  const netplay_bridge::SearchInfo s = netplay_bridge::search_info();
  if (s.state >= 0) publish_view(s);
}

void hud(std::vector<HudLine>& out) {
  std::lock_guard<std::mutex> lock(hud_mutex);
  // A search started from the Practice menu shows in the menu's own closed-menu bar while in Training (when the menu is built in).
  const bool menu_bar = kept.from_menu && &fe::practice_menu_draw && scene_major() == kTrainingMajor;
  if (!hud_text.empty() && !menu_bar) out.push_back({hud_text, kept.stage == Stage::Away ? 0xFFC850FFu : 0x80FF90FFu});
  if (!note_text.empty() && retrace() < note_until) out.push_back({note_text, 0xFF8080FFu});
}

std::string on_off(bool v) { return v ? "On" : "Off"; }
}  // namespace

void register_search() {
  add_setting(setting);
  add_retrace(on_retrace);
  add_hud(hud);
  add_menu_entry({"Online", "Play while searching (hold L+R on the select)", [] { return on_off(play_while_searching); },
                  [](int) { play_while_searching = !play_while_searching; }, nullptr,
                  [](MenuEntry::Write w) { w("play_while_searching", play_while_searching ? "1" : "0"); }});
  add_menu_entry({"Online", "Warm-up room (Direct, Teams: hold L+R on the select)", [] { return on_off(warmup); },
                  [](int) { warmup = !warmup; }, nullptr,
                  [](MenuEntry::Write w) { w("warmup", warmup ? "1" : "0"); }});
  add_menu_entry({"Online", "Quick chat", [] { return std::string(quickchat == 0 ? "On" : quickchat == 1 ? "Direct only" : "Off"); },
                  [](int d) { quickchat = (quickchat + (d < 0 ? 2 : 1)) % 3; }, nullptr,
                  [](MenuEntry::Write w) { w("quickchat", std::to_string(quickchat)); }});
  add_menu_entry({"Online", "Live pick previews (Melee-NX-NetPlay opponents)", [] { return on_off(pick_previews); },
                  [](int) { pick_previews = !pick_previews; }, nullptr,
                  [](MenuEntry::Write w) { w("pick_previews", pick_previews ? "1" : "0"); }});
  add_menu_entry({"Online", "After a Direct or Teams session: into Training", [] { return on_off(training_after); },
                  [](int) { training_after = !training_after; }, nullptr,
                  [](MenuEntry::Write w) { w("online_training_after", training_after ? "1" : "0"); }});
  add_menu_entry({"Online", "After an online session: Training's select", [] { return on_off(return_training); },
                  [](int) { return_training = !return_training; }, nullptr,
                  [](MenuEntry::Write w) { w("online_return_training", return_training ? "1" : "0"); }});
}
}  // namespace fe::features

namespace fe::proai {
SearchView search_view() { std::lock_guard<std::mutex> lock(fe::features::view_mutex); return fe::features::view; }
void search_ask(int mode, const char* code, int port) {
  std::lock_guard<std::mutex> lock(fe::features::ask_mutex);
  fe::features::ask.mode = mode; fe::features::ask.code = code ? code : ""; fe::features::ask.port = port; fe::features::ask.stop = false;
}
void search_stop() { std::lock_guard<std::mutex> lock(fe::features::ask_mutex); fe::features::ask.stop = true; fe::features::ask.mode = 0; }
}  // namespace fe::proai
