// Picture frontend for the native-engine runner. See frontend.cpp.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fe {
struct Options {
  bool picture = true;        // --headless: behave as the headless runner
  bool pace = true;           // --unpaced: do not wait for 60 Hz after each retrace
  bool vsync = true;          // --novsync
  bool null_backend = false;  // --gfx-null: decode and check every frame, draw nothing
  bool verbose = false;       // --gfx-log: Aurora's information messages too
  bool sync_decode = false;   // --gfx-sync: decode inside the game's flush instead of on the decoder thread
  bool warm_all = false;      // --warm-all: compile every pipeline in the cache at launch, not only those marked as drawn
  bool overclock = true;      // the runner's --no-overclock also keeps the GPU and memory clocks as they are
  bool stretch = false;       // --stretch: fill the 16:9 screen instead of showing the game's 4:3 picture
  // Play mode (--play): no script, the app's controller input, sound, no frame limit unless --frames is given.
  bool play = false;
  bool fresh_card = false;    // --fresh-card: empty the play memory card folder first
  int volume = -1;            // --volume 0..100; default 70 with --play, else no sound device
  std::string wav;            // --wav FILE: everything played, as the app's audio output writes it
  std::string audio_raw;      // --audio-raw FILE: the mixer's blocks exactly as it hands them over (right/left, host order)
  bool auto_vs = false;       // --auto-vs: a built-in controller script walks from boot into a VS match (Ness against a computer player)
  std::string play_script;    // --play-script FILE: a controller script in the app's format, through the app's input layer
  std::string iso;            // the runner's --iso and --frames, read here for play mode
  unsigned frames = 0;
  std::string settings;       // --settings FILE (default <out>/settings.ini)
  bool auto_menus = false;    // --auto-vs-menus: --auto-vs through the menus instead of the Boot to CSS code
  bool profile = false;       // --profile: per host service time on the simulation thread (the runner's hostprof)
  std::vector<std::string> rest;   // the arguments the frontend did not take (play mode reads the netplay ones from them)
  std::string cache_dir;      // --cache-dir DIR: Aurora's pipeline cache folder (default <out>/aurora)
  int width = 1280, height = 720;
  std::string out_dir;        // the runner's --out
  bool sync_net = false;      // --gfx-sync-net: with netplay, decode inside the game's flush (the earlier way; for comparison)
  bool log_direct = false;    // --log-direct: every line written where it is made (the earlier way; for comparison)
  int layout = 0;             // what fixedmap_startup said at the top of main (0: the game's fixed ranges are free)
  // The app (no arguments, or --app): play mode with the app's folders, launcher.ini, sign-in, in-game menu, HUD
  // and self-update (app.cpp). Paths below are filled by app_configure().
  bool app = false;
  std::string base_dir;       // sdmc:/switch/melee-nx-netplay (--base DIR off the console)
  std::string log_path;       // empty: <out>/picture.log
  std::string card_dir;       // empty: <out>/card-play
  std::string host_log_path;  // empty: <out>/host.log
  std::string net_dir;        // where the netplay host writes its end-of-run file (empty: <out>)
  std::string user_dir;       // Slippi user folder (empty: the netplay host's default)
  std::string self_path;      // this program's file (the self-update target)
  bool offline = false;       // launcher.ini offline=1: no online services in this session
  int online_delay = -1;      // launcher.ini online_delay: 0 automatic, 1..9 frames, -1 not given
};
extern Options options;
void say(const char* format, ...);   // stdout (nxlink) and <out>/picture.log; through the log thread while play mode's game runs
void log_open();                     // <out>/picture.log, appended to
void log_close();
void log_write(const char* text, size_t size, bool to_console, bool to_file);
// log_queue.cpp (play mode): lines are handed to a writer thread; the file is only written between matches
void log_queue_start();
void log_queue_stop();
void log_queue_match(bool fighting);
bool log_queue_running();
bool log_queue_push(const char* line);   // false when the queue is not running (the caller writes the line itself)
uint64_t log_queue_lines();
// play_io.cpp
void io_start(bool picture);         // run start: sound output (with a picture only), held log, raw sound dump
void io_stop();                      // run end: closes the sound output, prints its statistics and the held log lines
uint64_t io_last_pad_ns();           // when the game last read the controllers (0: never)
void io_retrace(uint32_t retrace, uint32_t scene_major, uint32_t scene_minor, bool fighting);   // once per retrace, simulation thread
double io_take_pad_ms();             // time spent reading controllers since the last call
void io_game_speed(double speed);    // online time sync's frame rate (net_host::set_speed_hook): the sound output follows it
// settings.cpp (play mode)
void settings_load(const std::string& path);
bool settings_vanilla();
bool settings_vanilla_only();            // settings.ini vanilla_only 1 or --vanilla-only: retail gameplay, own card, no mods
void settings_force_vanilla_only();      // --vanilla-only
void settings_online(bool on);       // the option words follow the online rules while this is on
void settings_deactivate();          // the host's own words again (the netplay runs that boot straight into a match)
int settings_online_delay();         // frames; 0: automatic
void settings_net_prefs(bool* direct_pause, int port_colour[2], uint32_t* shield_online, std::string skip[3]);   // patches/net-0004
bool settings_widescreen();
void settings_boot_to_css(bool on);
void settings_report();
const std::string& settings_profile();
uint32_t& settings_cpu_word();
uint32_t& settings_te_word(int which);       // 1 or 2
int& settings_te_music();
void settings_state_changed();
// mods.cpp (play mode)
std::string mods_configure(const std::string& profile);   // before the host starts; the profile's memory card folder
void mods_configure_costumes(const std::string& profile);  // a session that can go online: only the profile's look-only costumes
void mods_session_can_go_online();                        // skins are served in their online form (features/cosmetics.cpp)
bool mods_te_owned();
bool mods_hackpack();
uint32_t mods_flags();
uint32_t mods_stage_bits();
// slippi_files.cpp (play mode, `slippi_menus 1`)
bool settings_slippi_menus();
void slippi_files_build();
std::vector<std::string> slippi_files_publish();
int slippi_file_read(uint64_t offset, void* out, size_t bytes);
bool slippi_menus_on();
// settings.cpp: the in-game menu's view of settings.ini (app_menu.cpp)
std::string settings_get(const std::string& key);                    // the current value as settings.ini writes it
bool settings_change(const std::string& key, const std::string& value);   // sets it and writes settings.ini; false: unknown key
void settings_app_defaults(bool online_possible);                    // the app's defaults before settings.ini is read
bool settings_online_now();                                          // the online rules are in force (simulation thread's view)
bool settings_given(const std::string& key);                         // the key was in settings.ini
void settings_write_key(const std::string& key, const std::string& value);   // writes a line (a feature module's key) without reading it
void settings_set_quiet(const std::string& key, const std::string& value);   // sets it without writing the file
const std::string& settings_path();
// play_main.cpp
int play_main();
bool session_online();               // the session is online now (Slippi's online flow, a search or a match)
uint32_t online_delay_frames();      // the input delay in use (0: automatic, not yet chosen)

// ---- the app (app.cpp, app_menu.cpp, app_services.cpp) ----
bool app_wanted(int argc, char** argv);        // no arguments but the app's own: start as the app
void app_configure();                          // folders and launcher.ini; before log_open
void app_after_settings();                     // launcher.ini values that override settings.ini
void app_report();                             // the log lines saying what the app read
std::string app_ini(const std::string& key, const std::string& fallback);
// software_keyboard.cpp: the console's keyboard applet (blocks until OK or cancel; call it off the render thread in a match).
// `text` is the initial text and, on true, the words typed. Off the console: the MU_TEST_KEYBOARD test answers.
bool software_keyboard(const char* header, const char* guide, std::string& text);
void diagnostics_start();   // diagnostics.cpp (the app with a picture): Logs/crash.txt when the program is stopped by a signal
void app_ini_set(const std::string& key, const std::string& value);   // and writes launcher.ini
void app_backup_card();                        // the first session of this app copies the memory card once
// Simulation thread (lane 0, the game's poll) or the input-suggestion thread (lane 1), under play_io's input lock:
// remapping, deadzones and the rest of the controller setup (host::PadState[4]).
void app_pad_postprocess(void* pads4, int lane);
// controls.cpp: the controller setup beyond the app's button remap (controls_core.h), its menu pages, profiles,
// Dolphin import, the Start + D-pad + Z chords and the controller report rates.
bool controls_active();                        // some controller has a setup that differs from its defaults
uint32_t controls_test_adapter_ports();        // MU_CONTROLS_TEST_ADAPTER (Linux tests): ports read as adapter ports
void controls_apply_adapter(int port, void* pad, int lane);   // host::PadState of an adapter port
void controls_apply_sdl(const std::string& guid, void* sdl_gamepad, const void* before_app, void* pad, int trigger_press, int lane);
bool controls_capture_frame(const void* raw_pads4);   // a binding is being captured: true = the menu gets neutral pads
void controls_chords(void* pads4);             // Start + D-pad Down + Z, Start + D-pad Up + Z; Training's lone Start
void controls_menu_pages(void* extra_pages);   // appends the pages (std::vector<host::MenuExtraPage>)
void controls_saved(void (*put)(const std::string& id, const std::string& text));   // launcher.ini padx.<id>
void controls_load(const std::string& id, const std::string& text);
std::vector<std::string> controls_rate_lines();   // render thread: Pro Controller / Joy-Con report rates (console)
bool app_rumble_allowed(int port);
void app_retrace(uint32_t retrace, bool fighting, bool online);   // simulation thread, once per retrace
void app_menu_setup();                         // the in-game menu's settings and pages (Aurora builds)
void hud_frame(double work_ms);                // simulation thread: the frame time the HUD shows
void hud_log_line(const char* line);
int perf_history(float* sim_ms, int capacity);   // the newest `capacity` simulation-thread frame times (oldest first); count
void overlay_draw(int window_w, int window_h);   // overlay_draw.cpp: the overlays and picture options (render thread, Aurora builds)
void feature_overlay(void* overlay_frame, int window_w, int window_h);   // the feature modules' HUD lines (host::OverlayFrame)           // every queued log line: the ping and the input delay come from them
// The screens before the game (layout failure, disc missing, update, sign-in). Services program only for the
// update and sign-in; false when the screen could not be shown (no picture).
bool app_layout_screen(const char* reason);
bool app_message_screen(const std::string& headline, const std::vector<std::string>& lines);
enum class AppStart { Play, PlayOffline, Quit };
// app_services.cpp in the services program with a picture; null (weak) in the others. The update check against
// launcher.ini's update_host (prompt, download, swap), then the sign-in screen when `need_signin`.
__attribute__((weak)) AppStart app_services_run(bool check_update, bool need_signin, const std::string& user_dir);
extern std::string home_replay;               // play_main.cpp: a replay the home screen's Replay Viewer chose
__attribute__((weak)) bool app_notice_run(const std::string& headline, const std::vector<std::string>& lines);

// ---- frontend.cpp: the render thread before the game (app mode) ----
// The render thread starts early, owns Aurora and runs screen jobs on itself, then draws the game.
bool screens_start();                          // false: no picture (null build, --headless, the renderer failed)
bool screens_running();
// Runs `job` on the render thread (which owns Aurora); returns when it is done. The argument is the render
// thread's host::MetalFrontend (switch_signin.h's screens take it). False when the screens are not running.
bool screens_run(void (*job)(void* frontend, void* user), void* user);
void screens_stop();                           // the game will not start: Aurora shuts down
void window_size(int* w, int* h);              // the presented window's size (1280x720 before Aurora reports one)
// Presentation pacing (render thread). The simulation stays at 60 Hz; by default the render thread presents each
// finished game frame once (vsync paces it). A sub-frame presenter (for example melee-unlocked's Predict/Interpolate
// for 120 Hz displays) sets this before the game starts: after a frame has been drawn, present_again(backend, frame, i)
// is called for i = 1 .. presents_per_frame()-1 while no newer frame is waiting. `backend` is the gx::Backend
// (gx::AuroraBackend with a picture), `frame` the gx::Frame just drawn. Return false to stop early.
struct PresentHook {
  int (*presents_per_frame)() = nullptr;
  bool (*present_again)(void* backend, const void* frame, int index) = nullptr;
};
void set_present_hook(const PresentHook& hook);
double display_refresh_hz();                   // the display's refresh rate as SDL reports it (60 until known)
// services_check.cpp, linked into the services program only (null in the other): curl and TLS set-up, nothing contacted
__attribute__((weak)) bool services_self_check();
// frontend.cpp
void decoder_sync();                 // simulation thread: returns when the decoder thread has read everything handed to it
// Simulation-thread time other files measure, running totals in ns: [0] rollback state saves and loads, [1] waiting
// for the decoder before them, [2] every other netplay command. in_resim: the game is simulating frames again.
extern uint64_t sim_cost[3];
extern bool in_resim;
}  // namespace fe
