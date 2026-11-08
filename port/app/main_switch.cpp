// Nintendo Switch entry point (libnx). Phase 1: the recompiled game runs
// headless on the console with a scripted or neutral controller and writes its
// log to the SD card, so the simulation cost of a match can be measured before
// the renderer is wired. Phase 2 adds the Aurora/Dawn/NVK frontend and input;
// phase 3 the Slippi online services. See docs/ARCHITECTURE.md.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <switch.h>
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

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <map>
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
  {
    host::ScopedFatalBoundary fatal_boundary;
    ppc::ScopedGuestFpEnvironment fp_environment(0);
    try {
      if (!host::disc_open(host::options.iso)) {
        host::log("cannot open disc image %s", host::options.iso.c_str());
        g_result = "disc_open_failed"; code = 1;
      } else {
        gx::init(nullptr);
        if (!host::audio_open(0, host::options.audio_dump.c_str(), false)) {
          host::log("cannot open the AI DMA sink"); g_result = "audio_open_failed"; code = 1;
        } else {
          ppc::init_dispatch();
          host::boot_setup();
          host::log("boot: entering __start at 8000522C");
          ppc::call(*host::cpu, host::ram, 0x8000522Cu);
          host::log("guest returned from __start");
          g_result = "guest_returned"; code = 4;
        }
      }
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
  slippi::shutdown();
  hle::dvd_shutdown();
  host::audio_close();
  ppc::log_aot_diagnostics();
  host::log("switch result: exit=%d retraces=%u requested=%u", code, host::retrace_count(), host::options.frames);
  host::close_log_file();
  g_exit_code = code;
}
}  // namespace

extern "C" void userAppInit() {
  socketInitializeDefault();
  nxlinkStdio();
}
extern "C" void userAppExit() { socketExit(); }

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  switch_set_exit_hook([] { host::close_log_file(); });
  consoleInit(nullptr);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  const std::string base = kBaseDir;
  ensure_dir(base);
  ensure_dir(base + "/User");
  ensure_dir(base + "/User/GC");
  ensure_dir(base + "/User/GC/CardA");
  ensure_dir(base + "/Replays");
  ensure_dir(base + "/Cache");
  ensure_dir(base + "/Logs");
  const auto ini = read_ini(base + "/launcher.ini");
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
  o.offline = true;
  o.volume = 0;
  slippi::online::config().offline = true;
  slippi::online::config().user_dir = o.profile_dir;
  const std::string script = get("script", "");
  if (!script.empty() && !host::input_load_script(script.c_str()))
    std::printf("cannot load input script %s\n", script.c_str());

  std::printf("Melee-NX-NetPlay %s\n", MELEE_PORT_VERSION);
  std::printf("disc %s\nlog  %s\n", o.iso.c_str(), o.log_file.c_str());
  if (appletGetAppletType() != AppletType_Application && appletGetAppletType() != AppletType_SystemApplication)
    std::printf("\nLaunched in applet mode: hold R while opening a game to get the full memory.\n");
  std::printf("frames=%u fast=%d script=%s\n\n", o.frames, o.fast ? 1 : 0, script.empty() ? "(neutral pad)" : script.c_str());
  consoleUpdate(nullptr);

  host::log("Melee-NX-NetPlay %s: Switch headless bring-up", MELEE_PORT_VERSION);
  host::log("execution: frames=%u fast=%s fp_profile=%s", o.frames, o.fast ? "true" : "false", ppc::fp_profile_name(ppc::fp_profile()));
  host::log("paths: iso=%s sys=%s profile=%s cache=%s", o.iso.c_str(), o.sys_dir.c_str(), o.profile_dir.c_str(), o.cache_dir.c_str());

  Thread sim;
  Result rc = threadCreate(&sim, simulation_thread, nullptr, nullptr, kSimulationStack, 0x2C, -2);
  if (R_FAILED(rc)) { std::printf("threadCreate failed: %08X\n", rc); consoleUpdate(nullptr); svcSleepThread(3'000'000'000ULL); consoleExit(nullptr); return 1; }
  threadStart(&sim);

  uint32_t last_retraces = 0;
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) { host::log("plus pressed: stopping"); break; }
    const uint32_t retraces = host::retrace_count();
    if (retraces != last_retraces && retraces % 60 == 0) {
      std::printf("\rframe %u", retraces);
      last_retraces = retraces;
    }
    if (g_exit_code >= 0) break;
    consoleUpdate(nullptr);
    svcSleepThread(16'000'000ULL);
  }
  if (g_exit_code < 0) {
    // The guest has no cooperative stop yet; the log already has the frame data.
    host::close_log_file();
  } else {
    threadWaitForExit(&sim);
    threadClose(&sim);
  }
  std::printf("\n%s (exit %d). Press + to leave.\n", g_result.c_str(), g_exit_code.load());
  consoleUpdate(nullptr);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(nullptr);
    svcSleepThread(50'000'000ULL);
  }
  consoleExit(nullptr);
  return g_exit_code < 0 ? 0 : g_exit_code.load();
}
