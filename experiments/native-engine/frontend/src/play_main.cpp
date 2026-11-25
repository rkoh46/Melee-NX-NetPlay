// mu-native-gfx --play: the native engine for a person. No script, no reference digest, no frame
// limit: the game boots to its own menus, reads the app's controller input and plays its sound,
// until the player holds L + R + Start or closes the program. The scripted runner (runner_main.cpp)
// is not involved, so the exactness runs are unchanged.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"
#include "feature_calls.h"

#include "host_services.h"
#include "mu_elf_bridge.h"
#include "fixedmap.h"
#include "hostprof.h"
#include "net_host.h"
#include "netplay_bridge.h"
#include "rollback.h"
#include "mu_host.h"

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdarg>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <string>
#include <vector>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#ifdef __SWITCH__
#include <switch.h>
#include "switch_clocks.h"
#endif

extern "C" const uint8_t mu_embedded_game[], mu_embedded_game_end[];

namespace fe::mu_ui { __attribute__((weak)) void note_session(bool fighting, bool online); }
namespace fe {
std::string home_replay;   // a replay the home screen (mu_home.cpp) asked to watch: the game starts with it
namespace {
using Clock = std::chrono::steady_clock;
inline uint64_t now_ns() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()); }

net_host::Options net;
bool net_on = false;          // the netplay host answers the game's online commands
bool direct = false;          // --net or --online on the command line: straight into a match, the runner's game, seed and clock
bool menu_online = false;     // the services program with Slippi's menus and a user file: a search starts from the game's own menu
bool searching = false;       // between the game's "find a match" and its "clean the connection up" (simulation thread)
bool online_now = false, in_match = false, tracked = false;
std::atomic<bool> online_flag{false};   // online_now for other threads (the menu and the HUD)
std::vector<std::string> restart_args;   // a feature module asked for the game to start again with these
uint32_t last_match_frame = 0, match_count = 0, scene_major = 0xFFFFFFFF, scene_minor = 0;
Clock::time_point last_retrace; bool have_last_retrace = false;

// The option words as the game last read them (the host's functions are wrapped in the tables hook).
enum { kWords = 6 };
const char* const kWordName[kWords] = {"options", "options 2", "options 3", "display", "audio", "mod flags"};
uint32_t (*word_original[kWords])() = {};
uint32_t word_seen[kWords] = {};
bool word_read[kWords] = {};
template <int N> uint32_t word_reader() { const uint32_t v = word_original[N] ? word_original[N]() : 0; word_seen[N] = v; word_read[N] = true; return v; }

// Once per retrace after the frame, on the simulation thread.
uint32_t trace_retrace = 0;   // the command trace's retrace column
void observer(const MuStatePod& state, const linux_host::Counters& counters, void*) {
  trace_retrace = counters.retraces;
  const auto now = Clock::now();
  const bool fighting = state.match_frame && state.match_frame != last_match_frame;
  last_match_frame = state.match_frame;
  log_queue_match(fighting);
  if (state.scene_major != scene_major || state.scene != scene_minor) {
    scene_major = state.scene_major; scene_minor = state.scene;
    in_match = false;
    say("play: scene %u/%u at retrace %u", scene_major, scene_minor, counters.retraces);
    // The match of a menu session is over (the game left its scene): the rollback store stops watching the game's
    // memory. Its list of written pages only grows; the next match's first state save starts it again from nothing.
    if (tracked && !direct) { net_host::stop_tracking(); tracked = false; say("play: rollback store released"); }
  }
  // Online, as upstream's host decides it: the game is in Slippi's online flow (major scene 8), or a search or
  // match of the netplay host is on. The option words follow (settings.cpp).
  // A kept search (the player went to Training while it runs, features/search.cpp) is no online session for the game: it plays
  // offline with the player's own option words until the search brings it back to the online screens (upstream: search_away).
  if (searching && !direct && state.scene_major != 8) {
    // A kept search that ended while the player was away (features/search.cpp: it stopped, or could not come back) was
    // cleaned up by the netplay object itself, not by the game: the session is over.
    const netplay_bridge::SearchInfo s = netplay_bridge::search_info();
    if (!s.kept && s.state <= 0) { searching = false; say("play: online: the search or session is over (the netplay object closed it; scene %u/%u)", state.scene_major, state.scene); }
  }
  const bool online = net_on && (direct || (searching && !netplay_bridge::search_kept()) || state.scene_major == 8);
  if (online != online_now) {
    online_now = online;
    online_flag.store(online, std::memory_order_relaxed);
    if (!direct) {
      settings_online(online);
      say("play: %s at retrace %u (scene %u/%u): the option words are now the %s ones", online ? "online" : "offline again", counters.retraces,
          state.scene_major, state.scene, online ? "online" : "player's");
    }
  }
  if (fighting && !in_match) {
    in_match = true; ++match_count;
    char words[200]; int n = 0;
    for (int i = 0; i < kWords; ++i) n += std::snprintf(words + n, sizeof words - n, "%s%s %08X%s", i ? ", " : "", kWordName[i], word_seen[i], word_read[i] ? "" : " (not read)");
    say("play: match %u starts at retrace %u (scene %u/%u, %s); the words the game last read: %s", match_count, counters.retraces, state.scene_major, state.scene,
        online ? "online" : "offline", words);
  }
  // The feature modules (feature_hooks.h) and the app's own per-retrace work (app.cpp).
  {
    features::Retrace info;
    info.retrace = counters.retraces; info.scene_major = state.scene_major; info.scene_minor = state.scene;
    info.match_frame = state.match_frame; info.fighting = fighting; info.online = online; info.state = &state;
    features::call_retrace(info);
  }
  app_retrace(counters.retraces, fighting, online);
  if (&mu_ui::note_session) mu_ui::note_session(fighting, online);   // mu_panel.cpp (builds with a picture)
  // The Replays page's "Watch it": the game is started again with the arguments the module returns.
  if (restart_args.empty() && features::call_take_restart(&restart_args)) {
    say("play: the game starts again with %zu more argument(s) (asked for by a feature module)", restart_args.size());
    linux_host::request_stop(0);
  }
  if (net_on) {
    // The state row the netplay host checks and records, and the frame's wall time (it takes the pacing wait out).
    char row[1400]; int n = 0;
    n += std::snprintf(row + n, sizeof row - n, "%u,%u,%u,%u,%u", counters.retraces, state.scene_major, state.scene, state.match_frame, state.rng);
    for (const auto& f : state.player)
      n += std::snprintf(row + n, sizeof row - n, ",%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u", f.present, f.stocks, f.action, f.anim_frame,
                         f.pos_x, f.pos_y, f.pos_z, f.vel_x, f.vel_y, f.vel_z, f.percent, f.facing);
    row[n++] = '\n';
    net_host::on_retrace(row, size_t(n), fighting, have_last_retrace ? std::chrono::duration<double, std::milli>(now - last_retrace).count() : -1.0);
  }
  last_retrace = Clock::now(); have_last_retrace = true;
}

// The host hands its tables over before the game sees them: netplay takes slippi_command and its other entries;
// the mod commands (frontend/src/mods.cpp) are asked first, as in the hook of the host services.
int32_t (*net_command)(uint8_t, const uint8_t*, uint32_t, uint8_t*, uint32_t, uint32_t*) = nullptr;
void (*resim_original)(int32_t) = nullptr;
extern "C" __attribute__((weak)) int32_t mu_frontend_command(uint8_t, const uint8_t*, uint32_t, uint8_t*, uint32_t, uint32_t*);
int32_t command_inner(uint8_t c, const uint8_t* payload, uint32_t size, uint8_t* response, uint32_t capacity, uint32_t* response_size) {
  if (response_size) *response_size = 0;
  if (mu_frontend_command) { const int32_t mine = mu_frontend_command(c, payload, size, response, capacity, response_size); if (mine != INT32_MIN) return mine; }
  if (!net_command) return -1;
  // A state save (0xB1) or load (0xB2), and the self-test's two: the snapshot store copies pages of game memory and,
  // on the console, takes them out of the address space for a moment to make them read-only again. The decoder
  // thread reads that memory while it has stream bytes left, so it is brought up to date first. At the start of a
  // frame it already is (the retrace waited for it), so this costs nothing unless frames are being repeated.
  const bool store = c == 0xB1 || c == 0xB2 || c == 0xF0 || c == 0xF1;
  const uint64_t t0 = now_ns();
  if (store) { decoder_sync(); tracked = true; }
  const uint64_t t1 = now_ns();
  if (c == 0xB4 && payload && size >= 1) {
    static const char* const mode[] = {"Ranked", "Unranked", "Direct", "Teams", "Party"};
    searching = true;
    say("play: online: the game asked for a match: mode %u (%s)%s", payload[0], payload[0] < 5 ? mode[payload[0]] : "?", direct ? "" : ", from its menu");
  } else if (c == 0xBA && searching && !netplay_bridge::search_kept()) {
    // A search the player chose to keep while they play elsewhere (features/search.cpp, patches/net-0005) survives the game's
    // cleanups: the netplay object skips them, and the session stays a search.
    searching = false;
    say("play: online: the game closed the connection");
  }
  const int32_t result = net_command(c, payload, size, response, capacity, response_size);
  if (result == 0 && response && response_size) features::call_reply(c, response, *response_size);   // read only (overlay names)
  features::call_exchange(c, payload, size, response, response_size ? *response_size : 0, result, in_resim);   // read only (session trace)
  const uint64_t t2 = now_ns();
  if (store) { sim_cost[1] += t1 - t0; sim_cost[0] += t2 - t1; } else sim_cost[2] += t2 - t0;
  return result;
}
// MELEE_TRACE_SLIPPI_CMDS=1 (a test aid, upstream hle/slippi_online.cpp's command trace): one line per menu or online command
// with its payload and reply (FNV-1a of the reply), runs of identical lines collapsed into one with a count; the per-frame
// streams (0xB0-0xB2) and the pick previews are left out. Read only: nothing the game gets changes.
bool trace_cmds() { static const bool on = [] { const char* v = std::getenv("MELEE_TRACE_SLIPPI_CMDS"); return v && v[0] == '1'; }(); return on; }
struct CmdTrace { std::string key; uint32_t count = 0, first_retrace = 0; uint64_t seq = 0; } cmd_trace;
uint64_t cmd_seq = 0;
void flush_cmd_trace() {
  if (cmd_trace.count > 1) say("slippi-cmd: seq %llu retrace %u %s (x%u)", (unsigned long long)cmd_trace.seq, cmd_trace.first_retrace, cmd_trace.key.c_str(), cmd_trace.count);
}
int32_t command(uint8_t c, const uint8_t* payload, uint32_t size, uint8_t* response, uint32_t capacity, uint32_t* response_size) {
  const int32_t result = command_inner(c, payload, size, response, capacity, response_size);
  if (!trace_cmds() || c == 0xB0 || c == 0xB1 || c == 0xB2 || c == 0xED) return result;
  char buf[16];
  std::string key;
  std::snprintf(buf, sizeof buf, "cmd %02X len %u", c, size); key += buf;
  key += " payload ";
  const uint32_t shown = payload ? std::min<uint32_t>(size, 64) : 0;
  for (uint32_t i = 0; i < shown; ++i) { std::snprintf(buf, sizeof buf, "%02X", payload[i]); key += buf; }
  if (shown < size) key += "..";
  const uint32_t got = result == 0 && response && response_size ? *response_size : 0;
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < got; ++i) { h ^= response[i]; h *= 16777619u; }
  char tail[64];
  std::snprintf(tail, sizeof tail, " reply %u fnv %08X%s", got, result == 0 ? h : 0u, result == 0 ? "" : " unhandled");
  key += tail;
  ++cmd_seq;
  if (cmd_trace.count && key == cmd_trace.key) { ++cmd_trace.count; return result; }
  flush_cmd_trace();
  cmd_trace = {key, 1, trace_retrace, cmd_seq};
  say("slippi-cmd: seq %llu retrace %u %s", (unsigned long long)cmd_seq, cmd_trace.first_retrace, key.c_str());
  return result;
}
void resim_phase(int32_t entering) {
  in_resim = entering != 0;
  if (resim_original) resim_original(entering);
}
void tables_hook(MuHostApi* host, MuGameApi* game) {
  features::call_tables(host, game);   // first, before anything below wraps the table; also for the game's table (host null)
  if (host) {
    if (net_on) {
      const auto own_test_match = host->online_test_match;
      net_host::install(host);
      // With the menus, nothing asks for a match at boot: the game's own online screens do, through slippi_command.
      if (menu_online) host->online_test_match = own_test_match;
      net_command = host->slippi_command; host->slippi_command = command;
      resim_original = host->resim_phase; host->resim_phase = resim_phase;
    }
    uint32_t (**slot[kWords])() = {&host->game_options, &host->game_options2, &host->game_options3, &host->display_options, &host->audio_options, &host->mod_flags};
    uint32_t (*const reader[kWords])() = {word_reader<0>, word_reader<1>, word_reader<2>, word_reader<3>, word_reader<4>, word_reader<5>};
    for (int i = 0; i < kWords; ++i) if (*slot[i]) { word_original[i] = *slot[i]; *slot[i] = reader[i]; }
  }
  if (options.profile) hostprof::wrap_tables(host, game);
}
void net_say(const char* format, ...) {
  char line[2048];
  va_list args; va_start(args, format); std::vsnprintf(line, sizeof line, format, args); va_end(args);
  say("%s", line);
}
// The disc header a vanilla-only session asks for: game id GALE01, revision 2 (upstream also compares every file; the
// mod layers here are separate files, never the disc itself, so the header is what can differ).
bool retail_disc(const std::string& path) {
  unsigned char h[8] = {};
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  const bool read = std::fread(h, 1, sizeof h, f) == sizeof h;
  std::fclose(f);
  return read && std::memcmp(h, "GALE01", 6) == 0 && h[7] == 2;
}
bool file_exists(const std::string& path) { if (FILE* f = std::fopen(path.c_str(), "rb")) { std::fclose(f); return true; } return false; }

struct Run { int result = 1; };
void* play_thread(void* user) {
  auto* run = static_cast<Run*>(user);
  linux_host::Config config;
  config.iso_path = options.iso;
  config.game_path = "(embedded)";
  config.log_path = options.host_log_path.empty() ? options.out_dir + "/host.log" : options.host_log_path;
  // Kept between sessions, apart from the scripted runs' card; a mod profile has its own.
  // No mod files in a session that can go online (upstream switches to the retail files for the match; here they are not loaded).
  if (net_on && !direct && !settings_profile().empty())
    say("play: mod profile \"%s\" is not loaded: this session can go online (slippi_menus 0 plays it offline)", settings_profile().c_str());
  // A vanilla-only session (upstream's --vanilla-only): no mod profile, and its own card folder inside the ordinary
  // one, so neither a modded save (20XX TE carries code) nor this session's saves cross over.
  const bool vanilla_only = settings_vanilla_only() && !direct;
  if (vanilla_only && !settings_profile().empty()) say("play: vanilla-only session: mod profile \"%s\" is not loaded", settings_profile().c_str());
  const std::string profile_card = direct || vanilla_only ? std::string() : mods_configure(net_on ? std::string() : settings_profile());
  // A session that can go online: skins in their online form, and of a mod profile only its look-only costumes (cosmetics.cpp).
  if (net_on) { mods_session_can_go_online(); if (!direct && !vanilla_only && !settings_profile().empty()) mods_configure_costumes(settings_profile()); }
  config.card_dir = !profile_card.empty() ? profile_card : !options.card_dir.empty() ? options.card_dir : options.out_dir + "/card-play";
  if (options.app && profile_card.empty() && !direct && !vanilla_only) app_backup_card();   // the earlier app's card, copied once before it is used
  if (vanilla_only) {
    mkdir(config.card_dir.c_str(), 0777);
    config.card_dir += "/VanillaOnly";
    say("play: vanilla-only session: retail gameplay, memory card folder %s", config.card_dir.c_str());
  }
  // The Lossless adapter's lag test (upstream --lag-test or the saved switch lagtestnext, which arms one start only): the game's
  // shim/mu_lagtest.c reads MELEE_LAG_TEST (develop mode, boot to the character select, its default rules on Dream Land, a
  // 10-second rumble when the select loads, X held a white and A held a black background for the adapter's photodiode), and the
  // session gets an empty memory card folder of its own, as upstream's (its app runs Dolphin with no card). Offline only.
  const bool lag_test_asked = settings_lag_test_take() || std::find(options.rest.begin(), options.rest.end(), "--lag-test") != options.rest.end();
  if (lag_test_asked && !direct && !vanilla_only && !net_on) {
    setenv("MELEE_LAG_TEST", "1", 1);
    config.card_dir = options.out_dir + "/LagTestCard";
    options.fresh_card = true;
    say("play: Lossless adapter lag test: this start only (develop mode, boot to the character select, rumble on its load, X white / A black background); memory card folder %s", config.card_dir.c_str());
  } else {
    if (lag_test_asked) say("play: the Lossless adapter lag test is offline only and not in a vanilla-only session: not started");
    unsetenv("MELEE_LAG_TEST");
  }
  config.max_frames = options.frames;                     // 0: until the player stops it
  // The frontend paces the retrace; with netplay the host does (its clock is what the netplay host moves).
  config.pace = net_on;
  if (net_on) options.pace = false;
  config.headless = true; config.no_gc_adapter = true;    // the host's own SDL2/libusb input is not built in
  config.vanilla_game = settings_vanilla();               // by default melee-unlocked's game as its own hosts run it
  if (direct) {
    // As the runner's netplay runs: the retail game, the same seed and clock on both sides. No settings file, no mods.
    config.vanilla_game = true;
    config.has_rng_seed = true; config.rng_seed = 0x12345678;
    config.has_time_base = true; config.time_base = 0;
    options.fresh_card = true;
  }
  if (!direct) {
    // The game's calendar from the real clock (upstream source_host.cpp h_boot_time: saves and Training Mode CE exports were
    // dated January 1, 2000 at zero). The console's clock keeps local wall time, read as UTC as upstream does. On the Switch
    // newlib's time() is the network/user clock in UTC with no time zone, so the local time comes from the time service.
    int64_t local_seconds = -1;
#ifdef __SWITCH__
    u64 now = 0; TimeCalendarTime cal{}; TimeCalendarAdditionalInfo info{};
    if (R_SUCCEEDED(timeGetCurrentTime(TimeType_Default, &now)) && R_SUCCEEDED(timeToCalendarTimeWithMyRule(now, &cal, &info))) {
      std::tm t{}; t.tm_year = cal.year - 1900; t.tm_mon = cal.month - 1; t.tm_mday = cal.day; t.tm_hour = cal.hour; t.tm_min = cal.minute; t.tm_sec = cal.second;
      local_seconds = int64_t(timegm(&t));
    }
#else
    const std::time_t now = std::time(nullptr); std::tm t{}; localtime_r(&now, &t); local_seconds = int64_t(timegm(&t));
#endif
    if (local_seconds > 946684800) {
      config.has_time_base = true;
      config.time_base = uint64_t(local_seconds - 946684800) * uint64_t(MU_TB_HZ);
      std::tm d{}; const std::time_t ls = std::time_t(local_seconds); gmtime_r(&ls, &d);
      say("play: the game's calendar starts at %04d-%02d-%02d %02d:%02d (local time)", d.tm_year + 1900, d.tm_mon + 1, d.tm_mday, d.tm_hour, d.tm_min);
    }
  }
  mkdir(config.card_dir.c_str(), 0777);
  if (options.fresh_card)
    if (DIR* dir = opendir(config.card_dir.c_str())) {
      std::vector<std::string> names;
      while (dirent* entry = readdir(dir)) if (entry->d_name[0] != '.') names.push_back(entry->d_name);
      closedir(dir);
      for (const auto& name : names) std::remove((config.card_dir + "/" + name).c_str());
    }
  linux_host::set_tables_hook(tables_hook);
  mac_elf::set_embedded_image(mu_embedded_game, size_t(mu_embedded_game_end - mu_embedded_game));
  std::string error;
  if (!linux_host::initialize(config, &error)) { say("play: cannot start: %s", error.c_str()); linux_host::shutdown(); return nullptr; }
  say("play: started; memory card folder %s%s", config.card_dir.c_str(), options.fresh_card ? " (emptied)" : "");
  if (!direct) settings_report();
  linux_host::set_frame_observer(observer, nullptr);       // the frontend attaches here
  run->result = linux_host::run();
  features::call_run_end();                                // the game stopped; its memory is still mapped
  const auto stats = linux_host::counters();
  linux_host::set_frame_observer(nullptr, nullptr);        // and detaches, before the game is unmapped
  if (net_on) net_host::stop_tracking();                   // tracked pages writable again before they are unmapped
  say("play: stopped with code %d after %u retraces (%.1f min of game time)", run->result, stats.retraces, stats.retraces / 3600.0);
  linux_host::shutdown();
  if (net_on) { net_host::report(); net_host::shutdown(); }
  return nullptr;
}
}  // namespace

bool session_online() { return online_flag.load(std::memory_order_relaxed); }

int play_main() {
#ifdef __SWITCH__
  socketInitializeDefault();
  nxlinkStdio();
  // Keep the display awake while the game runs (upstream main.cpp SetThreadExecutionState: controllers do not count as user
  // activity). The GameCube adapter's pads reach this program over usb:hs, which the system's idle timer never sees, so the
  // console would dim and go to sleep mid-match: the program reports media playback for as long as it runs, as video players do.
  struct Awake {
    bool on = R_SUCCEEDED(appletSetMediaPlaybackState(true));
    ~Awake() { if (on) appletSetMediaPlaybackState(false); }
  } awake;
#endif
  mkdir(options.out_dir.c_str(), 0777);
  log_open();
  say("---- mu-native-gfx --play (native engine, interactive)%s%s ----", netplay_bridge::has_services() ? ", with online services" : "", options.app ? ", as the app" : "");
  if (options.app) app_report();
#ifdef __SWITCH__
  say("play: the display stays awake while the game runs (media playback state %s)", awake.on ? "set" : "refused");
#endif
  // The process layout, checked at the top of main (frontend_main.cpp), as the runner handles it.
  say("%s", fixedmap_layout());
  if (options.layout > 0) {
    say("play: this process cannot hold the game; starting the title again (start %d of at most 5)", fixedmap_relaunch_count() + 2);
    log_close();
    fixedmap_relaunch();                                    // does not come back when it works
    log_open();
  }
  if (options.layout) {
    say("play: cannot start: %s", fixedmap_error());
    // The app tells the player in words, not only in the log (a forwarder with --relaunch never gets here).
    if (options.app) { app_layout_screen(fixedmap_error()); screens_stop(); }
    log_close();
    return 1;
  }
  if (fixedmap_relaunch_count()) say("play: started over %d time(s) to get a usable layout", fixedmap_relaunch_count());
  if (options.iso.empty()) { say("play: --iso PATH is needed"); log_close(); return 2; }
  if (options.app && (!file_exists(options.iso) || options.iso.size() < 4 || options.iso.compare(options.iso.size() - 4, 4, ".iso") != 0))
    app_disc_convert(&options.iso);   // an RVZ or WIA image becomes the plain disc image once (melee-unlocked's disc_rvz.cpp)
  if (options.app && !file_exists(options.iso)) {
    say("play: cannot start: no disc image at %s", options.iso.c_str());
    app_message_screen("DISC NOT FOUND", {"Melee-NX-NetPlay needs your Super Smash Bros. Melee disc image (NTSC 1.02) at", options.iso, "",
                                          "Copy it there (or name it with iso= in launcher.ini), then start the app again."});
    screens_stop();
    log_close();
    return 2;
  }
  // Netplay options (--net, --online, --net-delay, --net-retry, --track-remap, ...: ../runner/net_host.h) among what is left.
  bool delay_given = false;
  for (size_t i = 0; i < options.rest.size(); ++i) {
    const auto next = [&]() -> std::string { return i + 1 < options.rest.size() ? options.rest[++i] : std::string(); };
    if (options.rest[i] == "--net-delay") delay_given = true;
    net_host::parse_option(options.rest[i], next, &net);
  }
  direct = net_host::wanted(net);
  // The app: Slippi's menus by default when this program can go online (launcher.ini offline=1 says it may not).
  if (options.app) settings_app_defaults(!options.offline && netplay_bridge::has_services());
  settings_boot_to_css(!direct && options.auto_vs && !options.auto_menus);
  settings_load(options.settings.empty() ? options.out_dir + "/settings.ini" : options.settings);
  for (const auto& arg : options.rest) if (arg == "--vanilla-only") settings_force_vanilla_only();
  if (settings_vanilla_only() && !direct && !retail_disc(options.iso)) {
    say("play: a vanilla-only session needs an unmodified NTSC 1.02 Melee disc (GALE01 revision 2): %s is not one", options.iso.c_str());
    log_close();
    return 2;
  }
  if (options.app) app_after_settings();
  if (direct) settings_deactivate();                        // only the online delay is taken from the file
  else if (settings_widescreen()) options.stretch = true;   // the 16:9 code draws for the whole screen
  // Input delay: --net-delay FRAMES|auto, else the settings file (onlinedelayauto / onlinedelay). Automatic is the
  // app's default: 2 to 5 frames from the round trip to the opponent, chosen by the netplay object at each game's start.
  if (!delay_given) {
    // The app: launcher.ini online_delay (auto or 1..9, the in-game menu writes it) wins over settings.ini.
    const int frames = options.app && options.online_delay >= 0 ? options.online_delay : settings_online_delay();
    net.delay_auto = frames <= 0;
    if (frames > 0) net.delay = frames;
  }
  // Between Melee-NX-NetPlay builds (patches/net-0004) and the Unranked skip list: settings.ini, except in a direct run (the
  // command line's --net-direct-pause, --net-port-colour, --net-shield, --net-skip there).
  if (!direct) settings_net_prefs(&net.direct_pause, net.port_colour, &net.shield_online, net.skip);
  net.out_dir = options.net_dir.empty() ? options.out_dir : options.net_dir; net.iso_path = options.iso; net.embedded_image = true;
  if (net.user_dir.empty() && !options.user_dir.empty()) net.user_dir = options.user_dir;   // the app's User/Slippi
#ifdef __SWITCH__
  const std::string user_dir = net.user_dir.empty() ? "sdmc:/switch/melee-nx-netplay/User/Slippi" : net.user_dir;   // the app's sign-in (port/app/main_switch.cpp)
#else
  const std::string user_dir = net.user_dir.empty() ? options.out_dir + "/user" : net.user_dir;
#endif
  // The services program checks what its first HTTPS request and the matchmaking connection need before anything
  // can use them (nothing is contacted). A failure leaves this session offline with the reason in the log.
  // The app's screens before the game (render thread, which then draws the game): an update from the player's build
  // computer, then the Slippi sign-in when there is no user.json yet, as port/app/main_switch.cpp does it.
  bool offline_chosen = options.offline;
  bool menu_ready = false;
  if (options.app && !direct) {
    const bool online_wanted = !options.offline && settings_slippi_menus() && netplay_bridge::has_services();
    const bool signed_in = net.fake_user || file_exists(user_dir + "/user.json");
    if (online_wanted && !signed_in) say("app: no %s/user.json: the sign-in screen comes first", user_dir.c_str());
    if (app_services_run && screens_start()) {
      // melee-unlocked's launcher comes first (mu_home.cpp): its Settings page is the in-game panel, so the panel's
      // rows are set up before it.
      app_menu_setup(); menu_ready = true;
      const AppStart start = app_services_run(!options.offline, online_wanted && !signed_in, user_dir);
      if (start == AppStart::Quit) { say("---- done: the player left before the game started ----"); screens_stop(); log_close(); return 0; }
      if (start == AppStart::PlayOffline && !file_exists(user_dir + "/user.json")) offline_chosen = true;
      if (!home_replay.empty()) {   // the home screen's Replay Viewer: the game starts with that replay, offline
        options.rest.push_back("--replay"); options.rest.push_back(home_replay);
        offline_chosen = true;
        say("play: the home screen asked for the replay %s", home_replay.c_str());
      }
      if (!delay_given) {           // the panel may have changed the online delay before the game
        const int frames = options.online_delay >= 0 ? options.online_delay : settings_online_delay();
        net.delay_auto = frames <= 0;
        if (frames > 0) net.delay = frames;
      }
    }
  }
  bool services_ok = true;
  if (netplay_bridge::has_services() && (!net.online.empty() || settings_slippi_menus()) && !offline_chosen) {
    services_ok = services_self_check && services_self_check();
    if (!services_ok) say("play: online is off for this session: the services check above failed");
  }
  if (!services_ok && !net.online.empty()) { screens_stop(); log_close(); return 1; }
  if (offline_chosen && !direct) {
    say("play: online is off for this session (%s): the Online Play page will show Log-in", options.offline ? "launcher.ini offline=1" : "the player chose to play offline");
  } else if (!direct && settings_slippi_menus() && netplay_bridge::has_services() && services_ok) {
    // The services program with Slippi's menus: the game's Online Play page asks the netplay host, as --online does.
    if (net.fake_user || file_exists(user_dir + "/user.json")) {
      menu_online = true;
      net.online = "unranked";             // only switches the services on; the mode is whatever row the player picks
      net.user_dir = user_dir;
    } else {
      say("play: online: no %s/user.json: the Online Play page will show Log-in. Sign in with the app first, or name the folder with --user-dir", user_dir.c_str());
    }
  } else if (!direct && settings_slippi_menus() && netplay_bridge::has_services()) {
    // services check failed: said above
  } else if (!direct && settings_slippi_menus()) {
    say("play: online: this program has no online services (the services program has): the Online Play page will show Log-in");
  }
  if (direct || menu_online) {
    net_host::set_speed_hook(io_game_speed);
    if (&fe::features::online_replay_for_report) net_host::set_replay_path_hook(fe::features::online_replay_for_report);   // Slippi's replay upload
    if (const int code = net_host::start(net, net_say)) { say("play: netplay could not start (%d)", code); screens_stop(); log_close(); return code; }
    net_on = true;
    // Tests (input suggestions): MU_NET_CONTROLLERS=1 lets the controllers, or the controller script, play a direct match
    // instead of the game's generated test inputs (the netplay host asks for those in a direct run).
    if (direct && std::getenv("MU_NET_CONTROLLERS") && std::getenv("MELEE_ONLINE_TEST_INPUTS")) {
      unsetenv("MELEE_ONLINE_TEST_INPUTS");
      say("play: netplay test: the controllers play the match, not the generated inputs (MU_NET_CONTROLLERS)");
    }
    if (options.sync_net) options.sync_decode = true;      // --gfx-sync-net: the earlier way, for comparison
    const std::string delay = net.delay_auto ? std::string("automatic") : std::to_string(net.delay);
    if (direct) say("play: netplay on: the retail game with the runner's seed and clock; settings.ini (but for the input delay, %s) and mod profiles are not read", delay.c_str());
    else say("play: online on: user folder %s, input delay %s; a search starts from the game's Online Play page", user_dir.c_str(), delay.c_str());
  }
#ifdef __SWITCH__
  // Clocks as the app sets them; the frontend adds GPU and memory when it attaches and restores those.
  const bool clock_service = switch_clocks_init();
  SwitchClockResult before{};
  if (clock_service) before = switch_clocks_read();
  if (options.overclock && clock_service) {
    SwitchClockRequest request; request.cpu_mhz = kSwitchClocksErista.cpu_mhz;
    const SwitchClockResult now = switch_clocks_apply(request);
    say("play: clocks: cpu %u -> %u MHz reported (%s); the core measures %u MHz", before.cpu_hz / 1000000u, now.cpu_hz / 1000000u, now.cpu_ok ? "accepted" : "REFUSED", switch_measure_cpu_mhz());
  }
#endif
  if (options.app && !menu_ready) app_menu_setup();        // the app's in-game menu (Aurora builds)
  Run run;
  for (;;) {
    // The game runs its own main loop and expects a desktop-sized stack.
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_t thread;
    if (pthread_create(&thread, &attr, play_thread, &run) == 0) pthread_join(thread, nullptr);
    else say("play: cannot create the simulation thread");
    pthread_attr_destroy(&attr);
    if (restart_args.empty() || run.result != 0) break;
    // Again in this process, offline (a replay): the netplay host has been shut down with the first run.
    for (auto& a : restart_args) options.rest.push_back(a);
    restart_args.clear();
    if (net_on) { net_on = false; menu_online = false; settings_online(false); }
    online_now = false; online_flag = false; in_match = false; tracked = false; scene_major = 0xFFFFFFFF; last_match_frame = 0;
    run = Run{};
    say("---- the game starts again ----");
  }
  screens_stop();                                           // the game never attached (it could not start): Aurora shuts down
  fixedmap_release_all();
#ifdef __SWITCH__
  if (clock_service) {
    if (options.overclock && before.cpu_hz) { SwitchClockRequest back; back.cpu_mhz = before.cpu_hz / 1000000u; switch_clocks_apply(back); }
    switch_clocks_exit();
  }
#endif
  say("---- done (%d) ----", run.result);
  log_close();
#ifdef __SWITCH__
  svcSleepThread(1000000000ull);    // let the last lines reach nxlink
  socketExit();
#endif
  return run.result;
}
}  // namespace fe
