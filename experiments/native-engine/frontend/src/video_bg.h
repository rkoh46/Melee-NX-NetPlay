// Looping menu video backgrounds (src/video_bg.cpp, melee-unlocked's gx/video_background.cpp). Weak: video_bg.cpp is built only
// where FFmpeg is (devkitPro's portlib on the console, build.sh ffmpeg for the Linux test build); without it gx_aurora.cpp
// (frontend/patches/0130) finds none of these and draws the game's own backgrounds. Render thread.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fe::video_bg {
struct Frame { uint32_t width = 0, height = 0; uint64_t serial = 0; std::vector<uint8_t> rgba; };   // rows from the top
__attribute__((weak)) void begin_frame(uint8_t scene_major, uint8_t scene_minor);
// The stage select: true while its video plays, so the renderer names its textures for the learner.
__attribute__((weak)) bool wants_texture_names();
// The video frame to draw in place of the texture `name`, or null (not the learned background, or learning).
__attribute__((weak)) std::shared_ptr<const Frame> lookup(const std::string& name, uint32_t width, uint32_t height);
// The character select: the frame to draw as a full-screen layer behind its foreground, or null.
__attribute__((weak)) std::shared_ptr<const Frame> fullscreen_frame();
__attribute__((weak)) std::string status(int slot);   // 0 character select, 1 stage select
__attribute__((weak)) void relearn(int slot);
__attribute__((weak)) void shutdown();
// A picture file (PNG, JPG, BMP, WebP) as RGBA rows from the top, scaled down to fit max_w x max_h (upstream's
// linux_media::load_picture on FFmpeg): the border art's other formats (overlay_draw.cpp). False when it cannot be read.
__attribute__((weak)) bool load_picture(const std::string& path, uint32_t max_w, uint32_t max_h, uint32_t* width, uint32_t* height,
                                        std::vector<uint8_t>& rgba);
}  // namespace fe::video_bg
