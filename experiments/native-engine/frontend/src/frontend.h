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
};
extern Options options;
void say(const char* format, ...);   // stdout (nxlink) and <out>/picture.log
// play_io.cpp
void io_start(bool picture);         // run start: sound output (with a picture only), held log, raw sound dump
void io_stop();                      // run end: closes the sound output, prints its statistics and the held log lines
uint64_t io_last_pad_ns();           // when the game last read the controllers (0: never)
void io_retrace(uint32_t retrace, uint32_t scene_major, uint32_t scene_minor, bool fighting);   // once per retrace, simulation thread
double io_take_pad_ms();             // time spent reading controllers since the last call
// settings.cpp (play mode)
void settings_load(const std::string& path);
bool settings_vanilla();
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
// play_main.cpp
int play_main();
}  // namespace fe
