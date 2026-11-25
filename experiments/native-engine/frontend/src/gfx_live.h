// The graphics options of the display wave (0.4.8) shared by the settings module (src/features/gfx.cpp, simulation and menu
// threads) and the renderer side (src/gfx_render.cpp, render thread, Aurora): texture packs and their dumps, screen-space
// ambient occlusion, the frame capture burst, the one-frame glitch detector, the Lab view and its looks, looping menu video
// backgrounds and RetroArch shader presets. Everything here is display only: none of it reaches game memory, so the
// simulation, replays and online play are the same with any of it on (each one off by default, as upstream ships it).
// Relaxed atomics: a frame drawn half with an old value is harmless. Strings under `mutex`.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <cctype>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "frontend.h"

namespace fe::gfx {

struct Live {
  // Texture packs (upstream gx/texture_pack.cpp; Aurora's Dolphin-format replacement here, frontend/patches/0124).
  std::atomic<bool> textures{false};          // settings.ini customtextures
  std::atomic<bool> dump{false};              // dumptextures: every texture the game draws written once as a PNG
  std::atomic<uint32_t> textures_generation{0};
  std::vector<std::string> packs_off;         // texpackoff: packs switched off by name (under mutex)
  // Screen-space ambient occlusion (ssao 0..1), in Aurora's present pass (patches/0125).
  std::atomic<float> ssao{0.0f};
  // Frame capture burst (upstream capture_burst, Ctrl+F2): the next N presented frames as PNG files. Requests come from the
  // menu row or the chord; capture_frames counts down on the render side.
  std::atomic<uint32_t> capture_request{0};
  std::atomic<uint32_t> capture_size{90};      // capture_burst_frames
  // One-frame glitch detector (upstream --flicker-scan / flickerscan).
  std::atomic<bool> flicker{false};
  std::atomic<bool> picture_dirty{false};          // overlay_draw.cpp sets the picture options again (after a test's one-frame flash)
  // Lab view (upstream gx/lab_view.cpp): on, its fighter and stage looks, colours.
  std::atomic<bool> lab{false};
  std::atomic<int> lab_fighters{0}, lab_stage{0};   // fighters 0 silhouettes, 1 stick figures, 2 capsules, 3 blobs; stage 0 as drawn, 1 outline, 2 blocks, 3 hidden
  std::atomic<uint32_t> lab_colour[4] = {};         // lab_colour_0..3: 0 the look's own, else 0x1000000 | RGB
  std::atomic<uint32_t> lab_background{0}, lab_stage_colour{0};
  std::atomic<bool> lab_skip_scene{true};           // labskipscene: the 3D scene is not drawn under a covering Lab view
  std::atomic<bool> lab_covering{false};            // render side: the newest Lab view draw covered the window
  // Looping menu video backgrounds (upstream gx/video_background.cpp, videobackgrounds).
  std::atomic<bool> video_backgrounds{false};
  // RetroArch shader presets (upstream gx/user_shader.cpp, shaderpreset).
  std::string shader_preset;                       // file name in <folder>/Shaders (under mutex), empty: off
  std::atomic<uint32_t> shader_generation{0};
  std::mutex mutex;
  // What the render side reports back (for the menu and the end-of-run lines).
  std::atomic<uint32_t> packs_loaded{0}, replacements{0};
  std::atomic<uint32_t> packs_ready{0};            // bumped each time the loader has finished (gx_aurora.cpp looks paletted textures up again)
  std::atomic<uint64_t> dumped{0}, captured{0}, flicker_hits{0}, flicker_cells{0}, flicker_frames{0};
  std::string status;                              // a line for the menu (under mutex)
};
inline Live& live() { static Live l; return l; }

// gfx_render.cpp (builds with a picture): once per frame from overlay_draw(), and before Aurora shuts down. Render thread.
void render_frame();
void render_end();
// A texture the renderer decoded itself (paletted textures, patches/0128), dumped as a PNG under its Dolphin name while dumping.
void dump_texture(const std::string& name, uint32_t width, uint32_t height, const uint8_t* rgba);
// lab.cpp (builds with a picture; weak for the feature modules): melee-unlocked's Lab view. lab_retrace on the simulation thread
// once per retrace (the game's look snapshot, only while the view is on), lab_draw on the render thread inside the ImGui frame,
// the fighter packs' download (services program) and its state for the menu, lab_end before Aurora goes.
__attribute__((weak)) void lab_retrace(const std::string names[4]);
__attribute__((weak)) void lab_draw(int window_w, int window_h);
__attribute__((weak)) void lab_download();
__attribute__((weak)) std::string lab_status();
__attribute__((weak)) void lab_end();

// The folder texture packs, dumps, captures, the Lab view's packs, videos and shaders live in: the app's folder
// (sdmc:/switch/melee-nx-netplay) or the run's output folder off the console.
inline std::string folder() { return options.app && !options.base_dir.empty() ? options.base_dir : options.out_dir; }

inline bool is_dir(const std::string& path) { struct stat st{}; return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
inline bool replacement_file(const std::string& name) {
  if (name.rfind("tex1_", 0) != 0 || name.size() < 9) return false;
  std::string ext = name.substr(name.size() - 4);
  for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".png" || ext == ".dds";
}
// The installed packs (upstream texpack::refresh_packs): every folder in Load/Textures/GALE01 (Dolphin's place) and in
// TexturePacks is one pack; files loose in either root are one more, named after the root. File names only, nothing decoded.
struct PackInfo { std::string name, path; bool loose = false; uint32_t files = 0; };
inline uint32_t count_files(const std::string& dir, int depth) {
  uint32_t n = 0;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      const std::string name = e->d_name;
      if (name.empty() || name[0] == '.') continue;
      const std::string path = dir + "/" + name;
      if (is_dir(path)) { if (depth < 8) n += count_files(path, depth + 1); }
      else if (replacement_file(name)) ++n;
    }
    closedir(d);
  }
  return n;
}
inline std::vector<PackInfo> scan_packs() {
  std::vector<PackInfo> out;
  const std::string base = folder();
  for (const char* root_name : {"Load/Textures/GALE01", "TexturePacks"}) {
    const std::string root = base + "/" + root_name;
    DIR* d = opendir(root.c_str());
    if (!d) continue;
    uint32_t loose = 0;
    std::vector<std::string> dirs;
    while (dirent* e = readdir(d)) {
      const std::string name = e->d_name;
      if (name.empty() || name[0] == '.') continue;
      if (is_dir(root + "/" + name)) dirs.push_back(name);
      else if (replacement_file(name)) ++loose;
    }
    closedir(d);
    std::sort(dirs.begin(), dirs.end());
    if (loose) out.push_back({root_name, root, true, loose});
    for (const auto& n : dirs) out.push_back({n, root + "/" + n, false, count_files(root + "/" + n, 0)});
  }
  return out;
}
}  // namespace fe::gfx
