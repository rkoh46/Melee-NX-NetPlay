// The display wave's renderer side (render thread, Aurora; the settings are src/features/gfx.cpp, the shared state
// src/gfx_live.h):
//   - texture packs: the enabled packs registered with Aurora's Dolphin-format replacement on a loader thread (frontend/patches/
//     0124), the textures already made dropped so the next frames look their replacements up; dumps as PNG in Dump/Textures/GALE01;
//   - ambient occlusion: overlay_draw.cpp's AuroraPicture carries live().ssao (patches/0125);
//   - the frame capture burst and the one-frame glitch detector: the presented game picture read back (patches/0126): channel 0
//     at 64 x 64 for melee-unlocked's flicker_scan.cpp (fetched tree, patches/0127), channel 1 at the picture's size, written as
//     PNG files into capture/ by a writer thread;
//   - the Lab view (lab.cpp) is drawn from overlay_draw.cpp.
// Nothing here touches the game: it reads Aurora's picture and files on the SD card.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx_live.h"
#include "frontend.h"
#include "flicker_scan.h"        // gen/gfx: melee-unlocked's glitch detector (patches/0127)
#include "video_bg.h"
#include "app/game_menu.h"       // host::menu_is_open (patches/0104): a capture asked for from the menu starts after it closes

#include <aurora/aurora.h>
#include <dolphin/gx.h>           // GXTexObj, which aurora/texture.hpp names
#include <aurora/texture.hpp>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace gx { std::string flicker_folder() { return fe::gfx::folder() + "/flicker"; } }   // patches/0127

namespace fe::gfx {
namespace {
using Clock = std::chrono::steady_clock;

void make_dirs(const std::string& path) {
  std::string at;
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') { if (!at.empty()) mkdir(at.c_str(), 0777); }
    if (i < path.size()) at += path[i];
  }
}
void set_status(const std::string& s) { std::lock_guard<std::mutex> lock(live().mutex); live().status = s; }

// ---- texture packs ----
// Files loose in a root (Load/Textures/GALE01 itself) are registered one by one so the folders inside it stay their own packs;
// Aurora reads them through this callback on its workers.
struct LooseDir { std::string dir; };
bool read_loose(void* user, const char* path, std::vector<uint8_t>& out) {
  const auto* d = static_cast<const LooseDir*>(user);
  FILE* f = std::fopen((d->dir + "/" + path).c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
  if (n <= 0) { std::fclose(f); return false; }
  out.resize(size_t(n));
  const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}
struct PackState {
  std::thread loader;
  uint32_t applied = ~0u;
  std::vector<aurora::texture::ReplacementGroup> groups;
  std::vector<aurora::texture::ReplacementRegistration> loose;
  std::vector<std::unique_ptr<LooseDir>> loose_dirs;
  bool budget_set = false;
} packs;

void load_packs(bool on, bool dump) {
  for (auto& g : packs.groups) aurora::texture::unregister_replacements(g);
  aurora::texture::unregister_replacements(packs.loose);
  packs.groups.clear(); packs.loose.clear(); packs.loose_dirs.clear();
  const std::string dump_dir = folder() + "/Dump/Textures/GALE01";
  aurora::texture::set_texture_dumps(dump, dump_dir);
  uint32_t loaded = 0; size_t before = 0;
  std::string names;
  if (on) {
    std::vector<std::string> off;
    { std::lock_guard<std::mutex> lock(live().mutex); off = live().packs_off; }
    const auto t0 = Clock::now();
    for (const PackInfo& p : scan_packs()) {
      if (std::find(off.begin(), off.end(), p.name) != off.end()) continue;
      before = aurora::texture::registered_replacement_count();
      if (p.loose) {
        auto dir = std::make_unique<LooseDir>(LooseDir{p.path});
        if (DIR* d = opendir(p.path.c_str())) {
          while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (!replacement_file(n) || is_dir(p.path + "/" + n)) continue;
            const auto r = aurora::texture::register_virtual_replacement(n, {read_loose, dir.get()});
            if (r.id) packs.loose.push_back(r);
          }
          closedir(d);
        }
        packs.loose_dirs.push_back(std::move(dir));
      } else {
        packs.groups.push_back(aurora::texture::load_replacement_directory(p.path));
      }
      ++loaded;
      names += (names.empty() ? "" : ", ") + p.name + " (" + std::to_string(aurora::texture::registered_replacement_count() - before) + ")";
    }
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    say("gfx: texture packs: %u loaded in %.0f ms, %zu replacements: %s", loaded, ms, aurora::texture::registered_replacement_count(), names.empty() ? "none installed" : names.c_str());
  }
  live().packs_loaded.store(loaded);
  live().replacements.store(uint32_t(aurora::texture::registered_replacement_count()));
  if (dump) say("gfx: dumping every texture the game draws into %s", dump_dir.c_str());
  aurora::texture::reload_textures();
  live().packs_ready.fetch_add(1, std::memory_order_relaxed);
  set_status(!on && !dump ? std::string("Off") : std::to_string(loaded) + " packs, " + std::to_string(aurora::texture::registered_replacement_count()) +
             " replacements" + (dump ? ", dumping" : ""));
}

void textures_frame() {
  const uint32_t gen = live().textures_generation.load(std::memory_order_relaxed);
  if (gen == packs.applied) return;
  const bool on = live().textures.load(), dump = live().dump.load();
  if (packs.applied == ~0u && !on && !dump) { packs.applied = gen; return; }   // never on this run: Aurora is left as it is
  packs.applied = gen;
  if (!packs.budget_set) {
    // Decoded replacements stay in memory within this budget (Aurora's default is 4 GB; the console has far less).
#ifdef __SWITCH__
    aurora::texture::set_replacement_cache_budget(384ull << 20);
#else
    aurora::texture::set_replacement_cache_budget(1024ull << 20);
#endif
    packs.budget_set = true;
  }
  if (packs.loader.joinable()) packs.loader.join();
  set_status("Loading...");
  packs.loader = std::thread(load_packs, on, dump);   // directory scans of the SD card off the render thread
}

// ---- the read-back picture: glitch detector and capture burst ----
gx::FlickerScanner* scanner = nullptr;
std::mutex flicker_mutex;
// The GPU hands frames back in whatever order their mappings complete: they are put back in presentation order (the
// detector compares each frame with the one before and after it) and pushed once the frame before them has been.
std::map<uint64_t, std::vector<float>> flicker_pending;
uint64_t flicker_next = 0;
void flicker_push(uint64_t frame, const std::vector<float>& grid) {
  if (!scanner) scanner = new gx::FlickerScanner();
  scanner->push(grid.data(), uint32_t(frame));
  live().flicker_frames.fetch_add(1, std::memory_order_relaxed);
  live().flicker_hits.store(scanner->hits(), std::memory_order_relaxed);
  live().flicker_cells.store(scanner->cell_hits(), std::memory_order_relaxed);
}
void flicker_frame(void*, uint64_t frame, uint32_t w, uint32_t h, const uint8_t* rgba) {
  if (w != gx::FlickerScanner::kGrid || h != gx::FlickerScanner::kGrid) return;
  std::vector<float> grid(size_t(w) * h);
  for (uint32_t i = 0; i < w * h; ++i) grid[i] = 0.299f * rgba[i * 4] + 0.587f * rgba[i * 4 + 1] + 0.114f * rgba[i * 4 + 2];
  std::lock_guard<std::mutex> lock(flicker_mutex);
  if (frame < flicker_next) return;   // came back after its place was given up
  flicker_pending[frame] = std::move(grid);
  if (!flicker_next) flicker_next = flicker_pending.begin()->first;
  while (!flicker_pending.empty() && (flicker_pending.begin()->first == flicker_next || flicker_pending.size() > 6)) {
    auto it = flicker_pending.begin();
    flicker_push(it->first, it->second);
    flicker_next = it->first + 1;
    flicker_pending.erase(it);
  }
}

// PNG with zlib (the capture files): RGB, rows filtered "none".
void put32(std::vector<uint8_t>& v, uint32_t x) { v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); }
void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> head; put32(head, uint32_t(data.size()));
  std::fwrite(head.data(), 1, 4, f);
  uLong crc = crc32(0L, reinterpret_cast<const Bytef*>(type), 4);
  if (!data.empty()) crc = crc32(crc, data.data(), uInt(data.size()));
  std::fwrite(type, 1, 4, f);
  if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
  std::vector<uint8_t> tail; put32(tail, uint32_t(crc));
  std::fwrite(tail.data(), 1, 4, f);
}
bool write_png(const std::string& path, uint32_t w, uint32_t h, const uint8_t* rgba, bool alpha = false) {
  const uint32_t channels = alpha ? 4 : 3;
  std::vector<uint8_t> raw; raw.reserve(size_t(w * channels + 1) * h);
  for (uint32_t y = 0; y < h; ++y) {
    raw.push_back(0);
    const uint8_t* row = rgba + size_t(y) * w * 4;
    if (alpha) raw.insert(raw.end(), row, row + size_t(w) * 4);
    else for (uint32_t x = 0; x < w; ++x) { raw.push_back(row[x * 4]); raw.push_back(row[x * 4 + 1]); raw.push_back(row[x * 4 + 2]); }
  }
  uLongf packed_size = compressBound(uLong(raw.size()));
  std::vector<uint8_t> packed(packed_size);
  if (compress2(packed.data(), &packed_size, raw.data(), uLong(raw.size()), 1) != Z_OK) return false;
  packed.resize(packed_size);
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::fwrite(sig, 1, 8, f);
  std::vector<uint8_t> ihdr; put32(ihdr, w); put32(ihdr, h); ihdr.insert(ihdr.end(), {8, uint8_t(alpha ? 6 : 2), 0, 0, 0});
  chunk(f, "IHDR", ihdr); chunk(f, "IDAT", packed); chunk(f, "IEND", {});
  return std::fclose(f) == 0;
}

struct Shot { uint64_t frame; uint32_t w, h; std::vector<uint8_t> rgba; };
struct Capture {
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<Shot> queue;
  size_t queued_bytes = 0;
  uint32_t wanted = 0, taken = 0, skipped = 0, written = 0, burst = 0;
  bool armed = false, stop = false, delayed = false;
  Clock::time_point start_at{};
  std::thread writer;
  std::string dir;
} cap;
#ifdef __SWITCH__
constexpr size_t kCaptureQueueBytes = 96u << 20;    // frames waiting for the SD card; beyond this a frame is skipped
#else
constexpr size_t kCaptureQueueBytes = 512u << 20;
#endif

void capture_frame(void*, uint64_t frame, uint32_t w, uint32_t h, const uint8_t* rgba) {
  std::lock_guard<std::mutex> lock(cap.mutex);
  if (!cap.armed || cap.taken >= cap.wanted) return;
  ++cap.taken;
  const size_t bytes = size_t(w) * h * 4;
  if (cap.queued_bytes + bytes > kCaptureQueueBytes) { ++cap.skipped; return; }
  cap.queue.push_back({frame, w, h, std::vector<uint8_t>(rgba, rgba + bytes)});
  cap.queued_bytes += bytes;
  cap.wake.notify_one();
}
void capture_writer() {
  std::unique_lock<std::mutex> lock(cap.mutex);
  for (;;) {
    cap.wake.wait(lock, [] { return cap.stop || !cap.queue.empty(); });
    if (cap.queue.empty() && cap.stop) return;
    Shot s = std::move(cap.queue.front());
    cap.queue.pop_front();
    cap.queued_bytes -= s.rgba.size();
    const std::string dir = cap.dir;
    const uint32_t index = cap.written + cap.skipped;   // frames are numbered in the order presented
    lock.unlock();
    char name[64];
    std::snprintf(name, sizeof name, "/blink_%03u_frame%llu.png", index, (unsigned long long)s.frame);
    const bool ok = write_png(dir + name, s.w, s.h, s.rgba.data());
    lock.lock();
    if (ok) { ++cap.written; live().captured.fetch_add(1, std::memory_order_relaxed); }
    else ++cap.skipped;
  }
}

void readback_frame() {
  // Glitch detector: channel 0, every presented frame at 64 x 64, while it is on.
  static bool flicker_on = false;
  const bool want_flicker = live().flicker.load(std::memory_order_relaxed);
  if (want_flicker != flicker_on) {
    flicker_on = want_flicker;
    aurora_set_frame_readback(0, gx::FlickerScanner::kGrid, gx::FlickerScanner::kGrid, want_flicker ? flicker_frame : nullptr, nullptr);
    say("gfx: one-frame glitch detector %s%s", want_flicker ? "on: reports go to the log, the grids into " : "off", want_flicker ? gx::flicker_folder().c_str() : "");
  }
  // Capture burst: channel 1 at the picture's size for the next N presented frames.
  const uint32_t request = live().capture_request.exchange(0, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(cap.mutex);
  if (request) {
    cap.wanted = request & 0x7FFFFFFFu;
    cap.delayed = (request & 0x80000000u) != 0;
    cap.start_at = Clock::now() + std::chrono::seconds(3);
    cap.taken = 0; cap.skipped = 0; cap.written = 0; cap.armed = false;
    cap.dir = folder() + "/capture/burst-" + std::to_string(++cap.burst);
    make_dirs(cap.dir);
    if (!cap.writer.joinable()) cap.writer = std::thread(capture_writer);
    if (!cap.delayed) { cap.armed = true; aurora_set_frame_readback(1, 0, 0, capture_frame, nullptr); say("gfx: capturing %u frames into %s", cap.wanted, cap.dir.c_str()); }
  }
  if (cap.delayed && !cap.armed && cap.wanted) {
    if (host::menu_is_open()) cap.start_at = Clock::now() + std::chrono::seconds(3);
    else if (Clock::now() >= cap.start_at) {
      cap.armed = true; cap.delayed = false;
      aurora_set_frame_readback(1, 0, 0, capture_frame, nullptr);
      say("gfx: capturing %u frames into %s", cap.wanted, cap.dir.c_str());
    }
  }
  if (cap.armed && cap.taken >= cap.wanted) {
    cap.armed = false;
    aurora_set_frame_readback(1, 0, 0, nullptr, nullptr);
    say("gfx: capture burst taken: %u frames (%u skipped: the SD card was slower than the game), writing into %s", cap.taken, cap.skipped, cap.dir.c_str());
  }
}

// Test aid: MU_TEST_FLICKER_AT=N makes the N-th frame drawn here (and only it) twice as bright, a one-frame glitch for the
// detector to find (verification runs).
void test_flash() {
  static const char* at = std::getenv("MU_TEST_FLICKER_AT");
  if (!at) return;
  static uint32_t frames = 0;
  const uint32_t n = ++frames, when = uint32_t(std::atoi(at));
  if (n == when) {
    AuroraPicture p{2.0f, 1.3f, 1.0f, 0.0f, 0, 1.0f, 0.0f};
    aurora_set_picture(&p);
    say("gfx: test: frame %u drawn twice as bright (MU_TEST_FLICKER_AT)", n);
  } else if (n == when + 1) {
    live().picture_dirty.store(true);
  }
}
}  // namespace

// A paletted texture the renderer decoded itself, under its Dolphin name (patches/0128): written once, counted with Aurora's dumps.
void dump_texture(const std::string& name, uint32_t width, uint32_t height, const uint8_t* rgba) {
  const std::string path = folder() + "/Dump/Textures/GALE01/" + name;
  struct stat st{};
  if (name.empty() || stat(path.c_str(), &st) == 0) return;
  if (write_png(path, width, height, rgba, true)) aurora::texture::count_host_dump();
}

// Render thread, from overlay_draw() each frame.
void render_frame() {
  textures_frame();
  readback_frame();
  test_flash();
}

// Render thread, before Aurora shuts down: the loader and the writer finish, the read-back channels close.
extern const int lab_linked;   // lab.cpp: keeps the Lab view in the program (see there)
#if MU_FE_VIDEO
namespace video_anchor { }
}  // namespace fe::gfx
namespace fe::video_bg { extern const int linked; }   // video_bg.cpp: keeps it in the program (see there)
namespace fe::gfx {
#endif
void render_end() {
#if MU_FE_VIDEO
  if (!fe::video_bg::linked) return;
#endif
  if (lab_linked && &lab_end) lab_end();
  if (&fe::video_bg::shutdown) fe::video_bg::shutdown();
  if (packs.applied != ~0u) {
    live().dumped.store(aurora::texture::dumped_texture_count());
    if (live().dump.load()) say("gfx: texture dumps: %llu written this run", (unsigned long long)live().dumped.load());
  }
  aurora_set_frame_readback(0, 0, 0, nullptr, nullptr);
  aurora_set_frame_readback(1, 0, 0, nullptr, nullptr);
  if (packs.loader.joinable()) packs.loader.join();
  {
    std::lock_guard<std::mutex> lock(cap.mutex);
    cap.stop = true; cap.armed = false;
  }
  cap.wake.notify_all();
  if (cap.writer.joinable()) cap.writer.join();
  if (cap.burst) say("gfx: capture: %u frames written, %u skipped in the last burst (%s)", cap.written, cap.skipped, cap.dir.c_str());
}
}  // namespace fe::gfx
