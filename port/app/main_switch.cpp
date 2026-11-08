// Nintendo Switch entry point (libnx). Phase 1: the recompiled game runs
// headless on the console with a scripted or neutral controller and writes its
// log to the SD card, so the simulation cost of a match can be measured before
// the renderer is wired. Phase 2 adds the Aurora/Dawn/NVK frontend and input;
// phase 3 the Slippi online services. See docs/ARCHITECTURE.md.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <switch.h>
#include "switch_clocks.h"
#include "switch_threads.h"

#include "audio.h"
#include "audio_headless.h"
#include "exi_slippi.h"
#include "gx_core.h"
#include "hle_dvd.h"
#include "host.h"
#include "numeric.h"
#include "slippi_online.h"
#include "window.h"
#if MELEE_NX_AURORA
#include "game_menu.h"
#include "gx_aurora_bridge.h"
#include "metal_frontend.h"
#include "metal_window.h"
#include "overlay_imgui.h"
#include "slippi_login.h"
#include "switch_signin.h"
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <sys/stat.h>

#ifndef MELEE_PORT_VERSION
#define MELEE_PORT_VERSION "dev"
#endif

namespace ppc { void init_dispatch(); }

namespace {
// Everything the port reads or writes lives under this folder on the SD card.
const char* kBaseDir = "sdmc:/switch/melee-nx-netplay";
// The guest's call depth is capped at 20000 frames (ppc.h); the simulation
// thread gets a stack sized for that rather than the loader's main stack.
constexpr size_t kSimulationStack = 32u << 20;

std::atomic<int> g_exit_code{-1};
std::string g_result = "running";
int g_volume = 100;
bool g_hud = false;
std::string g_base;
std::map<std::string, std::string> g_ini;

void write_ini(const std::string& path, const std::map<std::string, std::string>& values) {
  std::ofstream out(path, std::ios::trunc);
  out << "# Melee-NX-NetPlay settings. Edit while the app is closed; the in-game menu (L+R+Start) saves here too.\n";
  for (const auto& [key, value] : values) out << key << '=' << value << '\n';
}

std::map<std::string, std::string> read_ini(const std::string& path) {
  std::map<std::string, std::string> out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    out[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return out;
}

void ensure_dir(const std::string& path) { mkdir(path.c_str(), 0777); }

// Boots the guest. Runs on the simulation thread; exceptions thrown out of the
// guest (ExitRequested, HostFatal, OSLoadContext at top level) end the run.
void simulation_thread(void*) {
  switch_sync_clang_tls();
  int code = 0;
#if MELEE_NX_AURORA
  std::unique_ptr<host::MetalFrontend> frontend;
  bool attached = false;
#endif
  bool quit_before_boot = false;
  {
    host::ScopedFatalBoundary fatal_boundary;
    ppc::ScopedGuestFpEnvironment fp_environment(0);
    try {
      if (!host::disc_open(host::options.iso)) {
        host::log("cannot open disc image %s", host::options.iso.c_str());
        g_result = "disc_open_failed"; code = 1;
      } else {
#if MELEE_NX_AURORA
        host::AuroraMetalConfig aurora;
        aurora.app_name = "Melee-NX-NetPlay";
        aurora.user_path = host::options.profile_dir;
        aurora.cache_path = host::options.cache_dir;
        aurora.resources_path = host::options.cache_dir;   // Aurora writes nothing here; romfs is read-only
        aurora.window_width = 1280; aurora.window_height = 720;
        frontend = host::create_aurora_switch_frontend(aurora);
        host::metal_window_attach(*frontend);
        attached = true;
        host::g_has_window = true;
        auto& online = slippi::online::config();
        if (!online.offline) {
          // Signed in already (user.json from a previous sign-in or copied from the
          // Slippi Launcher)? Then straight to the game; otherwise the sign-in screen.
          slippi::login::Account account;
          if (!slippi::login::read_user_file(online.user_dir, account) || !account.valid()) {
            std::string name, connect;
            switch (host::switch_signin_run(*frontend, online.user_dir, name, connect)) {
              case host::SignInResult::SignedIn: break;
              case host::SignInResult::PlayOffline: online.offline = true; host::options.offline = true; host::log("slippi: playing offline"); break;
              case host::SignInResult::Quit: g_result = "requested_exit"; code = 0; quit_before_boot = true; break;
            }
          } else {
            host::log("slippi: signed in as %s (%s)", account.display_name.c_str(), account.connect_code.c_str());
          }
        }
        {
          host::RuntimeSettings rs;
          rs.volume = g_volume; rs.hud = g_hud; rs.online_delay = online.delay; rs.vsync = true;
          host::menu_init(rs, [](const host::RuntimeSettings& st, host::MenuChange what) {
            switch (what) {
              case host::MenuChange::Volume: host::audio_set_volume(st.volume); g_ini["volume"] = std::to_string(st.volume); break;
              case host::MenuChange::Hud: g_ini["hud"] = st.hud ? "1" : "0"; break;
              case host::MenuChange::OnlineDelay:
                slippi::online::config().delay = st.online_delay;
                g_ini["online_delay"] = std::to_string(st.online_delay);
                host::log("slippi: online input delay now %d frame%s, from the next match", st.online_delay, st.online_delay == 1 ? "" : "s");
                break;
              default: break;   // graphics, touch and fullscreen rows have no effect on the console yet
            }
            write_ini(g_base + "/launcher.ini", g_ini);
          });
        }
        host::switch_frontend_set_overlay([] {
          host::OverlayFrame overlay;
          if (host::game_overlay(overlay)) host::overlay_imgui_draw(overlay);
        });
        gx::init(frontend.get());
        const bool audio_ok = quit_before_boot || host::audio_open(g_volume, nullptr, true);
#else
        gx::init(nullptr);
        const bool audio_ok = host::audio_open(0, host::options.audio_dump.c_str(), false);
#endif
        if (quit_before_boot) {
        } else if (!audio_ok) {
          host::log("cannot open the audio output"); g_result = "audio_open_failed"; code = 1;
        } else {
          ppc::init_dispatch();
          host::boot_setup();
          host::log("boot: entering __start at 8000522C");
          ppc::call(*host::cpu, host::ram, 0x8000522Cu);
          host::log("guest returned from __start");
          g_result = "guest_returned"; code = 4;
        }
      }
#if MELEE_NX_AURORA
    } catch (const gx::aurora_bridge::CoverageError& e) {
      host::log("unsupported render state: %s", e.what()); g_result = std::string("render: ") + e.what(); code = 5;
#endif
    } catch (const host::HostFatal& e) {
      host::log("fatal: %s", e.what()); g_result = std::string("fatal: ") + e.what(); code = 3;
    } catch (const ExitRequested& stop) {
      code = stop.code; g_result = code ? "requested_exit" : "frame_bound";
    } catch (const LoadContextUnwind&) {
      host::log("unexpected OSLoadContext at top level"); g_result = "unexpected_load_context"; code = 3;
    } catch (const std::exception& e) {
      host::log("host exception: %s", e.what()); g_result = std::string("host exception: ") + e.what(); code = 3;
    } catch (...) {
      g_result = "unknown host exception"; code = 3;
    }
  }
  gx::init(nullptr);
#if MELEE_NX_AURORA
  host::g_has_window = false;
  if (attached) host::metal_window_detach(*frontend);
#endif
  slippi::shutdown();
  hle::dvd_shutdown();
  host::audio_close();
#if MELEE_NX_AURORA
  if (frontend) {
    const auto stats = frontend->stats();
    host::log("aurora: presented=%llu draws=%llu xfb=%llu unsupported(render=%llu order=%llu)",
              (unsigned long long)stats.presented_frames, (unsigned long long)stats.decoded_draws,
              (unsigned long long)stats.presented_xfb, (unsigned long long)stats.unsupported.render_state,
              (unsigned long long)stats.unsupported.frame_order);
    frontend->shutdown();
  }
#endif
  ppc::log_aot_diagnostics();
  host::log("switch result: exit=%d retraces=%u requested=%u", code, host::retrace_count(), host::options.frames);
  host::close_log_file();
  g_exit_code = code;
}
}  // namespace

extern "C" void userAppInit() {
  romfsInit();                 // Slippi's Sys folder ships in the NRO as romfs:/Sys
  socketInitializeDefault();
  nxlinkStdio();               // stdout to nxlink when a host is listening; harmless otherwise
}
extern "C" void userAppExit() {
  socketExit();
  romfsExit();
}

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  switch_set_exit_hook([] { host::close_log_file(); });
#if !MELEE_NX_AURORA
  // The text console and libnx pad are the headless build's only UI. With the
  // renderer, Aurora's SDL window owns the display and the controllers.
  consoleInit(nullptr);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
#endif

  const std::string base = kBaseDir;
  ensure_dir(base);
  ensure_dir(base + "/User");
  ensure_dir(base + "/User/GC");
  ensure_dir(base + "/User/GC/CardA");
  ensure_dir(base + "/User/Slippi");
  ensure_dir(base + "/Replays");
  ensure_dir(base + "/Cache");
  ensure_dir(base + "/Logs");
  g_base = base;
  g_ini = read_ini(base + "/launcher.ini");
  const auto& ini = g_ini;
  auto get = [&](const char* key, const std::string& fallback) {
    auto it = ini.find(key); return it == ini.end() || it->second.empty() ? fallback : it->second;
  };

  host::Options& o = host::options;
  o.iso = get("iso", base + "/GALE01.iso");
  o.sys_dir = get("sys_dir", "romfs:/Sys");
  o.profile_dir = base + "/User";
  o.card_dir = base + "/User/GC/CardA";
  o.replay_dir = base + "/Replays";
  o.cache_dir = base + "/Cache";
  o.log_file = base + "/Logs/latest.log";
  o.frames = static_cast<uint32_t>(std::strtoul(get("frames", "0").c_str(), nullptr, 10));
  o.fast = get("fast", "0") == "1";
  o.volume = 0;
  g_volume = static_cast<int>(std::strtol(get("volume", "100").c_str(), nullptr, 10));
  g_hud = get("hud", "0") == "1";
  // Slippi identity: user.json (connect code, play key) in User/Slippi, the Slippi
  // Launcher's own layout, until the console has a sign-in screen. The online
  // input delay is a player setting (1..9, Slippi's default 2), never hardcoded.
  auto& online = slippi::online::config();
  online.user_dir = base + "/User/Slippi";
  int delay = static_cast<int>(std::strtol(get("online_delay", "2").c_str(), nullptr, 10));
  online.delay = delay < 1 ? 1 : delay > 9 ? 9 : delay;
#if MELEE_NX_AURORA && !defined(MELEE_PORT_OFFLINE)
  o.offline = get("offline", "0") == "1";
#else
  o.offline = true;
#endif
  online.offline = o.offline;
  const std::string script = get("script", "");
  if (!script.empty() && !host::input_load_script(script.c_str()))
    std::printf("cannot load input script %s\n", script.c_str());

  const bool applet_mode = appletGetAppletType() != AppletType_Application && appletGetAppletType() != AppletType_SystemApplication;
#if !MELEE_NX_AURORA
  std::printf("Melee-NX-NetPlay %s\n", MELEE_PORT_VERSION);
  std::printf("disc %s\nlog  %s\n", o.iso.c_str(), o.log_file.c_str());
  if (applet_mode) std::printf("\nLaunched in applet mode: hold R while opening a game to get the full memory.\n");
  std::printf("frames=%u fast=%d script=%s\n\n", o.frames, o.fast ? 1 : 0, script.empty() ? "(neutral pad)" : script.c_str());
  consoleUpdate(nullptr);
#endif
  if (applet_mode) host::log("launched in applet mode: hold R while opening a game to get application memory");

  // Clocks: the simulation is CPU-bound, so ask for Erista's top CPU clock by
  // default (overclock=0 keeps the system's choice; the MHz keys tune each rail).
  if (get("overclock", "1") == "1") {
    SwitchClockRequest clocks = kSwitchClocksErista;
    clocks.cpu_mhz = static_cast<uint32_t>(std::strtoul(get("clock_cpu_mhz", std::to_string(clocks.cpu_mhz)).c_str(), nullptr, 10));
    clocks.gpu_mhz = static_cast<uint32_t>(std::strtoul(get("clock_gpu_mhz", std::to_string(clocks.gpu_mhz)).c_str(), nullptr, 10));
    clocks.mem_mhz = static_cast<uint32_t>(std::strtoul(get("clock_mem_mhz", std::to_string(clocks.mem_mhz)).c_str(), nullptr, 10));
    if (switch_clocks_init()) {
      const SwitchClockResult r = switch_clocks_apply(clocks);
      host::log("clocks: requested cpu %u gpu %u mem %u MHz; now cpu %u gpu %u mem %u MHz (%s%s%s)", clocks.cpu_mhz, clocks.gpu_mhz,
                clocks.mem_mhz, r.cpu_hz / 1000000u, r.gpu_hz / 1000000u, r.mem_hz / 1000000u, r.cpu_ok ? "cpu ok " : "cpu refused ",
                r.gpu_ok ? "gpu ok " : "gpu refused ", r.mem_ok ? "mem ok" : "mem refused");
    } else {
      host::log("clocks: the clock service is not reachable; running at the system's clocks (sys-clk can set them)");
    }
  } else {
    if (switch_clocks_init()) {
      const SwitchClockResult r = switch_clocks_read();
      host::log("clocks: overclock=0; system clocks cpu %u gpu %u mem %u MHz", r.cpu_hz / 1000000u, r.gpu_hz / 1000000u, r.mem_hz / 1000000u);
    }
  }
  #if MELEE_NX_AURORA
  host::log("Melee-NX-NetPlay %s: Aurora/Vulkan frontend", MELEE_PORT_VERSION);
#else
  host::log("Melee-NX-NetPlay %s: headless bring-up", MELEE_PORT_VERSION);
#endif
  host::log("execution: frames=%u fast=%s fp_profile=%s", o.frames, o.fast ? "true" : "false", ppc::fp_profile_name(ppc::fp_profile()));
  host::log("paths: iso=%s sys=%s profile=%s cache=%s", o.iso.c_str(), o.sys_dir.c_str(), o.profile_dir.c_str(), o.cache_dir.c_str());

  Thread sim;
  Result rc = threadCreate(&sim, simulation_thread, nullptr, nullptr, kSimulationStack, 0x28, 1);
  if (R_FAILED(rc)) { std::printf("threadCreate failed: %08X\n", rc); consoleUpdate(nullptr); svcSleepThread(3'000'000'000ULL); consoleExit(nullptr); return 1; }
  threadStart(&sim);

  uint32_t last_retraces = 0;
  while (appletMainLoop()) {
#if !MELEE_NX_AURORA
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) { host::log("plus pressed: stopping"); break; }
    const uint32_t retraces = host::retrace_count();
    if (retraces != last_retraces && retraces % 60 == 0) {
      std::printf("\rframe %u", retraces);
      last_retraces = retraces;
    }
    consoleUpdate(nullptr);
#else
    (void)last_retraces;
#endif
    if (g_exit_code >= 0) break;
    svcSleepThread(16'000'000ULL);
  }
  if (g_exit_code < 0) {
    // The guest has no cooperative stop yet; the log already has the frame data.
    host::close_log_file();
  } else {
    threadWaitForExit(&sim);
    threadClose(&sim);
  }
#if !MELEE_NX_AURORA
  std::printf("\n%s (exit %d). Press + to leave.\n", g_result.c_str(), g_exit_code.load());
  consoleUpdate(nullptr);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(nullptr);
    svcSleepThread(50'000'000ULL);
  }
  consoleExit(nullptr);
#endif
  switch_clocks_exit();   // the system restores its own clock policy for the next title
  return g_exit_code < 0 ? 0 : g_exit_code.load();
}
