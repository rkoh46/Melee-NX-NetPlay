// mu-native-gfx --play: the native engine for a person. No script, no reference digest, no frame
// limit: the game boots to its own menus, reads the app's controller input and plays its sound,
// until the player holds L + R + Start or closes the program. The scripted runner (runner_main.cpp)
// is not involved, so the exactness runs are unchanged.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"

#include "host_services.h"
#include "mu_elf_bridge.h"
#include "fixedmap.h"
#include "hostprof.h"
#include "net_host.h"
#include "mu_host.h"

#include <climits>
#include <cstdarg>

#include <cstdio>
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

namespace fe {
namespace {
net_host::Options net;
bool net_on = false;
uint32_t last_match_frame = 0;

// Once per retrace after the frame. With netplay: the state row the netplay host checks and records.
void observer(const MuStatePod& state, const linux_host::Counters& counters, void*) {
  if (!net_on) return;
  const bool fighting = state.match_frame && state.match_frame != last_match_frame;
  last_match_frame = state.match_frame;
  char row[1400]; int n = 0;
  n += std::snprintf(row + n, sizeof row - n, "%u,%u,%u,%u,%u", counters.retraces, state.scene_major, state.scene, state.match_frame, state.rng);
  for (const auto& f : state.player)
    n += std::snprintf(row + n, sizeof row - n, ",%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u", f.present, f.stocks, f.action, f.anim_frame,
                       f.pos_x, f.pos_y, f.pos_z, f.vel_x, f.vel_y, f.vel_z, f.percent, f.facing);
  row[n++] = '\n';
  net_host::on_retrace(row, size_t(n), fighting, -1.0);
}

// The host hands its tables over before the game sees them: netplay takes slippi_command and its other entries;
// the mod commands (frontend/src/mods.cpp) are asked first, as in the hook of the host services.
int32_t (*net_command)(uint8_t, const uint8_t*, uint32_t, uint8_t*, uint32_t, uint32_t*) = nullptr;
extern "C" __attribute__((weak)) int32_t mu_frontend_command(uint8_t, const uint8_t*, uint32_t, uint8_t*, uint32_t, uint32_t*);
int32_t command(uint8_t c, const uint8_t* payload, uint32_t size, uint8_t* response, uint32_t capacity, uint32_t* response_size) {
  if (response_size) *response_size = 0;
  if (mu_frontend_command) { const int32_t mine = mu_frontend_command(c, payload, size, response, capacity, response_size); if (mine != INT32_MIN) return mine; }
  return net_command ? net_command(c, payload, size, response, capacity, response_size) : -1;
}
void tables_hook(MuHostApi* host, MuGameApi* game) {
  if (host && net_on) { net_host::install(host); net_command = host->slippi_command; host->slippi_command = command; }
  if (options.profile) hostprof::wrap_tables(host, game);
}
void net_say(const char* format, ...) {
  char line[2048];
  va_list args; va_start(args, format); std::vsnprintf(line, sizeof line, format, args); va_end(args);
  say("%s", line);
}

struct Run { int result = 1; };
void* play_thread(void* user) {
  auto* run = static_cast<Run*>(user);
  linux_host::Config config;
  config.iso_path = options.iso;
  config.game_path = "(embedded)";
  config.log_path = options.out_dir + "/host.log";
  // Kept between sessions, apart from the scripted runs' card; a mod profile has its own.
  const std::string profile_card = net_on ? std::string() : mods_configure(settings_profile());
  config.card_dir = profile_card.empty() ? options.out_dir + "/card-play" : profile_card;
  config.max_frames = options.frames;                     // 0: until the player stops it
  // The frontend paces the retrace; with netplay the host does (its clock is what the netplay host moves).
  config.pace = net_on;
  if (net_on) options.pace = false;
  config.headless = true; config.no_gc_adapter = true;    // the host's own SDL2/libusb input is not built in
  config.vanilla_game = settings_vanilla();               // by default melee-unlocked's game as its own hosts run it
  if (net_on) {
    // As the runner's netplay runs: the retail game, the same seed and clock on both sides. No settings file, no mods.
    config.vanilla_game = true;
    config.has_rng_seed = true; config.rng_seed = 0x12345678;
    config.has_time_base = true; config.time_base = 0;
    options.fresh_card = true;
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
  if (!net_on) settings_report();
  linux_host::set_frame_observer(observer, nullptr);       // the frontend attaches here
  run->result = linux_host::run();
  const auto stats = linux_host::counters();
  linux_host::set_frame_observer(nullptr, nullptr);        // and detaches, before the game is unmapped
  if (net_on) net_host::stop_tracking();                   // tracked pages writable again before they are unmapped
  say("play: stopped with code %d after %u retraces (%.1f min of game time)", run->result, stats.retraces, stats.retraces / 3600.0);
  linux_host::shutdown();
  if (net_on) { net_host::report(); net_host::shutdown(); }
  return nullptr;
}
}  // namespace

int play_main() {
#ifdef __SWITCH__
  socketInitializeDefault();
  nxlinkStdio();
#endif
  mkdir(options.out_dir.c_str(), 0777);
  say("---- mu-native-gfx --play (native engine, interactive) ----");
  if (options.iso.empty()) { say("play: --iso PATH is needed"); return 2; }
  // Netplay options (--net, --online, --net-delay, --track-remap, ...: ../runner/net_host.h) among what is left.
  for (size_t i = 0; i < options.rest.size(); ++i) {
    const auto next = [&]() -> std::string { return i + 1 < options.rest.size() ? options.rest[++i] : std::string(); };
    net_host::parse_option(options.rest[i], next, &net);
  }
  if (net_host::wanted(net)) {
    net.out_dir = options.out_dir; net.iso_path = options.iso; net.embedded_image = true;
    if (const int code = net_host::start(net, net_say)) { say("play: netplay could not start (%d)", code); return code; }
    net_on = true;
#ifdef __SWITCH__
    // Rollback snapshots track writes by remapping pages of the game's memory (--track-remap). The decoder thread
    // reads that memory while the game runs, so with netplay the stream is decoded inside the game's own flush.
    options.sync_decode = true;
#endif
    say("play: netplay on: the retail game with the runner's seed and clock; settings.ini and mod profiles are not read");
  } else {
    settings_boot_to_css(options.auto_vs && !options.auto_menus);
    settings_load(options.settings.empty() ? options.out_dir + "/settings.ini" : options.settings);
    if (settings_widescreen()) options.stretch = true;      // the 16:9 code draws for the whole screen
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
  Run run;
  // The game runs its own main loop and expects a desktop-sized stack.
  pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 8u << 20);
  pthread_t thread;
  if (pthread_create(&thread, &attr, play_thread, &run) == 0) pthread_join(thread, nullptr);
  else say("play: cannot create the simulation thread");
  pthread_attr_destroy(&attr);
  fixedmap_release_all();
#ifdef __SWITCH__
  if (clock_service) {
    if (options.overclock && before.cpu_hz) { SwitchClockRequest back; back.cpu_mhz = before.cpu_hz / 1000000u; switch_clocks_apply(back); }
    switch_clocks_exit();
  }
#endif
  say("---- done (%d) ----", run.result);
#ifdef __SWITCH__
  svcSleepThread(1000000000ull);    // let the last lines reach nxlink
  socketExit();
#endif
  return run.result;
}
}  // namespace fe
