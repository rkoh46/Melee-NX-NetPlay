// Nintendo Switch entry point (libnx). Phase 1: the recompiled game runs
// headless on the console with a scripted or neutral controller and writes its
// log to the SD card, so the simulation cost of a match can be measured before
// the renderer is wired. Phase 2 adds the Aurora/Dawn/NVK frontend and input;
// phase 3 the Slippi online services. See docs/ARCHITECTURE.md.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <switch.h>
#include "switch_clocks.h"
#include "gxn_glue_host.h"
#include "gml_glue.h"
#include "switch_profile.h"
#include "switch_hitch.h"
#include "log_switch.h"
#if MELEE_NX_AURORA
#include "async_backend.h"
#endif
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
bool g_profiling = false;
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

// Names the exception in flight (call from a catch(...) block) and returns the exit code.
int describe_current_exception(std::string& result) {
  try { throw; }
#if MELEE_NX_AURORA
  catch (const gx::aurora_bridge::CoverageError& e) { host::log("unsupported render state: %s", e.what()); result = std::string("render: ") + e.what(); return 5; }
#endif
  catch (const host::HostFatal& e) { host::log("fatal: %s", e.what()); result = std::string("fatal: ") + e.what(); return 3; }
  catch (const ExitRequested& stop) { result = stop.code ? "requested_exit" : "frame_bound"; return stop.code; }
  catch (const LoadContextUnwind&) { host::log("unexpected OSLoadContext at top level"); result = "unexpected_load_context"; return 3; }
  catch (const std::exception& e) { host::log("host exception: %s", e.what()); result = std::string("host exception: ") + e.what(); return 3; }
  catch (...) { result = "unknown host exception"; return 3; }
}

struct GuestRun {
  std::atomic<bool> done{false};
  int code = 0;
  std::string result;
};

// The guest from __start to the end of the run. On the renderer build this is its own thread
// (core 2, the highest application priority) so the game never waits for the GPU side; the
// frames it finishes go through gx::AsyncBackend to the thread that owns Aurora.
// The first C++ exception of the process makes libgcc sort the unwind entries of the whole executable
// (one per translated function, ~2 MB of .eh_frame; there is no .eh_frame_hdr to search on the
// console). Melee's first guest longjmp (a stage animation lookup) paid that, ~16 ms, mid-match; one
// throw here pays it during boot instead.
[[gnu::noinline]] static void warm_unwinder() {
  try { throw ppc::GuestLongJmp{0, 0}; } catch (const ppc::GuestLongJmp&) {}
}
void run_guest(GuestRun& run) {
  switch_sync_clang_tls();
  { const double t0 = host::now_seconds(); warm_unwinder(); host::log("boot: unwinder warmed in %.1f ms", (host::now_seconds() - t0) * 1000.0); }
  host::ScopedFatalBoundary fatal_boundary;
  ppc::ScopedGuestFpEnvironment fp_environment(0);
  try {
    ppc::init_dispatch();
    host::boot_setup();
    host::log("boot: entering __start at 8000522C");
    ppc::call(*host::cpu, host::ram, 0x8000522Cu);
    host::log("guest returned from __start");
    run.result = "guest_returned"; run.code = 4;
  } catch (...) {
    run.code = describe_current_exception(run.result);
  }
  try { gx::gpu_finish(); } catch (...) {}   // the GPU thread's last frames reach the renderer before it stops
  run.done.store(true, std::memory_order_release);
}
void guest_thread_entry(void* arg) { run_guest(*static_cast<GuestRun*>(arg)); }

// Sets the game up and, on the renderer build, owns Aurora for the whole run: the sign-in
// screen, then the frames the guest thread hands over. Headless, it runs the guest itself.
void simulation_thread(void*) {
  switch_sync_clang_tls();
  int code = 0;
#if MELEE_NX_AURORA
  std::unique_ptr<host::MetalFrontend> frontend;
  std::unique_ptr<gx::AsyncBackend> async;
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
        switch_set_pthread_cores(0, 0x3);   // Aurora's FIFO processor and render worker: cores 0-1, never the game's core 2
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
        // draw_dump=MM:SS:N writes Logs/draws.txt for the N-th frame of scene MM/SS (hex), one
        // line per draw with the registers the bridge saw: the way to find what Aurora renders wrongly.
        if (const auto dd = g_ini.find("draw_dump"); dd != g_ini.end() && dd->second.size() >= 5) {
          unsigned mm = 0, ss = 0, n = 120;
          if (std::sscanf(dd->second.c_str(), "%x:%x:%u", &mm, &ss, &n) >= 2)
            host::switch_frontend_set_draw_dump(g_base + "/Logs/draws.txt", uint8_t(mm), uint8_t(ss), n);
        }
        // Logs/gfx_debug.txt holds render experiment bits, re-read every second (upload a new one over FTP).
        host::switch_frontend_set_debug_file(g_base + "/Logs/gfx_debug.txt");
        host::switch_frontend_set_overlay([] {
          host::OverlayFrame overlay;
          if (host::game_overlay(overlay)) host::overlay_imgui_draw(overlay);
        });
        async = std::make_unique<gx::AsyncBackend>(*frontend);
        async->set_idle([] { host::window_pump(); });   // events are the owner thread's job
        gx::init(async.get());
        const bool audio_ok = quit_before_boot || host::audio_open(g_volume, host::options.audio_dump.c_str(), true);
#else
        gx::init(nullptr);
        const bool audio_ok = host::audio_open(0, host::options.audio_dump.c_str(), false);
#endif
        if (quit_before_boot) {
        } else if (!audio_ok) {
          host::log("cannot open the audio output"); g_result = "audio_open_failed"; code = 1;
        } else {
#if MELEE_NX_AURORA
          GuestRun run;
          Thread guest;
          int guest_priority = 0x1C;
          Result grc = threadCreate(&guest, guest_thread_entry, &run, nullptr, kSimulationStack, guest_priority, 2);
          if (R_FAILED(grc)) { guest_priority = 0x28; grc = threadCreate(&guest, guest_thread_entry, &run, nullptr, kSimulationStack, guest_priority, 2); }
          if (R_FAILED(grc)) throw std::runtime_error("cannot create the guest thread");
          host::log("threads: guest on core 2 priority 0x%02X; renderer on core 1", guest_priority);
          threadStart(&guest);
          // Hitch catcher: where the game thread is when a frame runs past 40 ms (hitch_watch=0 turns it off).
          const auto hw = g_ini.find("hitch_watch");
          if (!g_profiling && (hw == g_ini.end() || hw->second != "0") && switch_hitch_start(&guest, kSimulationStack))
            host::log("hitch: watching the game thread (a frame over 40 ms logs where it is)");
          if (g_profiling) {
            // The owner thread (this one) renders; sample it alongside the guest so both sides of the
            // frame show up (pc/lr = guest, pc1/lr1 = renderer).
            static Thread self; self.handle = threadGetCurHandle();   // a real handle: the pseudo handle means "caller" to the sampler
            // profile_range=LO:HI:ANCHOR (ELF hex addresses; tools/nx-profile.sh --range writes it) counts the callers of that code.
            if (const auto pr = g_ini.find("profile_range"); pr != g_ini.end()) {
              unsigned long long lo = 0, hi = 0, anchor = 0;
              if (std::sscanf(pr->second.c_str(), "%llx:%llx:%llx", &lo, &hi, &anchor) == 3) switch_profile_watch_range(lo, hi, anchor);
            }
            // profile_frames=A:B samples only frames A..B-1 (one hitch, instead of a whole run's average).
            if (const auto pf = g_ini.find("profile_frames"); pf != g_ini.end()) {
              unsigned a = 0, b = 0;
              if (std::sscanf(pf->second.c_str(), "%u:%u", &a, &b) == 2 && b > a) { switch_profile_frames(a, b); host::log("profile: frames %u..%u only", a, b - 1); }
            }
            if (switch_profile_start(&guest, 2000, &self)) host::log("profile: sampling the guest and render threads at 1 kHz each (Logs/profile.txt at exit)");
            else host::log("profile: cannot start the sampler");
          }
          try {
            async->run(run.done);
          } catch (...) {
            code = describe_current_exception(g_result);
            host::request_exit(code);          // the guest stops at its next retrace
            async->finish();
          }
          threadWaitForExit(&guest);
          threadClose(&guest);
          if (code == 0) { code = run.code; g_result = run.result; }
          host::log("renderer: %llu frames rendered, %llu stale frames dropped", (unsigned long long)async->rendered(), (unsigned long long)async->dropped());
#else
          GuestRun run;
          run_guest(run);
          code = run.code; g_result = run.result;
#endif
        }
      }
    } catch (...) {
      code = describe_current_exception(g_result);
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
  // The result first: the AOT diagnostics below are hundreds of lines, and an exit that outruns the
  // nxlink socket used to cut the log before the result line.
  host::log("switch result: exit=%d retraces=%u requested=%u", code, host::retrace_count(), host::options.frames);
  host::log("clocks: the CPU core measures %u MHz at the end of the run", switch_measure_cpu_mhz());
  ppc::log_aot_diagnostics();
  host::close_log_file();
  g_exit_code = code;
}
}  // namespace

extern "C" void userAppInit() {
  romfsInit();                 // Slippi's Sys folder ships in the NRO as romfs:/Sys
  socketInitializeDefault();
  nxlinkStdio();               // stdout to nxlink when a host is listening; harmless otherwise
  melee_nx_log_init();         // host::log hands lines to this writer thread instead of blocking on the socket
}
extern "C" void userAppExit() {
  socketExit();
  romfsExit();
}

namespace slippi { void finish_replay(); }   // exi_slippi.cpp: completes a replay cut off by quitting

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  // Runs on exit(), abort() and Aurora's FATAL: flush the log and, when sampling, write the profile
  // so a crash still leaves the hot-address table behind.
  switch_set_exit_hook([] { switch_hitch_stop(); slippi::finish_replay(); host::card_flush(); ppc::dump_branch_counts((std::string(kBaseDir) + "/Logs/branch_counts.txt").c_str()); switch_profile_stop((std::string(kBaseDir) + "/Logs/profile.txt").c_str()); host::close_log_file(); melee_nx_log_flush(); });
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
  // audio_dump=1: everything the game plays also goes to Logs/audio.wav (32 kHz stereo).
  if (get("audio_dump", "0") == "1") host::options.audio_dump = std::string(kBaseDir) + "/Logs/audio.wav";
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
  // Code the recompiler did not translate (Slippi's Gecko routines placed in high memory, such as
  // 80BECFF0) runs in the interpreter, as on the Mac. strict_aot=1 makes such a call fatal instead.
  ppc::set_interpreter_allowed(get("strict_aot", "0") != "1");
  const std::string script = get("script", "");
  if (!script.empty() && !host::input_load_script(script.c_str()))
    std::printf("cannot load input script %s\n", script.c_str());
  // Measurement aids read by hle_pad.cpp at the first pad poll: force_css=1 opens the character select
  // with P2 Fox and P3 Marth as CPUs (a script then needs only START); force_cpu=1..9 is their level.
  if (const std::string v = get("force_css", "0"); v == "1" || v == "4") setenv("MELEE_FORCE_CSS", v.c_str(), 1);
  if (get("force_teams", "0") == "1") setenv("MELEE_FORCE_TEAMS", "1", 1);
  if (const std::string v = get("force_stage", ""); !v.empty()) setenv("MELEE_FORCE_STAGE", v.c_str(), 1);
  if (const std::string level = get("force_cpu", ""); !level.empty()) setenv("MELEE_FORCE_CPU", level.c_str(), 1);
  // hle_verify=1: hooked matrix routines also run the original code in the interpreter and log differences.
  if (get("hle_verify", "0") == "1") setenv("MELEE_HLE_VERIFY", "1", 1);
  // bridge_verify=1: the Aurora bridge also converts every draw with full state comparison and logs any difference.
  if (get("bridge_verify", "0") == "1") setenv("MELEE_BRIDGE_VERIFY", "1", 1);
  // draw_merge=0: every primitive stays its own draw (the parser otherwise joins consecutive ones with identical state).
  if (get("draw_merge", "1") == "0") setenv("MELEE_DRAW_MERGE", "0", 1);
  // render_skip=0: never leave a draw pass out when the game is behind (it then runs slow instead).
  if (get("render_skip", "1") == "0") setenv("MELEE_RENDER_SKIP", "0", 1);
  if (!get("render_skip_ms", "").empty()) setenv("MELEE_RENDER_SKIP_MS", get("render_skip_ms", "").c_str(), 1);
  if (get("hitch_test", "0") == "1") setenv("MELEE_HITCH_TEST", "1", 1);
  // copy_history=1: the Aurora bridge's EFB-copy coverage warnings (diagnostic; costs render-thread time).
  if (get("copy_history", "0") == "1") setenv("MELEE_COPY_HISTORY", "1", 1);
  // profile_thread=fifo|worker: with profile=1, sample that Aurora thread instead of the renderer (pc1).
  if (const std::string t = get("profile_thread", ""); !t.empty()) setenv("MELEE_PROFILE_THREAD", t.c_str(), 1);
  // gxn_verify=1: every native GX call (decomp source compiled for the console) is compared with the translation.
  if (get("gxn_verify", "0") == "1") gxn::g_verify = true;
  // gml_verify=1: sampled whole-RAM comparison of natively compiled decomp C against the translation.
  if (get("gml_verify", "0") == "1") gml::g_verify = true;
  // gml_verify_frames=A:B: every native call in frames A..B is compared (not one in 512).
  if (const std::string v = get("gml_verify_frames", ""); !v.empty()) { gml::g_verify = true; setenv("MELEE_GML_VERIFY_FRAMES", v.c_str(), 1); }
  // frame_trace=1: GX counters every frame (first-divergence search between builds).
  if (get("frame_trace", "0") == "1") setenv("MELEE_FRAME_TRACE", "1", 1);
  if (const std::string v = get("audio_frames", ""); !v.empty()) setenv("MELEE_AUDIO_FRAMES", v.c_str(), 1);
  if (const std::string v = get("audio_slack_ms", ""); !v.empty()) setenv("MELEE_AUDIO_SLACK_MS", v.c_str(), 1);
  // gpu_thread=0: parse the GX stream on the simulation thread (as before the GPU thread) for A/B tests.
  if (get("gpu_thread", "1") == "0") setenv("MELEE_GPU_THREAD", "0", 1);
  // Core 2 belongs to the simulation thread: Aurora's shader compilers must never pin there.
  setenv("AURORA_RESERVED_CORE_MASK", "0x4", 0);
  if (get("shader_warmup", "1") == "0") setenv("AURORA_LAUNCH_WARM_SECONDS", "0", 1);
  // shader_warmup_all=1: one long warm-up session that compiles every pipeline in the seed
  // (sdmc:/switch/melee-nx/initial_pipeline_cache.db) with every core before the game starts.
  if (get("shader_warmup_all", "0") == "1") { setenv("AURORA_LAUNCH_WARM_ALL", "1", 1); setenv("AURORA_LAUNCH_WARM_HELPERS", "2", 1); }
  // uber=1: a draw whose pipeline is still compiling uses Aurora's uber-shader instead of waiting
  // for it (a first-seen pipeline otherwise stalls the renderer and then the game); 2 always.
  // The uber pipelines share one shader, so they need the shared Vulkan pipeline cache.
  // Screenshot aids: menu_open=<retrace> opens the in-game menu then, menu_page=controls|remap picks the page.
  if (const std::string v = get("menu_open", ""); !v.empty()) setenv("MELEE_MENU_OPEN", v.c_str(), 1);
  if (const std::string v = get("menu_page", ""); !v.empty()) setenv("MELEE_MENU_PAGE", v.c_str(), 1);
  if (const std::string v = get("uber", ""); !v.empty()) {
    setenv("AURORA_UBER", v.c_str(), 1);
    if (v != "0") setenv("AURORA_MONOLITHIC_PIPELINE_CACHE", "1", 1);
  }

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
      host::log("clocks: the CPU core measures %u MHz", switch_measure_cpu_mhz());
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
  host::log("execution: frames=%u fast=%s %s fp_profile=%s", o.frames, o.fast ? "true" : "false",
            ppc::interpreter_allowed() ? "interpreter fallback allowed" : "strict AOT", ppc::fp_profile_name(ppc::fp_profile()));
  host::log("paths: iso=%s sys=%s profile=%s cache=%s", o.iso.c_str(), o.sys_dir.c_str(), o.profile_dir.c_str(), o.cache_dir.c_str());

  // The simulation thread gets core 2 to itself at the highest priority an application may use
  // (0x1C); the main thread stays on core 1 and Aurora's workers float over cores 0-2. Sharing a core
  // at one priority made the kernel rotate the game out every 10 ms. 0x28 is the fallback when the
  // title we run under does not permit 0x1C.
  // Renderer build: this thread sets up and then owns Aurora on core 1 (the main thread there
  // only sleeps); the guest gets core 2 at the highest application priority from run_guest.
  // Headless build: this thread runs the guest itself.
  g_profiling = get("profile", "0") == "1";
  Thread sim;
#if MELEE_NX_AURORA
  const int sim_priority = 0x28, sim_core = 1;   // the same priority as Aurora's workers, so they cannot preempt the renderer
#else
  const int sim_priority = 0x1C, sim_core = 2;
#endif
  Result rc = threadCreate(&sim, simulation_thread, nullptr, nullptr, kSimulationStack, sim_priority, sim_core);
  if (R_FAILED(rc)) rc = threadCreate(&sim, simulation_thread, nullptr, nullptr, kSimulationStack, 0x2C, sim_core);
  if (R_FAILED(rc)) { std::printf("threadCreate failed: %08X\n", rc); consoleUpdate(nullptr); svcSleepThread(3'000'000'000ULL); consoleExit(nullptr); return 1; }
  threadStart(&sim);
#if !MELEE_NX_AURORA
  if (g_profiling) {
    if (switch_profile_start(&sim, 1000)) host::log("profile: sampling the simulation thread at 1 kHz (Logs/profile.txt at exit)");
    else host::log("profile: cannot start the sampler");
  }
#endif

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
    if (g_profiling) { switch_profile_stop((base + "/Logs/profile.txt").c_str()); host::log("profile: written to Logs/profile.txt"); }
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
  melee_nx_log_flush();
  melee_nx_log_shutdown();   // join the writer thread; a thread alive past main() makes hbloader report an error
  return g_exit_code < 0 ? 0 : g_exit_code.load();
}
