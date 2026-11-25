// Looks for the renderer (frontend/patches/0116): how fighters, the stage and the menus are drawn, the Lightweight
// ladder, Visual effects, the look's background and the Arcade look's plain background. Display only: the renderer
// changes what it hands Aurora, never game memory, so the simulation, online play and replays are the same at every look.
//
// The rules are melee-unlocked's (fetched tree: port/runtime/gx/gx_retro.h, the lightweight and effects parts of
// gx_core.h, the tint in gx_shader.cpp fill_ps_constants, the presets in render_options.h), adapted to this renderer:
// melee-unlocked applies them inside its own shader generator and back ends, here the Aurora bridge applies them to
// each draw's state, textures and vertex stream before Aurora sees it. Adapted from melee-unlocked v1.0.5
// (GPL-2.0-or-later); this file is GPL-3.0-or-later.
//
// Written by the looks module (features/looks.cpp) and settings.cpp on the simulation or menu thread, read by the
// render thread once per frame. Relaxed atomics: a frame drawn half with the old look is harmless.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace fe::looks {

// How a group of draws is textured (gx_retro.h Surface). Wire is drawn as lines.
enum class Surface : uint8_t { Textured = 0, Retro64 = 1, Flat = 2, White = 3, Posterize = 4, Wire = 5 };

struct Live {
  std::atomic<uint8_t> fighter{0}, stage{0}, menus{0};   // Surface per group; menus = everything outside a match's world camera
  std::atomic<bool> unfiltered{false};                   // nearest-texel sampling on treated groups (the 64 look)
  std::atomic<uint8_t> colours{0};                       // 0 original, 1 port colours on the fighters' models
  std::atomic<uint8_t> background{0};                    // 0 the stage's own, 1 black, 2 plain colour
  std::atomic<uint32_t> background_rgb{0x000000};
  std::atomic<uint8_t> lightweight{0};                   // 0..5 (gx_core.h's ladder; 1 is the game's low poly fighters)
  std::atomic<uint8_t> effects{0};                       // Visual effects 0..3: Full, Reduced, Minimal, No decorative particles
  std::atomic<uint8_t> arcade_backdrop{0};               // settings.ini arcade_backdrop: 1 plain colour, 2 the grid (also plain behind)
  std::atomic<uint32_t> arcade_rgb{0x1E2430};            // arcade_background_rgb
  std::atomic<bool> owners{false};                       // the game marks draw owners (MuHostApi.native_owner_tracking), set by the module
  std::atomic<bool> true_widescreen{false};              // settings.ini truewidescreen: the renderer widens to 16:9 (bridge, patches/0116)
  std::atomic<uint8_t> scene_major{0};                   // the scene of the newest frame the renderer converted (the opening movie stays 4:3)
  // Measurements for the picture test's timing lines (render thread only writes them).
  std::atomic<uint64_t> frames{0}, rule_ns{0}, skipped{0}, drawn{0}, treated_textures{0}, tinted{0}, wired{0};
};
inline Live& live() { static Live l; return l; }

inline uint8_t load(const std::atomic<uint8_t>& a) { return a.load(std::memory_order_relaxed); }
// Something here changes the picture of a match: the renderer needs to know which draws are fighters and stage
// pieces, so the game is asked to mark draw owners (tokens 0xF1, 0xF2, 0xF7 in its GX stream) only then.
inline bool active() {
  const Live& l = live();
  return load(l.fighter) || load(l.stage) || load(l.menus) || load(l.colours) || load(l.background) || load(l.lightweight) >= 2 ||
         load(l.effects) || load(l.arcade_backdrop);
}

// The presets as melee-unlocked ships them (render_options.h look_presets), minus the three that need the Lab
// view's stick figures and capsules (not here). Fields: Lightweight level, fighter / stage / menus surface,
// unfiltered, port colours, background, Visual effects; `exact` names the mod artwork a Diet or Crystal preset uses
// when the player's own build of it is served (melee-unlocked's "Get mod looks"); without it the built-in stand-in
// treatments below draw the look, as upstream does before setup.
struct Preset {
  const char* name; const char* tip;
  int lightweight, fighter, stage, menus; bool unfiltered; int colours, background, effects;
  bool unlocked;   // upstream lifts the frame cap (the console's display is 60 Hz: nothing to lift here)
};
inline const Preset* presets(int* count) {
  static const Preset p[] = {
    {"Off", "The game as it is.", 0, 0, 0, 0, false, 0, 0, 0, false},
    {"Light", "Low poly fighters: the models the game itself uses in reflections and shadows.", 1, 0, 0, 0, false, 0, 0, 0, false},
    {"Lighter", "Low poly fighters, and the stage scenery behind the fighting area is not drawn.", 2, 0, 0, 0, false, 0, 0, 0, false},
    {"Diet Classic (mod)", "Diet Classic: low poly fighters, no scenery, black background, fewer glows.", 2, 0, 0, 0, false, 0, 1, 1, false},
    {"Diet 64 (mod)", "Diet 64: low poly fighters and sharp 64 x 64 textures everywhere, fewer glows.", 1, 1, 1, 1, true, 0, 0, 1, false},
    {"Crystal (original colors)", "Crystal: every texture as its own flat colour, posterized menus, black background.", 2, 2, 2, 4, false, 0, 1, 2, false},
    {"Crystal (player colors)", "Crystal with the fighters recoloured by port: P1 red, P2 blue, P3 yellow, P4 green.", 2, 2, 2, 4, false, 1, 1, 2, false},
    {"Wireframe", "Everything in the fighting area as lines on black.", 5, 5, 5, 0, false, 0, 1, 2, true},
    {"Potato", "White fighters in port colours on a white fighting area, black background.", 4, 3, 3, 0, false, 1, 1, 2, true},
    {"64 Style (built-in)", "N64-inspired: low poly fighters and sharp 64 x 64 textures, no patch needed.", 1, 1, 1, 1, true, 0, 0, 1, false},
    {"Silhouettes", "Every fighter as its real model in its port colour on white platforms over a plain background.", 2, 3, 3, 0, false, 1, 2, 2, false},
    {"Classic Style (built-in)", "A built-in Diet Classic-inspired look, no patch needed.", 2, 0, 0, 0, false, 0, 1, 1, false},
  };
  *count = int(sizeof p / sizeof p[0]);
  return p;
}
inline void apply_preset(int index) {
  int n = 0; const Preset* p = presets(&n);
  if (index < 0 || index >= n) return;
  Live& l = live();
  l.lightweight.store(uint8_t(p[index].lightweight)); l.fighter.store(uint8_t(p[index].fighter)); l.stage.store(uint8_t(p[index].stage));
  l.menus.store(uint8_t(p[index].menus)); l.unfiltered.store(p[index].unfiltered); l.colours.store(uint8_t(p[index].colours));
  l.background.store(uint8_t(p[index].background)); l.effects.store(uint8_t(p[index].effects));
}
// The preset the current parts match, or -1 (Custom).
inline int preset_of() {
  int n = 0; const Preset* p = presets(&n);
  const Live& l = live();
  for (int i = 0; i < n; ++i)
    if (load(l.lightweight) == p[i].lightweight && load(l.fighter) == p[i].fighter && load(l.stage) == p[i].stage && load(l.menus) == p[i].menus &&
        l.unfiltered.load() == p[i].unfiltered && load(l.colours) == p[i].colours && load(l.background) == p[i].background && load(l.effects) == p[i].effects)
      return i;
  return -1;
}

// ---- texture treatments (gx_retro.h), on RGBA8 pixels ----
constexpr uint32_t kRetroMaxSide = 64;
inline void retro_halve(std::vector<uint8_t>& px, uint32_t& w, uint32_t& h) {
  const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
  std::vector<uint8_t> out(size_t(nw) * nh * 4);
  for (uint32_t y = 0; y < nh; ++y)
    for (uint32_t x = 0; x < nw; ++x)
      for (int c = 0; c < 4; ++c) {
        uint32_t sum = 0, n = 0;
        for (uint32_t dy = 0; dy < 2; ++dy)
          for (uint32_t dx = 0; dx < 2; ++dx) {
            const uint32_t sx = std::min(w - 1, x * 2 + dx), sy = std::min(h - 1, y * 2 + dy);
            sum += px[(size_t(sy) * w + sx) * 4 + c]; ++n;
          }
        out[(size_t(y) * nw + x) * 4 + c] = uint8_t((sum + n / 2) / n);
      }
  px.swap(out); w = nw; h = nh;
}
// One colour for a whole texture: the mean of its visible texels (weighted by alpha), alpha the mean alpha.
inline void flat_colour(const std::vector<uint8_t>& px, uint8_t out[4]) {
  double r = 0, g = 0, b = 0, wsum = 0, asum = 0; const size_t n = px.size() / 4;
  for (size_t i = 0; i < n; ++i) {
    const double a = px[i * 4 + 3] / 255.0; asum += a;
    r += px[i * 4] * a; g += px[i * 4 + 1] * a; b += px[i * 4 + 2] * a; wsum += a;
  }
  if (wsum <= 0) {   // no alpha at all: the plain mean
    for (size_t i = 0; i < n; ++i) { r += px[i * 4]; g += px[i * 4 + 1]; b += px[i * 4 + 2]; }
    const double k = 1.0 / double(std::max<size_t>(1, n));
    out[0] = uint8_t(r * k + 0.5); out[1] = uint8_t(g * k + 0.5); out[2] = uint8_t(b * k + 0.5); out[3] = 0;
    return;
  }
  out[0] = uint8_t(r / wsum + 0.5); out[1] = uint8_t(g / wsum + 0.5); out[2] = uint8_t(b / wsum + 0.5);
  out[3] = uint8_t(asum / double(std::max<size_t>(1, n)) * 255.0 + 0.5);
}
inline void posterize(std::vector<uint8_t>& px) {
  for (size_t i = 0; i < px.size(); ++i) if (i % 4 != 3) px[i] = uint8_t(std::min(3, px[i] / 64) * 85);
}
// Treats level 0 of a decoded RGBA8 texture for a surface in place. False: upload the game's own.
inline bool treat(Surface s, std::vector<uint8_t>& px, uint32_t& w, uint32_t& h) {
  if (s == Surface::Textured || s == Surface::Wire) return false;
  if (s == Surface::Retro64 && w <= kRetroMaxSide && h <= kRetroMaxSide) return false;
  if (px.size() < size_t(w) * h * 4) {
    if (s != Surface::White) return false;
    px = {255, 255, 255, 255}; w = h = 1;
    return true;
  }
  if (s == Surface::White || s == Surface::Flat) {
    // One colour for the whole texture, its own alpha kept (a single opaque texel turned smoke and cut-out leaves
    // into solid squares); a texture with no transparency becomes one texel.
    uint8_t flat[4] = {255, 255, 255, 255};
    if (s == Surface::Flat) flat_colour(px, flat);
    bool opaque = true;
    for (size_t i = 3; i < px.size() && opaque; i += 4) opaque = px[i] == 255;
    if (opaque) { px = {flat[0], flat[1], flat[2], 255}; w = h = 1; }
    else for (size_t i = 0; i + 3 < px.size(); i += 4) { px[i] = flat[0]; px[i + 1] = flat[1]; px[i + 2] = flat[2]; }
  } else if (s == Surface::Retro64) {
    while (w > kRetroMaxSide || h > kRetroMaxSide) retro_halve(px, w, h);
  } else if (s == Surface::Posterize) {
    posterize(px);
  }
  return true;
}

// The port colours of the fighters' models (gx_shader.cpp): P1 red, P2 blue, P3 yellow, P4 green.
inline const float* port_colour(int slot) {
  static const float port[4][3] = {{1.0f, 0.22f, 0.22f}, {0.28f, 0.45f, 1.0f}, {1.0f, 0.85f, 0.15f}, {0.25f, 0.9f, 0.3f}};
  return port[slot & 3];
}
// The background clear in a match (gx_retro.h look_clear_colour): the Arcade look's colour wins, then the look's
// black or plain colour; alpha kept. ARGB.
inline uint32_t clear_colour(bool in_match, uint32_t argb) {
  const Live& l = live();
  if (!in_match) return argb;
  if (load(l.arcade_backdrop)) return (argb & 0xFF000000u) | (l.arcade_rgb.load(std::memory_order_relaxed) & 0xFFFFFFu);
  const uint8_t b = load(l.background);
  if (!b) return argb;
  return (argb & 0xFF000000u) | (b == 1 ? 0u : (l.background_rgb.load(std::memory_order_relaxed) & 0xFFFFFFu));
}
}  // namespace fe::looks
