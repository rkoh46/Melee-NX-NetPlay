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
// service closed it), the line says so and the player stays in Training. Unranked, Direct and Teams (a typed connect code)
// alike: upstream lets Slippi's Direct and Teams practise while the code is searched too (its wait_practice_go, which starts the
// code's search again from Training; here the running search is kept, as for Unranked). Party is left out, as upstream.
//
// Leaving the select while searching (settings.ini keep_search, upstream's key: 0 ask, the default; 1 always keep searching;
// 2 never). With a search running and the fighter locked in, B on the select asks "Keep searching while you play?": A keeps the
// search and goes to the menus (the search goes on as above, and the player is brought back when it finds someone), B stops it
// and goes to the menus; Z still stops the search on the select. 1: B leaves at once and keeps searching. 2: B is the game's.
//
// Search again (upstream's g_offer_*): when an online session ended and the player went to Training by online_training_after
// or online_return_training, "Search Unranked again?" (or Direct, Teams: the same code) once the Training match runs: D-pad Up
// searches again with the same fighter (the search is kept from the start, so the player trains until it finds someone), D-pad
// Down keeps training. Leaving Training drops the offer.
//
// Warm-up room (settings.ini warmup, on by default; upstream's slippi_warmup.h and warmup_park, its WARM UP button or L+R).
// Connected in Direct or Teams and not locked in: hold L and R on the select and the game goes to Training (with the fighter
// last locked in this session, else Training's own) while the connection stays up. The Training CPU is the sparring partner
// (upstream's warmup_enter and warmup_tick): the opponent's fighter once they locked in, else the one they played last game,
// played by that character's pro AI (Fight a pro: Phillip's policy when installed, else the pro's habit table) when there is
// one; the line names it ("Zain AI", "Marth CPU"). The player's own Practice CPU comes back when the warm-up ends. When every other player has locked in:
// "Opponent is ready", three seconds, then back to the select to lock in. L and R held in Training go back at once. If the
// connection is lost meanwhile the player stays where they are. Upstream also tells its own builds "warming up" (message 0xE9,
// shown on their select); a Slippi player just sees this player not locked in yet, as when someone is slow to pick, so it is
// not sent here.
//
// Training after online (upstream's keys): online_training_after 1: when a Direct or Teams session ends (the other player left,
// Z held, the connection dropped) after at least one match, straight into a Training match with this player's fighter against
// the last opponent's fighter (Unranked excluded, as upstream: it ends a session after each game there). online_return_training
// 1: to Training's character select instead, in every mode. Both off by default, as upstream.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "features_internal.h"
#include "frontend.h"
#include "netplay_bridge.h"                // runner: search_info, keep_search (patches/net-0005)

#include <algorithm>
#include <atomic>
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
int keep_mode = 0;                           // keep_search: 0 ask, 1 always keep searching, 2 never (upstream's values)
int quickchat = 0;                           // 0 on, 1 Direct only, 2 off (Slippi's and upstream's quickchat)
bool pick_previews = true;
int applied_chat = -1; int applied_previews = -1;

uint64_t now_ms() { return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count()); }

enum class Stage { Off, Away, Found, Going };
struct Kept {
  Stage stage = Stage::Off;
  bool warmup = false;                       // the warm-up room (a connection kept), not a search
  int mode = 1;                              // the online mode to go back to
  std::string code;                          // Direct, Teams: the connect code searched for
  int cpu = -1;                              // warm-up: the sparring partner's fighter (external id), -1 Training's own
  bool spar_applied = false; uint32_t spar_ticks = 0; std::string spar;   // warm-up: the pro put in, the line's name
  int character = -1, color = 0, port = 0;   // the locked-in pick and the controller that held L+R
  uint64_t since_ms = 0, found_ms = 0, going_ms = 0, safe_wait_ms = 0;
  uint32_t via_menu = 0;                     // the retrace the way back reached the main menu
  bool left_online = false;
  std::string opponent;
} kept;
uint32_t hold = 0; bool armed = false;
uint32_t prev_major = 0xFFFFFFFF, prev_minor = 0xFFFFFFFF, steady = 0;

std::mutex hud_mutex;
std::string hud_text, note_text, prompt_text;
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
  else if (key == "keep_search") keep_mode = std::clamp(std::atoi(value.c_str()), 0, 2);
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

const char* mode_label(int mode) { return mode == 3 ? "Teams" : mode == 2 ? "Direct" : mode == 4 ? "Party" : "Unranked"; }
const char* const kFighters[26] = {"Falcon", "DK", "Fox", "Game & Watch", "Kirby", "Bowser", "Link", "Luigi", "Mario", "Marth", "Mewtwo",
    "Ness", "Peach", "Pikachu", "Ice Climbers", "Puff", "Samus", "Yoshi", "Zelda", "Sheik", "Falco", "Young Link", "Dr. Mario", "Roy", "Pichu",
    "Ganondorf"};

void spar_end() {
  if (kept.spar_applied) { proai_spar_end(); say("search: warm-up: the player's own Practice CPU is back"); }
  kept.spar_applied = false; kept.spar.clear(); kept.spar_ticks = 0;
}

void end(const std::string& why, bool cleanup) {
  say("search: kept search over: %s", why.c_str());
  spar_end();
  if (cleanup) netplay_bridge::end_kept_search(why.c_str());
  else netplay_bridge::keep_search(false);
  kept = Kept{};
}

void go_back(const char* why) {
  say("search: back to the %s select (%s) from scene %u/%u at retrace %u", kept.warmup ? "online" : mode_label(kept.mode), why, scene_major(), scene_minor(), retrace());
  spar_end();
  kept.stage = Stage::Going;
  kept.going_ms = now_ms();
  // The character select starts from the locked-in fighter (upstream write_event_backup), in Unranked.
  if (MuGameApi* g = game(); g && g->practice && kept.character >= 0) {
    int32_t a[3] = {kept.character, kept.color, kept.port};
    g->practice(MU_PRACTICE_SET_EVENT_BACKUP, a, 3);
  }
  set_online_mode(kept.mode);
  // From a match scene through the main menu first, as upstream's Source Port goes back.
  if (scene_major() == kOnlineMajor) return;
  kept.via_menu = 0;
  request_major(kMainMenuMajor);
}

void begin(const netplay_bridge::SearchInfo& s, int port) {
  kept = Kept{};
  kept.stage = Stage::Away;
  kept.mode = s.mode; kept.code = s.code;
  kept.character = s.character; kept.color = s.color; kept.port = port;
  kept.since_ms = now_ms();
  netplay_bridge::keep_search(true);
  if (!training_go(s.character, s.color, -1, 0, true)) { end("the game could not be asked for Training", true); return; }
  say("search: kept search: Training with fighter %d (costume %d) while the %s search%s%s goes on (%u s searched so far)", s.character, s.color,
      mode_label(s.mode), s.code.empty() ? "" : " for ", s.code.c_str(), s.elapsed_ms / 1000);
}

// keep_search: the player leaves the select while searching and keeps the search: the menus, as upstream's main menu takes it.
void begin_menus(const netplay_bridge::SearchInfo& s, int port, const char* why) {
  kept = Kept{};
  kept.stage = Stage::Away;
  kept.mode = s.mode; kept.code = s.code;
  kept.character = s.character; kept.color = s.color; kept.port = port;
  kept.since_ms = now_ms();
  netplay_bridge::keep_search(true);
  request_major(kMainMenuMajor);
  say("search: kept search: %s; the %s search goes on from the menus (%u s searched so far)", why, mode_label(s.mode), s.elapsed_ms / 1000);
}

bool unlocked_offline(int character) {
  MuGameApi* g = game();
  int32_t a[1] = {0};
  if (!g || !g->practice || g->practice(MU_PRACTICE_UNLOCKED, a, 1) != 0) return true;   // unknown: let the game decide
  return (uint32_t(a[0]) >> character & 1u) != 0;
}

void begin_warmup(const netplay_bridge::SearchInfo& s, int port, int character, int color, int last_opponent, int last_opponent_color) {
  kept = Kept{};
  kept.stage = Stage::Away;
  kept.warmup = true;
  kept.mode = s.mode;
  kept.character = character; kept.color = color; kept.port = port;
  kept.since_ms = now_ms();
  kept.opponent = s.opponent;
  // The sparring partner (upstream's warmup_enter): the opponent's pick once locked in, else the fighter they played last game.
  int cpu = s.remote_character, cpu_color = s.remote_color;
  if (cpu < 0) { cpu = last_opponent; cpu_color = last_opponent_color; }
  if (cpu >= 26) cpu = -1;
  if (cpu >= 0 && !unlocked_offline(cpu)) { say("search: warm-up: fighter %d is not unlocked offline; Training keeps its own CPU", cpu); cpu = -1; }
  if (cpu >= 0 && cpu == character && cpu_color == color) cpu_color = (cpu_color + 1) % 4;   // two players cannot wear the same colours
  kept.cpu = cpu;
  netplay_bridge::keep_search(true);
  if (!training_go(character, color, cpu, cpu_color, true)) { netplay_bridge::keep_search(false); kept = Kept{}; return; }
  say("search: warm-up: Training with fighter %d against %s %d while the %s connection stays up", character, cpu >= 0 ? "the opponent's fighter" : "Training's own CPU,",
      cpu, s.mode == 3 ? "Teams" : s.mode == 2 ? "Direct" : "Unranked (test)");
}

// ---- search again (upstream's g_offer_*), and the keep-search question: the pads ----
struct Offer { int mode = -1; std::string code; int character = -1, color = 0; bool shown = false; } offer;
bool asking = false;
enum class Pending { None, Keep, Stop, Again };
std::atomic<int> pending{0};
std::atomic<int> pending_port{0};
netplay_bridge::SearchInfo last_info;          // as on_retrace saw it (the pad hook runs inside the next frame, same thread)
uint16_t was_buttons[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};

// Tests only: --feature-test unranked-as-direct treats Unranked as Direct for the warm-up and online_training_after (the loopback
// stand-in pairs Unranked searches; Direct needs a connect code typed on the name entry screen).
bool unranked_as_direct() { static const bool on = feature_test("unranked-as-direct"); return on; }

// ---- Training after online: the session watch ----
struct Session {
  bool connected = false;
  int mode = -1, character = -1, color = 0;
  std::string code;
  int opponent = -1, opponent_color = 0;
  uint32_t matches = 0;
  bool was_fighting = false;
} session;

void watch_session(const Retrace& info, const netplay_bridge::SearchInfo& s) {
  const bool connected = s.state == 4 && (!s.kept || kept.warmup);   // a warm-up keeps the session; a kept search is none yet
  if (connected) {
    session.connected = true;
    session.mode = s.mode;
    if (!s.code.empty()) session.code = s.code;
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
  const bool into_training = training_after && (ended.mode != kUnranked || unranked_as_direct()) && ended.character >= 0;
  if (into_training) {
    say("search: online session over after %u match(es) (mode %d); into Training with fighter %d against fighter %d (online_training_after)", ended.matches,
        ended.mode, ended.character, ended.opponent);
    training_go(ended.character, ended.color, ended.opponent, ended.opponent_color, true);
  } else if (return_training) {
    say("search: online session over after %u match(es) (mode %d); to Training's character select (online_return_training)", ended.matches, ended.mode);
    request_major(kTrainingMajor);
  }
  // "Search again?" once the Training match runs (upstream's enter_training_after_session sets it; Direct and Teams: the same code).
  if ((into_training || return_training) && ended.mode >= 1 && ended.mode <= 3 && ended.character >= 0 && (ended.mode == kUnranked || !ended.code.empty())) {
    offer = Offer{};
    offer.mode = ended.mode; offer.code = ended.code; offer.character = ended.character; offer.color = ended.color;
  }
}

// The keep-search question can be asked: on the select, a search going out, locked in, Unranked/Direct/Teams.
bool question_possible(const netplay_bridge::SearchInfo& s, uint32_t major, uint32_t minor) {
  return kept.stage == Stage::Off && keep_mode != 2 && major == kOnlineMajor && minor == 0 && (s.state == 1 || s.state == 2) && s.locked &&
         s.mode >= kUnranked && s.mode <= 3 && !replay_playing();
}

// "Search again?" answered with D-pad Up: the same search, kept from the start, so the player trains until it finds someone.
void search_again() {
  if (offer.mode < 0) return;
  const Offer o = offer;
  offer = Offer{};
  if (!netplay_bridge::request_match(o.mode, o.code, uint8_t(o.character), uint8_t(o.color))) { note("The search could not start", 300); return; }
  kept = Kept{};
  kept.stage = Stage::Away;
  kept.mode = o.mode; kept.code = o.code;
  kept.character = o.character; kept.color = o.color; kept.port = pending_port.load();
  kept.since_ms = now_ms();
  kept.left_online = true;
  netplay_bridge::keep_search(true);
  say("search: search again (%s%s%s) from Training with fighter %d (costume %d)", mode_label(o.mode), o.code.empty() ? "" : " for ", o.code.c_str(),
      o.character, o.color);
}

void offer_tick(uint32_t major, uint32_t minor) {
  if (offer.mode < 0) return;
  if (major == kOnlineMajor) { offer = Offer{}; return; }        // a new online session: the offer is stale
  if (!offer.shown && major == kTrainingMajor && minor == 2 && kept.stage == Stage::Off) {
    offer.shown = true;
    say("search: Training is running; asking whether to search %s again", mode_label(offer.mode));
  } else if (offer.shown && (major != kTrainingMajor || minor != 2 || kept.stage != Stage::Off)) {
    say("search: the search-again offer went away (left Training)");
    offer = Offer{};
  }
}

void on_retrace(const Retrace& info) {
  const uint32_t major = info.scene_major, minor = info.scene_minor;
  steady = major == prev_major && minor == prev_minor ? steady + 1 : 0;
  prev_major = major; prev_minor = minor;
  const netplay_bridge::SearchInfo s = netplay_bridge::search_info();
  if (s.state < 0) return;                                          // no netplay object running
  if (applied_chat != quickchat || applied_previews != int(pick_previews)) {
    applied_chat = quickchat; applied_previews = pick_previews;
    netplay_bridge::set_chat(quickchat, pick_previews);
    say("search: quick chat %s, live pick previews %s", quickchat == 0 ? "on" : quickchat == 1 ? "in Direct only" : "off", pick_previews ? "on" : "off");
  }
  watch_session(info, s);
  last_info = s;
  // What the pad hook was answered (the keep-search question, the search-again offer): done here, between frames.
  switch (Pending(pending.exchange(0))) {
    case Pending::Keep:
      if (question_possible(s, major, minor)) begin_menus(s, pending_port.load(), "the player kept searching and left the select");
      break;
    case Pending::Stop:
      say("search: the player stopped the search and left the select");
      request_major(kMainMenuMajor);              // the main menu's own cleanup stops it
      break;
    case Pending::Again: search_again(); break;
    case Pending::None: break;
  }
  if (asking && !question_possible(s, major, minor)) { asking = false; say("search: the keep-searching question went away (the search changed)"); }
  offer_tick(major, minor);
  {
    std::string prompt;
    if (asking) prompt = "KEEP SEARCHING WHILE YOU PLAY?   A: Yes, keep my place   B: No, stop searching";
    else if (offer.shown)
      prompt = std::string("Search ") + mode_label(offer.mode) + (offer.code.empty() ? "" : " for " + offer.code) + " again?   D-pad Up: search   Down: keep training";
    std::lock_guard<std::mutex> lock(hud_mutex);
    prompt_text = prompt;
  }
  int port = 0;
  const bool lr = lr_held(&port);
  if (!lr) armed = true;
  hold = lr && armed ? hold + 1 : 0;
  const uint64_t now = now_ms();

  if (kept.stage == Stage::Off) {
    // On the select, searching, locked in, Unranked: the hold keeps the search and goes to Training.
    const bool possible = play_while_searching && major == kOnlineMajor && minor == 0 && (s.state == 1 || s.state == 2) && s.locked &&
                          s.mode >= kUnranked && s.mode <= 3 && s.character >= 0 && !replay_playing();
    const bool test_unranked = unranked_as_direct();
    const bool warm = warmup && major == kOnlineMajor && minor == 0 && s.state == 4 && (s.mode == 2 || s.mode == 3 || (test_unranked && s.mode == kUnranked)) &&
                      !s.locked && !replay_playing();
    if (possible && hold == kHold) { hold = 0; armed = false; begin(s, port); }
    else if (warm && hold == kHold) { hold = 0; armed = false; begin_warmup(s, port, session.character, session.color, session.opponent, session.opponent_color); }
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
      spar_end();
      kept = Kept{};
      return;
    }
  }
  // The warm-up's sparring partner, once the Training match runs (upstream's warmup_tick): the pro who plays the CPU's fighter.
  if (kept.warmup && kept.stage == Stage::Away && major == kTrainingMajor && minor == 2 && kept.cpu >= 0 && kept.spar.empty() && ++kept.spar_ticks == 30) {
    const int pro = proai_for_character(kept.cpu);
    std::string label;
    if (pro >= 0 && proai_spar_begin(pro, &label)) {
      kept.spar_applied = true; kept.spar = label;
      say("search: warm-up: %s plays the CPU (fighter %d)", label.c_str(), kept.cpu);
    } else {
      kept.spar = std::string(kFighters[kept.cpu]) + " CPU";
      say("search: warm-up: the CPU is %s (no pro AI for fighter %d)", kept.spar.c_str(), kept.cpu);
    }
  }
  if (kept.warmup && major != kTrainingMajor && !kept.spar.empty()) spar_end();   // out of Training: the player's own CPU until the next one
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
      std::snprintf(line, sizeof line, "Warming up%s%s: %s is still picking  %u:%02u   (hold L+R to go back)", kept.spar.empty() ? "" : " with ", kept.spar.c_str(),
                    kept.opponent.empty() ? "your opponent" : kept.opponent.c_str(), away / 60, away % 60);
    else if (kept.stage == Stage::Found) std::snprintf(line, sizeof line, "Opponent is ready: back to the select in %llu", (unsigned long long)left);
    else std::snprintf(line, sizeof line, "Going back to the select");
  } else if (kept.stage == Stage::Away && kept.mode == kUnranked)
    std::snprintf(line, sizeof line, "Searching for an Unranked opponent  %u:%02u   (hold L+R to go back)", secs / 60, secs % 60);
  else if (kept.stage == Stage::Away)
    std::snprintf(line, sizeof line, "Searching %s for %s  %u:%02u   (hold L+R to go back)", mode_label(kept.mode), kept.code.empty() ? "your code" : kept.code.c_str(),
                  secs / 60, secs % 60);
  else if (kept.stage == Stage::Found)
    std::snprintf(line, sizeof line, "Opponent found%s%s: back to the match in %llu", kept.opponent.empty() ? "" : ": ", kept.opponent.c_str(), (unsigned long long)left);
  else std::snprintf(line, sizeof line, "Opponent found: going back to the match");
  std::lock_guard<std::mutex> lock(hud_mutex);
  hud_text = line;
}

void hud(std::vector<HudLine>& out) {
  std::lock_guard<std::mutex> lock(hud_mutex);
  if (!prompt_text.empty()) out.push_back({prompt_text, 0xFFE070FFu});
  if (!hud_text.empty()) out.push_back({hud_text, kept.stage == Stage::Away ? 0xFFC850FFu : 0x80FF90FFu});
  if (!note_text.empty() && retrace() < note_until) out.push_back({note_text, 0xFF8080FFu});
}

std::string on_off(bool v) { return v ? "On" : "Off"; }
}  // namespace

// The keep-search question's answers and the search-again offer's D-pad (options.cpp pad_read, on the simulation thread inside
// the frame): what the player pressed this frame. The game never sees the buttons these take (upstream's kept_select_pads and
// its offer handling in apply_pads). Nothing is taken in a match.
void search_pad_hook(MuPadStatus pads[4]) {
  constexpr uint16_t kA = 0x0100, kB = 0x0200, kX = 0x0400, kStart = 0x1000, kZ = 0x0010, kUp = 0x0008, kDown = 0x0004;
  uint16_t fresh[4] = {}, any = 0;
  int first = -1;
  for (int i = 0; i < 4; ++i) {
    const uint16_t now = pads[i].err == 0 ? pads[i].button : 0;
    fresh[i] = uint16_t(now & ~was_buttons[i]);
    was_buttons[i] = now;
    if (fresh[i] && first < 0) first = i;
    any |= fresh[i];
  }
  const auto strip = [&](uint16_t bits) { for (int i = 0; i < 4; ++i) pads[i].button &= uint16_t(~bits); };
  if (asking) {
    if (any & kZ) {                                // Z still stops the search on the select: the question goes, Z reaches the game
      asking = false;
      strip(kA | kB | kX | kStart);
      say("search: Z while asked: the question goes, Z stops the search as always");
      return;
    }
    strip(kA | kB | kX | kStart | kZ);             // while asked, the game sees none of these
    if (any & kA) { asking = false; pending_port = first < 0 ? 0 : first; pending = int(Pending::Keep); }
    else if (any & kB) { asking = false; pending = int(Pending::Stop); }
    return;
  }
  if (offer.shown) {
    for (int i = 0; i < 4; ++i) {
      if (pads[i].err != 0 || (pads[i].button & uint16_t(~(kUp | kDown))) != 0) continue;   // Up or Down on its own only
      const uint16_t f = fresh[i] & (kUp | kDown);
      pads[i].button &= uint16_t(~(kUp | kDown));
      if ((f & kUp) && pending.load() == 0) { offer.shown = false; pending_port = i; pending = int(Pending::Again); say("search: search again from the offer"); }
      else if (f & kDown) { offer = Offer{}; say("search: the search-again offer dismissed"); }
    }
    return;
  }
  if ((any & kB) && question_possible(last_info, scene_major(), scene_minor())) {
    strip(kB);
    if (keep_mode == 0) { asking = true; say("search: B on the select while searching: asking whether to keep searching"); }
    else { pending_port = first < 0 ? 0 : first; pending = int(Pending::Keep); }
  }
}

void register_search() {
  add_setting(setting);
  add_retrace(on_retrace);
  add_hud(hud);
  add_menu_entry({"Online", "Play while searching (hold L+R on the select)", [] { return on_off(play_while_searching); },
                  [](int) { play_while_searching = !play_while_searching; }, nullptr,
                  [](MenuEntry::Write w) { w("play_while_searching", play_while_searching ? "1" : "0"); }});
  add_menu_entry({"Online", "Leaving the select while searching", [] { return std::string(keep_mode == 0 ? "Ask me" : keep_mode == 1 ? "Keep searching" : "Stop the search"); },
                  [](int d) { keep_mode = (keep_mode + (d < 0 ? 2 : 1)) % 3; }, nullptr,
                  [](MenuEntry::Write w) { w("keep_search", std::to_string(keep_mode)); }});
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
