// Looping menu video backgrounds (melee-unlocked's gx/video_background.cpp, videobackgrounds): VideoBackgrounds/sss.mp4 plays in
// place of the stage select's background. Presentation only: decoded frames never enter game memory. Upstream's rules are kept:
// its scene slots and the stage select's learned background texture (video_background_learning.h, compiled from the fetched
// tree), its stage select matte (video_background_style.h), the learned target kept in sss.target. Decoding is upstream's Linux
// path, linux_media.cpp's VideoReader (FFmpeg; devkitPro's FFmpeg on the console), on a thread of its own, paced to the video's
// own timestamps and only while its screen is up. Render thread: gx_aurora.cpp (frontend/patches/0130) calls begin_frame and
// the texture lookup. The character select's video, css.mp4, has no texture to stand in for (upstream: its backdrop is a long run
// of untextured draws), so it is drawn as upstream draws it, a full-screen layer before the first textured draw after them
// (frontend/patches/0131, fullscreen_frame and fullscreen_layer_before); patches/0132 lets that layer's GX library calls through.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_bg.h"
#include "gfx_live.h"
#include "video_background_learning.h"   // melee-unlocked port/runtime/gx (the fetched tree)
#include "video_background_style.h"
#include "full/linux_media.h"            // melee-unlocked port/linux (the fetched tree)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace fe::video_bg {
// gfx_render.cpp refers to this (MU_FE_VIDEO), so this object is linked: the renderer reaches it only through weak references.
extern const int linked = 1;
namespace learning = gx::video_bg::learning;
namespace {
using Clock = std::chrono::steady_clock;

// One slot's decoder: frames as RGBA rows from the top (upstream's BGRA bottom-up turned around here, with its matte).
class Decoder {
 public:
  ~Decoder() { stop(); }
  void start(const std::string& path, bool sss_matte) {
    if (thread_.joinable()) return;
    path_ = path; matte_ = sss_matte; stopping_ = false;
    thread_ = std::thread([this] { run(); });
  }
  void stop() {
    stopping_ = true;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }
  void set_active(bool on) {
    if (active_.exchange(on) != on) cv_.notify_all();
  }
  std::shared_ptr<const Frame> frame() { std::lock_guard<std::mutex> lock(mutex_); return frame_; }
  std::string error() { std::lock_guard<std::mutex> lock(mutex_); return error_; }
 private:
  bool wait_active() {
    std::unique_lock<std::mutex> lock(wait_mutex_);
    cv_.wait(lock, [&] { return stopping_.load() || active_.load(); });
    return !stopping_;
  }
  bool wait_until(Clock::time_point t) {
    std::unique_lock<std::mutex> lock(wait_mutex_);
    cv_.wait_until(lock, t, [&] { return stopping_.load() || !active_.load(); });
    return !stopping_ && active_;
  }
  void set_error(const std::string& e) { std::lock_guard<std::mutex> lock(mutex_); error_ = e; say("video backgrounds: %s: %s", path_.c_str(), e.c_str()); }
  void run() {
    linux_media::VideoReader reader;
    std::string error;
    if (!reader.open(path_, &error)) { set_error("cannot open the video: " + error); return; }
    const uint32_t width = reader.width(), height = reader.height();
    // The console decodes in software on the cores the renderer shares: a picture larger than 1280 x 720 is refused there.
#ifdef __SWITCH__
    const uint32_t max_w = 1280, max_h = 720;
#else
    const uint32_t max_w = 3840, max_h = 2160;
#endif
    if (!width || !height || width > max_w || height > max_h) {
      set_error("the video is " + std::to_string(width) + "x" + std::to_string(height) + "; at most " + std::to_string(max_w) + "x" + std::to_string(max_h));
      return;
    }
    say("video backgrounds: %s: %ux%u", path_.c_str(), width, height);
    std::vector<uint8_t> mask;
    if (matte_) {
      mask.resize(size_t(width) * height);
      for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) mask[size_t(y) * width + x] = gx::video_bg::style::sss_matte_alpha(x, y, width, height);
    }
    std::vector<uint8_t> bgra;
    uint64_t serial = 0;
    while (!stopping_) {
      if (!wait_active()) break;
      if (serial) reader.rewind();
      const auto wall_start = Clock::now();
      int64_t first_time = -1;
      bool failed = false;
      while (!stopping_ && active_) {
        int64_t sample_time = 0;
        const int got = reader.read(bgra, &sample_time, &error);
        if (got < 0) { set_error(error); failed = true; break; }
        if (got == 0) break;   // the end: rewind and loop
        if (first_time < 0) first_time = sample_time;
        if (!wait_until(wall_start + std::chrono::nanoseconds((sample_time - first_time) * 100))) break;
        auto next = std::make_shared<Frame>();
        next->width = width; next->height = height; next->serial = ++serial;
        next->rgba.resize(size_t(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y) {
          const uint8_t* src = bgra.data() + size_t(height - 1 - y) * width * 4;   // bottom row first
          uint8_t* dst = next->rgba.data() + size_t(y) * width * 4;
          const uint8_t* m = matte_ ? mask.data() + size_t(y) * width : nullptr;
          for (uint32_t x = 0; x < width; ++x) {
            uint32_t b = src[x * 4], g = src[x * 4 + 1], r = src[x * 4 + 2];
            if (m && m[x]) {   // upstream's slate panel behind the stage grid
              const uint32_t a = m[x];
              b = (b * (255 - a) + 76u * a + 127) / 255; g = (g * (255 - a) + 55u * a + 127) / 255; r = (r * (255 - a) + 45u * a + 127) / 255;
            }
            dst[x * 4] = uint8_t(r); dst[x * 4 + 1] = uint8_t(g); dst[x * 4 + 2] = uint8_t(b); dst[x * 4 + 3] = 255;
          }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        frame_ = std::move(next);
        error_.clear();
      }
      if (failed) {
        std::unique_lock<std::mutex> lock(wait_mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(500), [&] { return stopping_.load(); });
      }
    }
  }
  std::string path_;
  bool matte_ = false;
  std::thread thread_;
  std::atomic<bool> stopping_{false}, active_{false};
  std::mutex mutex_, wait_mutex_;
  std::condition_variable_any cv_;
  std::shared_ptr<const Frame> frame_;
  std::string error_;
};

struct Candidate { uint32_t width = 0, height = 0; unsigned observations = 0, distinct_frames = 0, last_learning_frame = ~0u; };
struct Slot {
  const char* stem = "";
  std::string video_path, target_path, target;
  std::map<std::string, Candidate> candidates;
  unsigned learning_frames = 0, saved_target_frames = 0;
  bool target_confirmed = false, learning_failed = false, started = false, has_video = false, logged = false;
  Decoder decoder;
  Clock::time_point checked{};
};

uint64_t score(const Candidate& c) { return learning::candidate_score(c.width, c.height, c.distinct_frames, c.observations); }

class Manager {
 public:
  Manager() {
    root_ = fe::gfx::folder() + "/VideoBackgrounds";
    mkdir(root_.c_str(), 0777);
    slots_[0].stem = "css"; slots_[1].stem = "sss";
    for (Slot& s : slots_) {
      s.video_path = root_ + "/" + s.stem + ".mp4";
      s.target_path = root_ + "/" + s.stem + ".target";
      if (FILE* f = std::fopen(s.target_path.c_str(), "r")) {
        char line[256] = {};
        if (std::fgets(line, sizeof line, f)) { s.target = line; while (!s.target.empty() && (s.target.back() == '\n' || s.target.back() == '\r')) s.target.pop_back(); }
        std::fclose(f);
      }
      if (s.target.rfind("tex1_", 0) != 0) s.target.clear();
    }
  }
  ~Manager() { for (Slot& s : slots_) s.decoder.stop(); }

  void begin(uint8_t major, uint8_t minor) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool enabled = fe::gfx::live().video_backgrounds.load(std::memory_order_relaxed);
    const int next = learning::scene_slot(major, minor);
    active_ = next;
    for (int i = 0; i < 2; ++i) {
      Slot& s = slots_[i];
      // Looked for once a second (a file dropped in shows within a second).
      if (enabled && Clock::now() - s.checked >= std::chrono::seconds(1)) {
        struct stat st{};
        s.has_video = stat(s.video_path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
        s.checked = Clock::now();
      }
      if (!enabled) s.has_video = false;
      if (s.has_video && !s.started) { s.started = true; s.decoder.start(s.video_path, i == 1); say("video backgrounds: found %s.mp4", s.stem); }
      s.decoder.set_active(s.has_video && i == active_);
    }
    if (active_ >= 0) {
      Slot& s = slots_[active_];
      if (!learning::uses_texture_target(active_)) return;
      if (!s.target.empty() && !s.target_confirmed && s.has_video && ++s.saved_target_frames >= learning::kSavedTargetTimeoutFrames) {
        say("video backgrounds: saved %s target %s was not seen; learning again", s.stem, s.target.c_str());
        s.target.clear(); s.saved_target_frames = 0; s.learning_frames = 0; std::remove(s.target_path.c_str());
      }
      if (s.target.empty() && s.has_video && !s.learning_failed) {
        ++s.learning_frames;
        if (s.learning_frames == learning::kDecisionFrames) try_learning(s, false);
        if (s.learning_frames >= learning::kTimeoutFrames && s.target.empty()) try_learning(s, true);
      }
    }
  }
  bool wants_names() {
    std::lock_guard<std::mutex> lock(mutex_);
    return wants_locked() && learning::uses_texture_target(active_);
  }
  std::shared_ptr<const Frame> lookup(const std::string& name, uint32_t w, uint32_t h) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!wants_locked() || !learning::uses_texture_target(active_)) return {};
    Slot& s = slots_[active_];
    if (s.target.empty() || !s.target_confirmed) record(s, name, w, h);
    if (!s.target.empty() && name == s.target && !s.target_confirmed) {
      s.target_confirmed = true; s.saved_target_frames = 0;
      say("video backgrounds: saved %s target %s seen (%ux%u)", s.stem, s.target.c_str(), w, h);
    }
    if (s.target.empty() || name != s.target) return {};
    auto f = s.decoder.frame();
    if (f && !s.logged) { s.logged = true; say("video backgrounds: the %s video replaces %s (%ux%u frames)", s.stem, s.target.c_str(), f->width, f->height); }
    return f;
  }
  std::shared_ptr<const Frame> fullscreen() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!wants_locked() || active_ != 0) return {};
    Slot& s = slots_[active_];
    auto f = s.decoder.frame();
    if (f && !s.logged) { s.logged = true; say("video backgrounds: the %s video plays behind the character select (%ux%u frames)", s.stem, f->width, f->height); }
    return f;
  }
  std::string status(int i) {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot& s = slots_[i];
    if (!fe::gfx::live().video_backgrounds.load()) return "Off";
    if (!s.has_video) return std::string("No ") + s.stem + ".mp4";
    const std::string e = s.decoder.error();
    if (!e.empty()) return e;
    if (i == 0) return s.decoder.frame() ? "Ready: full-screen video layer" : "Ready";
    if (!learning::uses_texture_target(i)) return "Ready";
    if (s.learning_failed) return "Could not learn the background";
    if (s.target.empty()) return active_ == i ? "Learning the background..." : "Visit the screen to learn it";
    return "Ready";
  }
  void relearn(int i) {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot& s = slots_[i];
    s.target.clear(); s.candidates.clear(); s.learning_frames = 0; s.saved_target_frames = 0; s.target_confirmed = false; s.learning_failed = false;
    std::remove(s.target_path.c_str());
    say("video backgrounds: %s target cleared", s.stem);
  }
  void stop() { for (Slot& s : slots_) s.decoder.stop(); }
 private:
  bool wants_locked() const {
    return learning::should_route_video(fe::gfx::live().video_backgrounds.load(std::memory_order_relaxed), active_, active_ >= 0 && slots_[active_].has_video);
  }
  void record(Slot& s, const std::string& name, uint32_t w, uint32_t h) {
    if (name.empty() || !w || !h || w > 4096 || h > 4096) return;
    auto it = s.candidates.find(name);
    if (it == s.candidates.end()) {
      if (s.candidates.size() >= learning::kMaximumCandidates) {
        auto weakest = std::min_element(s.candidates.begin(), s.candidates.end(), [](const auto& a, const auto& b) { return score(a.second) < score(b.second); });
        Candidate incoming; incoming.width = w; incoming.height = h; incoming.observations = incoming.distinct_frames = 1;
        if (weakest != s.candidates.end() && score(incoming) <= score(weakest->second)) return;
        if (weakest != s.candidates.end()) s.candidates.erase(weakest);
      }
      it = s.candidates.emplace(name, Candidate{}).first;
    }
    Candidate& c = it->second;
    c.width = w; c.height = h; ++c.observations;
    if (c.last_learning_frame != s.learning_frames) { c.last_learning_frame = s.learning_frames; ++c.distinct_frames; }
  }
  void try_learning(Slot& s, bool final_attempt) {
    auto best = std::max_element(s.candidates.begin(), s.candidates.end(), [](const auto& a, const auto& b) { return score(a.second) < score(b.second); });
    if (best != s.candidates.end() && learning::confident(best->second.width, best->second.height, best->second.distinct_frames, s.learning_frames, best->second.observations)) {
      s.target = best->first; s.target_confirmed = true; s.learning_failed = false;
      if (FILE* f = std::fopen(s.target_path.c_str(), "w")) { std::fprintf(f, "%s\n", s.target.c_str()); std::fclose(f); }
      say("video backgrounds: learned the %s background %s %ux%u (seen in %u of %u frames)", s.stem, s.target.c_str(), best->second.width, best->second.height,
          best->second.distinct_frames, s.learning_frames);
      return;
    }
    if (final_attempt) { s.learning_failed = true; say("video backgrounds: the %s background could not be learned (%zu textures seen)", s.stem, s.candidates.size()); }
  }
  std::string root_;
  Slot slots_[2];
  std::mutex mutex_;
  int active_ = -1;
};
// Border art as a looping video (upstream's Border class: an .mp4 in the Borders folder plays in the bars beside the picture).
// Decoded with the menu videos' decoder, no matte; it runs only while the bars are drawn (overlay_draw.cpp asks for a frame every
// frame they show) and pauses a second after they stop asking.
class Border {
 public:
  ~Border() { decoder_.reset(); }
  std::shared_ptr<const Frame> frame(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (path != path_ || !decoder_) {
      decoder_.reset();   // stops the old one's thread
      path_ = path;
      decoder_ = std::make_unique<Decoder>();
      decoder_->start(path, false);
      say("border art: playing %s", path.c_str());
    }
    asked_ = Clock::now();
    decoder_->set_active(true);
    return decoder_->frame();
  }
  std::string error() { std::lock_guard<std::mutex> lock(mutex_); return decoder_ ? decoder_->error() : std::string(); }
  void tick() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (decoder_ && Clock::now() - asked_ > std::chrono::seconds(1)) decoder_->set_active(false);
  }
  void stop() { std::lock_guard<std::mutex> lock(mutex_); decoder_.reset(); path_.clear(); }
 private:
  std::mutex mutex_;
  std::string path_;
  std::unique_ptr<Decoder> decoder_;
  Clock::time_point asked_{};
};
Border* g_border = nullptr;
std::once_flag g_border_once;
Border& border() { std::call_once(g_border_once, [] { g_border = new Border(); }); return *g_border; }
Manager* g_manager = nullptr;
std::once_flag g_once;
Manager& manager() { std::call_once(g_once, [] { g_manager = new Manager(); }); return *g_manager; }
}  // namespace

void begin_frame(uint8_t major, uint8_t minor) {
  if (g_border) g_border->tick();   // a border video nobody draws pauses
  if (!g_manager && !fe::gfx::live().video_backgrounds.load(std::memory_order_relaxed)) return;   // never on: nothing is made
  manager().begin(major, minor);
}
bool wants_texture_names() { return g_manager && manager().wants_names(); }
std::shared_ptr<const Frame> lookup(const std::string& name, uint32_t width, uint32_t height) { return g_manager ? manager().lookup(name, width, height) : nullptr; }
std::shared_ptr<const Frame> fullscreen_frame() { return g_manager ? manager().fullscreen() : nullptr; }
bool fullscreen_layer_before(unsigned leading_untextured, bool next_textured) {
  return learning::should_insert_fullscreen_layer(0, leading_untextured, next_textured);
}
std::string status(int slot) { return g_manager ? manager().status(slot) : std::string(fe::gfx::live().video_backgrounds.load() ? "Starts with the game" : "Off"); }
void relearn(int slot) { manager().relearn(slot); }
void shutdown() { if (g_manager) g_manager->stop(); if (g_border) g_border->stop(); }
std::shared_ptr<const Frame> border_frame(const std::string& path) { return border().frame(path); }
std::string border_error() { return g_border ? g_border->error() : std::string(); }
bool load_picture(const std::string& path, uint32_t max_w, uint32_t max_h, uint32_t* width, uint32_t* height, std::vector<uint8_t>& rgba) {
  std::vector<uint8_t> bgra;
  if (!linux_media::load_picture(path, max_w, max_h, width, height, bgra) || bgra.size() < size_t(*width) * *height * 4) return false;
  rgba.resize(size_t(*width) * *height * 4);
  for (size_t i = 0; i + 3 < rgba.size(); i += 4) { rgba[i] = bgra[i + 2]; rgba[i + 1] = bgra[i + 1]; rgba[i + 2] = bgra[i]; rgba[i + 3] = bgra[i + 3]; }
  return true;
}
}  // namespace fe::video_bg
