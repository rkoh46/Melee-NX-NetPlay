// mu-native-gfx --play: the native engine for a person. No script, no reference digest, no frame
// limit: the game boots to its own menus, reads the app's controller input and plays its sound,
// until the player holds L + R + Start or closes the program. The scripted runner (runner_main.cpp)
// is not involved, so the exactness runs are unchanged.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"

#include "host_services.h"
#include "mu_elf_bridge.h"
#include "fixedmap.h"

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
void no_observer(const MuStatePod&, const linux_host::Counters&, void*) {}

struct Run { int result = 1; };
void* play_thread(void* user) {
  auto* run = static_cast<Run*>(user);
  linux_host::Config config;
  config.iso_path = options.iso;
  config.game_path = "(embedded)";
  config.log_path = options.out_dir + "/host.log";
  config.card_dir = options.out_dir + "/card-play";       // kept between sessions, apart from the scripted runs' card
  config.max_frames = options.frames;                     // 0: until the player stops it
  config.pace = false;                                    // the frontend paces the retrace
  config.headless = true; config.no_gc_adapter = true;    // the host's own SDL2/libusb input is not built in
  config.vanilla_game = settings_vanilla();               // by default melee-unlocked's game as its own hosts run it
  mkdir(config.card_dir.c_str(), 0777);
  if (options.fresh_card)
    if (DIR* dir = opendir(config.card_dir.c_str())) {
      std::vector<std::string> names;
      while (dirent* entry = readdir(dir)) if (entry->d_name[0] != '.') names.push_back(entry->d_name);
      closedir(dir);
      for (const auto& name : names) std::remove((config.card_dir + "/" + name).c_str());
    }
  mac_elf::set_embedded_image(mu_embedded_game, size_t(mu_embedded_game_end - mu_embedded_game));
  std::string error;
  if (!linux_host::initialize(config, &error)) { say("play: cannot start: %s", error.c_str()); linux_host::shutdown(); return nullptr; }
  say("play: started; memory card folder %s%s", config.card_dir.c_str(), options.fresh_card ? " (emptied)" : "");
  linux_host::set_frame_observer(no_observer, nullptr);    // the frontend attaches here
  run->result = linux_host::run();
  const auto stats = linux_host::counters();
  linux_host::set_frame_observer(nullptr, nullptr);        // and detaches, before the game is unmapped
  say("play: stopped with code %d after %u retraces (%.1f min of game time)", run->result, stats.retraces, stats.retraces / 3600.0);
  linux_host::shutdown();
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
  settings_boot_to_css(options.auto_vs && !options.auto_menus);
  settings_load(options.settings.empty() ? options.out_dir + "/settings.ini" : options.settings);
  if (settings_widescreen()) options.stretch = true;      // the 16:9 code draws for the whole screen
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
