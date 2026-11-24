// The on-screen overlays and picture options, drawn on the render thread inside Aurora's frame (the ImGui pass that
// composites over the presented picture, as the app's menu and HUD are drawn). What they show comes from the overlay
// module's snapshot (src/features/overlays.cpp, overlay_state.h); nothing here touches the game.
//   - controller input display (inputoverlay*), 20XX TE's input display bit, Training's panel (actions, inputs and the
//     frame advantage the game measures, command 0xF4);
//   - frame counter and performance graph (showfps, performance), the GameCube adapter's report rate (adapterrate);
//   - players' names over the fighters online (playernicknames), the Arcade look's combo counter (arcade_combo_counter),
//     the L-cancel flash (lcancel_flash_mode) as a ring over the fighter, the display latency test (latencytest);
//   - picture options: shape (aspect), internal resolution (scale), brightness, contrast, vibrance, sharpening and the CRT
//     look (patches/0109's aurora_set_picture, in Aurora's present pass), border art in the side bars (borderart).
// The look follows melee-unlocked's overlays (port/runtime/gx/training_overlay.cpp, pc_settings.cpp; read, not copied);
// the action names of the Training panel are taken from the fetched training_overlay.cpp at build time (gen/action_names.inc).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"
#include "overlay_state.h"
#include "host.h"                // shim: gcadapter_status

#include <aurora/aurora.h>
#include <aurora/imgui.h>
#include <imgui.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace aurora::window {
void set_frame_buffer_aspect(float aspect);
void set_frame_buffer_scale(float scale);
}

namespace fe {
namespace {
#include "action_names.inc"   // const char* const kActionNames[] (build.sh sync, from the fetched training_overlay.cpp)
const char* action_name(uint32_t action) {
  return action < sizeof kActionNames / sizeof kActionNames[0] ? kActionNames[action] : "";
}

constexpr ImU32 kPort[4] = {IM_COL32(255, 99, 107, 255), IM_COL32(83, 164, 255, 255), IM_COL32(255, 211, 84, 255), IM_COL32(93, 220, 143, 255)};
ImU32 with_alpha(ImU32 c, float a) { return (c & 0x00FFFFFFu) | (ImU32(std::clamp(a, 0.0f, 1.0f) * 255.0f) << 24); }

struct Rect { float x0, y0, x1, y1; float w() const { return x1 - x0; } float h() const { return y1 - y0; } };

// The shape the game's picture is presented in (overlay_state.h Config::aspect); 0: the whole screen.
float picture_aspect(const overlay::Config& c) {
  switch (c.aspect) {
    case 1: return 73.0f / 60.0f;
    case 2: return 4.0f / 3.0f;
    case 3: return 16.0f / 9.0f;
    case 4: return 0.0f;
    default: return options.stretch ? 0.0f : 4.0f / 3.0f;   // Auto: 16:9 code fills the screen, else 4:3
  }
}
Rect game_rect(int ww, int wh, float aspect) {
  if (aspect <= 0.0f) return {0, 0, float(ww), float(wh)};
  float w = float(ww), h = w / aspect;
  if (h > float(wh)) { h = float(wh); w = h * aspect; }
  const float x = std::floor((float(ww) - w) * 0.5f), y = std::floor((float(wh) - h) * 0.5f);
  return {x, y, x + w, y + h};
}

void text(ImDrawList* dl, float size, ImVec2 at, ImU32 col, const char* s, bool shadow = true) {
  ImFont* font = ImGui::GetFont();
  if (shadow) dl->AddText(font, size, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, (col >> 24) * 3 / 4), s);
  dl->AddText(font, size, at, col, s);
}
float text_width(float size, const char* s) { return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, s).x; }

// ---- picture options: applied when they change ----
uint32_t applied_generation = ~0u;
bool applied_once = false;
void apply_picture(const overlay::Config& c) {
  if (applied_once && c.generation == applied_generation) return;
  applied_once = true; applied_generation = c.generation;
  AuroraPicture p{c.brightness, c.contrast, c.vibrance, c.sharpness, uint32_t(c.crt), c.crt_curve};
  aurora_set_picture(&p);
  aurora::window::set_frame_buffer_aspect(picture_aspect(c));
  aurora::window::set_frame_buffer_scale(float(c.scale));
  say("overlay: picture: shape %d (%.3f), internal resolution %s, brightness %.2f, contrast %.2f, vibrance %.2f, sharpening %.2f, CRT %d (curve %.2f), border \"%s\"",
      c.aspect, picture_aspect(c), c.scale ? (std::to_string(640 * c.scale) + "x" + std::to_string(480 * c.scale)).c_str() : "auto",
      c.brightness, c.contrast, c.vibrance, c.sharpness, c.crt, c.crt_curve, c.border.c_str());
}

// ---- border art ----
struct Art { ImTextureID id = 0; int w = 0, h = 0; bool tried = false; };
std::map<std::string, Art> arts;
const Art& art_for(const std::string& name) {
  Art& a = arts[name];
  if (a.tried) return a;
  a.tried = true;
  const std::string folder = (options.app && !options.base_dir.empty() ? options.base_dir : options.out_dir) + "/Borders/";
  const std::string path = name.find('/') != std::string::npos ? name : folder + name;
  SDL_Surface* loaded = SDL_LoadBMP(path.c_str());
  if (!loaded) { say("overlay: border art %s: %s (a .bmp in the Borders folder)", path.c_str(), SDL_GetError()); return a; }
  SDL_Surface* rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
  SDL_DestroySurface(loaded);
  if (!rgba) { say("overlay: border art %s: %s", path.c_str(), SDL_GetError()); return a; }
  std::vector<uint8_t> pixels(size_t(rgba->w) * size_t(rgba->h) * 4);
  for (int y = 0; y < rgba->h; ++y) std::memcpy(&pixels[size_t(y) * size_t(rgba->w) * 4], static_cast<const uint8_t*>(rgba->pixels) + size_t(y) * size_t(rgba->pitch), size_t(rgba->w) * 4);
  a.w = rgba->w; a.h = rgba->h;
  SDL_DestroySurface(rgba);
  a.id = aurora_imgui_add_texture(uint32_t(a.w), uint32_t(a.h), pixels.data());
  say("overlay: border art %s, %dx%d", path.c_str(), a.w, a.h);
  return a;
}
void draw_border(ImDrawList* dl, const overlay::Config& c, const Rect& g, int ww, int wh) {
  if (c.border.empty() || (g.x0 <= 0.5f && g.y0 <= 0.5f)) return;
  const float W = float(ww), H = float(wh);
  std::vector<Rect> parts;
  if (g.x0 > 0.5f) { parts.push_back({0, 0, g.x0, H}); parts.push_back({g.x1, 0, W, H}); }
  if (g.y0 > 0.5f) { parts.push_back({0, 0, W, g.y0}); parts.push_back({0, g.y1, W, H}); }
  if (c.border == "grid") {
    // Melee's own menu look: a dark blue field with a fine grid that brightens toward the picture.
    for (const Rect& r : parts) {
      dl->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), IM_COL32(6, 14, 40, 255));
      const float step = std::max(16.0f, H / 18.0f);
      for (float x = std::fmod(W * 0.5f, step); x < W; x += step)
        if (x >= r.x0 && x <= r.x1) dl->AddLine(ImVec2(x, r.y0), ImVec2(x, r.y1), IM_COL32(40, 80, 170, 110), 1.0f);
      for (float y = std::fmod(H * 0.5f, step); y < H; y += step)
        if (y >= r.y0 && y <= r.y1) dl->AddLine(ImVec2(r.x0, y), ImVec2(r.x1, y), IM_COL32(40, 80, 170, 110), 1.0f);
    }
  } else {
    const Art& a = art_for(c.border);
    if (!a.id) return;
    // The picture covers the whole screen (cut to fill, aspect kept); only the bars show it.
    const float s = std::max(W / float(a.w), H / float(a.h));
    const float iw = float(a.w) * s, ih = float(a.h) * s, ix = (W - iw) * 0.5f, iy = (H - ih) * 0.5f;
    for (const Rect& r : parts)
      dl->AddImage(a.id, ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), ImVec2((r.x0 - ix) / iw, (r.y0 - iy) / ih), ImVec2((r.x1 - ix) / iw, (r.y1 - iy) / ih));
  }
  if (c.border_edge) {
    const float e = std::max(10.0f, H / 40.0f);
    const ImU32 dark = IM_COL32(0, 0, 0, 170), clear = IM_COL32(0, 0, 0, 0);
    if (g.x0 > 0.5f) {
      dl->AddRectFilledMultiColor(ImVec2(g.x0 - e, 0), ImVec2(g.x0, H), clear, dark, dark, clear);
      dl->AddRectFilledMultiColor(ImVec2(g.x1, 0), ImVec2(g.x1 + e, H), dark, clear, clear, dark);
    }
    if (g.y0 > 0.5f) {
      dl->AddRectFilledMultiColor(ImVec2(0, g.y0 - e), ImVec2(W, g.y0), clear, clear, dark, dark);
      dl->AddRectFilledMultiColor(ImVec2(0, g.y1), ImVec2(W, g.y1 + e), dark, dark, clear, clear);
    }
  }
}

// ---- input display ----
// One controller, laid out like a GameCube pad: the control stick's gate with its position, the C-stick, the two
// triggers as bars (lit when the digital click is down), A B X Y Z Start and the D-pad. `u` is the unit (pixels).
void draw_pad(ImDrawList* dl, ImVec2 o, float u, const overlay::Pad& p, bool values, bool border) {
  const float w = 168 * u, h = values ? 104 * u : 90 * u;
  if (border) {
    dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), IM_COL32(10, 12, 18, 160), 6 * u);
    dl->AddRect(o, ImVec2(o.x + w, o.y + h), IM_COL32(210, 215, 225, 90), 6 * u, 0, 1.0f);
  }
  const ImU32 frame = IM_COL32(210, 215, 225, 190), dim = IM_COL32(70, 76, 88, 200);
  const ImU32 main = IM_COL32(236, 240, 246, 255), cyellow = IM_COL32(250, 214, 60, 255);
  auto stick = [&](ImVec2 c, float r, int x, int y, ImU32 col) {
    // The gate is an octagon, as on the controller.
    ImVec2 pts[8];
    for (int i = 0; i < 8; ++i) { const float a = float(i) * 0.785398f + 0.392699f; pts[i] = ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r); }
    dl->AddConvexPolyFilled(pts, 8, IM_COL32(20, 22, 28, 190));
    dl->AddPolyline(pts, 8, frame, ImDrawFlags_Closed, 1.5f);
    const ImVec2 d(c.x + r * float(std::clamp(x, -80, 80)) / 80.0f, c.y - r * float(std::clamp(y, -80, 80)) / 80.0f);
    dl->AddLine(c, d, col, 2.0f * u);
    dl->AddCircleFilled(d, r * 0.24f, col, 16);
  };
  stick(ImVec2(o.x + 26 * u, o.y + 32 * u), 20 * u, p.sx, p.sy, main);
  stick(ImVec2(o.x + 70 * u, o.y + 40 * u), 13 * u, p.cx, p.cy, cyellow);
  auto bar = [&](float x, uint8_t v, bool click, const char* label) {
    const ImVec2 a(o.x + x, o.y + 10 * u);
    dl->AddRectFilled(a, ImVec2(a.x + 9 * u, a.y + 44 * u), dim, 2 * u);
    const float fill = 44 * u * float(v) / 255.0f;
    dl->AddRectFilled(ImVec2(a.x, a.y + 44 * u - fill), ImVec2(a.x + 9 * u, a.y + 44 * u), click ? IM_COL32(120, 200, 255, 255) : IM_COL32(170, 180, 200, 255), 2 * u);
    text(dl, 11 * u, ImVec2(a.x + 1 * u, a.y + 46 * u), frame, label, false);
  };
  bar(92 * u, p.l, (p.button & 0x0040) != 0, "L");
  bar(105 * u, p.r, (p.button & 0x0020) != 0, "R");
  struct Btn { uint16_t mask; ImU32 col; float dx, dy, r; };
  const Btn btns[] = {
    {0x0100, IM_COL32(60, 200, 140, 255), 140, 36, 10},   // A
    {0x0200, IM_COL32(230, 70, 70, 255), 124, 48, 6.5f},  // B
    {0x0400, IM_COL32(220, 220, 225, 255), 156, 28, 6.5f},// X
    {0x0800, IM_COL32(220, 220, 225, 255), 136, 18, 6.5f},// Y
    {0x0010, IM_COL32(150, 100, 230, 255), 156, 12, 5.5f},// Z
    {0x1000, IM_COL32(220, 220, 225, 255), 118, 66, 4.5f},// Start
  };
  for (const Btn& b : btns) {
    const ImVec2 c(o.x + b.dx * u, o.y + b.dy * u);
    if (p.button & b.mask) dl->AddCircleFilled(c, b.r * u, b.col, 20);
    else dl->AddCircle(c, b.r * u, b.col, 20, 1.5f);
  }
  const ImVec2 dp(o.x + 46 * u, o.y + 70 * u);
  auto dpad = [&](uint16_t mask, float x, float y) {
    const ImVec2 a(dp.x + (x - 3.5f) * u, dp.y + (y - 3.5f) * u), b(dp.x + (x + 3.5f) * u, dp.y + (y + 3.5f) * u);
    if (p.button & mask) dl->AddRectFilled(a, b, main); else dl->AddRect(a, b, frame);
  };
  dpad(0x0008, 0, -7); dpad(0x0004, 0, 7); dpad(0x0001, -7, 0); dpad(0x0002, 7, 0);
  if (values) {
    char line[96];
    std::snprintf(line, sizeof line, "%+d,%+d  C %+d,%+d  L %u  R %u", p.sx, p.sy, p.cx, p.cy, p.l, p.r);
    const float room = w - 12 * u, want = text_width(12 * u, line);
    text(dl, want > room ? 12 * u * room / want : 12 * u, ImVec2(o.x + 6 * u, o.y + 88 * u), IM_COL32(200, 206, 218, 255), line, false);
  }
}

// Training's panel per fighter (offline Training with "Inputs and frame advantage"): action, frame, percent, speed and
// the pad; then the frame advantage the game measured last (0xF4), green when positive.
void draw_training(ImDrawList* dl, const overlay::Snapshot& s, const Rect& g, float u) {
  const float pw = 270 * u, ph = 162 * u;
  int shown = 0;
  for (int i = 0; i < 4; ++i) {
    const overlay::Player& f = s.players[i];
    if (!f.fighter) continue;
    const ImVec2 o(g.x0 + 12 * u + float(shown) * (pw + 10 * u), g.y0 + 12 * u);
    if (o.x + pw > g.x1) break;
    ++shown;
    dl->AddRectFilled(o, ImVec2(o.x + pw, o.y + ph), IM_COL32(10, 12, 18, 180), 6 * u);
    dl->AddRectFilled(o, ImVec2(o.x + 4 * u, o.y + ph), kPort[i], 2 * u);
    char line[96];
    const char* name = action_name(f.action);
    if (*name) std::snprintf(line, sizeof line, "P%d  %s", i + 1, name); else std::snprintf(line, sizeof line, "P%d  action %u", i + 1, f.action);
    text(dl, 17 * u, ImVec2(o.x + 10 * u, o.y + 5 * u), IM_COL32(236, 240, 246, 255), line);
    std::snprintf(line, sizeof line, "frame %.0f   %.1f%%", f.anim_frame, f.percent);
    text(dl, 15 * u, ImVec2(o.x + 10 * u, o.y + 25 * u), IM_COL32(200, 206, 218, 255), line);
    std::snprintf(line, sizeof line, "speed %.2f, %.2f", f.vel_x, f.vel_y);
    text(dl, 15 * u, ImVec2(o.x + 10 * u, o.y + 43 * u), IM_COL32(160, 168, 184, 255), line);
    draw_pad(dl, ImVec2(o.x + 50 * u, o.y + 66 * u), u, s.pads[i], false, false);
  }
  if (shown && s.advantage) {
    const ImVec2 o(g.x0 + 12 * u, g.y0 + 12 * u + ph + 8 * u);
    char line[64];
    std::snprintf(line, sizeof line, "Frame advantage  %+d  on %s", s.advantage_frames, s.advantage_kind == 2 ? "shield" : "hit");
    dl->AddRectFilled(o, ImVec2(o.x + pw, o.y + 28 * u), IM_COL32(10, 12, 18, 180), 6 * u);
    text(dl, 17 * u, ImVec2(o.x + 10 * u, o.y + 5 * u), s.advantage_frames >= 0 ? IM_COL32(120, 220, 140, 255) : IM_COL32(240, 120, 110, 255), line);
  }
}

void draw_inputs(ImDrawList* dl, const overlay::Config& c, const overlay::Snapshot& s, const Rect& g, float base) {
  const float u = base * (0.55f + 0.09f * float(c.input_size));
  std::vector<int> ports;
  if (c.input) { for (int i = 0; i < 4; ++i) if (c.input_ports & (1 << i)) ports.push_back(i); }
  else if (s.te_inputs && s.match) { for (int i = 0; i < 4; ++i) if (s.players[i].present) ports.push_back(i); }   // TE: one per player
  float x = g.x0 + 12 * u;
  const float w = 168 * u;
  for (int port : ports) {
    if (x + w > g.x1) break;
    const ImVec2 o(x, g.y0 + 12 * u);
    draw_pad(dl, o, u, s.pads[port], c.input_values, !c.input_hide_border);
    dl->AddRectFilled(o, ImVec2(o.x + 3 * u, o.y + (c.input_values ? 104 : 90) * u), kPort[port], 2 * u);
    text(dl, 14 * u, ImVec2(o.x + 7 * u, o.y + 62 * u), kPort[port], ("P" + std::to_string(port + 1)).c_str());
    x += w + 8 * u;
  }
}

// ---- frame counter, performance graph, adapter rate ----
using Clock = std::chrono::steady_clock;
Clock::time_point last_present{};
float present_ms[240] = {};
int present_at = 0;
float present_fps = 0; Clock::time_point fps_start = Clock::now(); int fps_frames = 0;
void note_present() {
  const auto now = Clock::now();
  if (last_present.time_since_epoch().count()) present_ms[present_at] = std::chrono::duration<float, std::milli>(now - last_present).count();
  present_at = (present_at + 1) % 240;
  last_present = now;
  ++fps_frames;
  const float span = std::chrono::duration<float>(now - fps_start).count();
  if (span >= 0.5f) { present_fps = float(fps_frames) / span; fps_frames = 0; fps_start = now; }
}
void draw_stats(ImDrawList* dl, const overlay::Config& c, const overlay::Snapshot& s, int ww, float base) {
  if (!c.fps && !c.graph && !c.adapter_rate) return;
  const float size = std::max(13.0f, 15.0f * base);
  const float right = float(ww) - 10.0f;
  float y = 8.0f;
  std::vector<std::pair<std::string, ImU32>> lines;
  char line[160];
  if (c.fps) {
    std::snprintf(line, sizeof line, "%.0f FPS  game %.0f/s", present_fps, s.game_fps);
    lines.push_back({line, IM_COL32(236, 240, 246, 255)});
    float sim[1] = {0};
    const int n = perf_history(sim, 1);
    std::snprintf(line, sizeof line, "simulation %.2f ms", n ? sim[0] : 0.0f);
    lines.push_back({line, n && sim[0] > 16.7f ? IM_COL32(240, 120, 110, 255) : IM_COL32(200, 206, 218, 255)});
  }
  if (c.adapter_rate) {
    host::GcAdapterStatus st{};
    if (host::gcadapter_status(st)) {
      if (st.report_hz > 0) std::snprintf(line, sizeof line, "GameCube adapter %.0f reports/s (polled every %d ms)", st.report_hz, st.interval_ms);
      else std::snprintf(line, sizeof line, "GameCube adapter: no reports yet");
    } else {
#ifdef __SWITCH__
      std::snprintf(line, sizeof line, "GameCube adapter: none found");
#else
      std::snprintf(line, sizeof line, "GameCube adapter: not read in this build (console only)");
#endif
    }
    lines.push_back({line, IM_COL32(200, 206, 218, 255)});
    // The system's controllers (Pro Controller, Joy-Con, the Switch Online GameCube controller): controls.cpp.
    for (const std::string& l : controls_rate_lines()) lines.push_back({l, IM_COL32(200, 206, 218, 255)});
  }
  float widest = 0;
  for (auto& l : lines) widest = std::max(widest, text_width(size, l.first.c_str()));
  const float gw = c.graph ? std::max(widest, 240.0f * base) : widest, gh = c.graph ? 70.0f * base : 0.0f;
  const float box_h = float(lines.size()) * (size + 2) + (c.graph ? gh + 8 : 0) + 8;
  dl->AddRectFilled(ImVec2(right - gw - 12, y - 4), ImVec2(right + 4, y + box_h), IM_COL32(10, 12, 18, 170), 5.0f);
  for (auto& l : lines) { text(dl, size, ImVec2(right - text_width(size, l.first.c_str()), y), l.second, l.first.c_str()); y += size + 2; }
  if (c.graph) {
    // The last four seconds: simulation-thread work per game frame (bars; red above the 16.7 ms budget) and the time
    // between presented pictures (line). The dashed line is 16.7 ms.
    y += 4;
    const float x0 = right - gw, x1 = right, y1 = y + gh, scale = gh / 22.0f;   // 0..22 ms: the 16.7 ms budget near the top
    dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y1), IM_COL32(0, 0, 0, 90));
    float sim[240];
    const int n = perf_history(sim, 240);
    const float bw = (x1 - x0) / 240.0f;
    for (int i = 0; i < n; ++i) {
      const float v = std::min(sim[i], 22.0f);
      const float bx = x1 - float(n - i) * bw;
      dl->AddRectFilled(ImVec2(bx, y1 - v * scale), ImVec2(bx + std::max(1.0f, bw - 0.5f), y1), sim[i] > 16.7f ? IM_COL32(240, 110, 100, 230) : IM_COL32(110, 200, 255, 210));
    }
    ImVec2 pts[240]; int k = 0;
    for (int i = 0; i < 240; ++i) {
      const float v = std::min(present_ms[(present_at + i) % 240], 22.0f);
      pts[k++] = ImVec2(x0 + (float(i) + 0.5f) * bw, y1 - v * scale);
    }
    dl->AddPolyline(pts, k, IM_COL32(250, 214, 60, 220), 0, 1.5f);
    for (float x = x0; x < x1; x += 8) dl->AddLine(ImVec2(x, y1 - 16.7f * scale), ImVec2(std::min(x + 4, x1), y1 - 16.7f * scale), IM_COL32(255, 255, 255, 120));
    text(dl, size * 0.8f, ImVec2(x0 + 4, y + 2), IM_COL32(110, 200, 255, 230), "simulation ms", false);
    text(dl, size * 0.8f, ImVec2(x0 + 4, y + 2 + size * 0.85f), IM_COL32(250, 214, 60, 230), "between pictures ms", false);
  }
}

// ---- the network panel (netoverlay 1), adapted from melee-unlocked's port/runtime/gx/net_overlay.cpp (GPL-2.0-or-later):
// the last 10 s of online ticks, newest at the right. Frame time (time since the tick before, yellow when late) with the tick's
// own work inside it (green); a tick that waited for the other player's inputs is a red column; time sync halts (yellow) and
// advances (blue) are marks at the top; rollbacks below, by depth (top: 7 frames); ping as a line (top: 200 ms); marks from the
// D-pad (or a long wait) cross both plots.
void draw_net(ImDrawList* dl, int ww, int wh, float top) {
  static std::vector<overlay::NetSample> recs;   // render thread only
  if (!&features::net_overlay_samples || !features::net_overlay_samples(recs) || recs.empty()) return;
  constexpr size_t kWindow = 600;
  constexpr float kFrameMs = 16.683f, kTimeScaleMs = 50.0f, kPingScaleMs = 200.0f;
  constexpr int kDepthScale = 7;
  const size_t n = std::min(recs.size(), kWindow);
  const overlay::NetSample* r0 = recs.data() + (recs.size() - n);
  const float text_h = ImGui::GetTextLineHeight();
  const float panel_w = std::min(640.0f, float(ww) - 24.0f);
  const char* const labels[] = {"frame time", "work", "wait", "rollback (top: 7)", "shed", "advanced", "ping (top: 200 ms)", "mark"};
  int legend_rows = 1; float used = 0;
  for (const char* label : labels) {
    const float size = 21.0f + ImGui::CalcTextSize(label).x;
    if (used > 0 && used + size > panel_w - 16.0f) { ++legend_rows; used = 0; }
    used += size;
  }
  const float panel_h = 3.0f * text_h + 116.0f + float(legend_rows - 1) * (text_h + 3.0f);
  if (panel_w < 240.0f || float(wh) < panel_h + top + 12.0f) return;
  const ImVec2 o(float(ww) - panel_w - 12.0f, top);
  const float plot_x = o.x + 8.0f, plot_w = panel_w - 16.0f;
  const float time_y = o.y + 8.0f + 2.0f * text_h + 4.0f, time_h = 64.0f;
  const float roll_y = time_y + time_h + 4.0f, roll_h = 24.0f;
  const float col_w = plot_w / float(kWindow), bar_w = std::max(1.0f, col_w);
  const ImU32 back = IM_COL32(10, 12, 18, 175), plot_back = IM_COL32(20, 22, 28, 170);
  const ImU32 textc = IM_COL32(236, 240, 246, 255), dim = IM_COL32(160, 168, 184, 255);
  const ImU32 interval_col = IM_COL32(90, 110, 140, 255), work_col = IM_COL32(120, 220, 140, 255);
  const ImU32 slow_col = IM_COL32(250, 214, 60, 255), wait_col = IM_COL32(230, 70, 70, 220);
  const ImU32 shed_col = IM_COL32(250, 214, 60, 255), advance_col = IM_COL32(120, 200, 255, 255);
  const ImU32 roll_col = IM_COL32(255, 150, 60, 255), ping_col = IM_COL32(0, 255, 255, 200), mark_col = IM_COL32(220, 130, 255, 255);
  dl->AddRectFilled(o, ImVec2(o.x + panel_w, o.y + panel_h), back, 6.0f);
  dl->AddRectFilled(ImVec2(plot_x, time_y), ImVec2(plot_x + plot_w, time_y + time_h), plot_back);
  dl->AddRectFilled(ImVec2(plot_x, roll_y), ImVec2(plot_x + plot_w, roll_y + roll_h), plot_back);
  auto column = [&](size_t i) { return plot_x + col_w * float(kWindow - n + i); };
  unsigned rollbacks = 0, deepest = 0, waited = 0, shed = 0, advanced = 0, marks = 0;
  float px0 = 0, py0 = 0;
  for (size_t i = 0; i < n; ++i) {
    const overlay::NetSample& r = r0[i];
    const float x = column(i);
    if (r.flags & 1) {
      ++waited;
      dl->AddRectFilled(ImVec2(x, time_y), ImVec2(x + bar_w, time_y + time_h), wait_col);
    } else {
      const float interval = i > 0 ? float((r.wall - r0[i - 1].wall) * 1000.0) : kFrameMs;
      const float ih = time_h * std::clamp(interval / kTimeScaleMs, 0.0f, 1.0f);
      const float wh2 = time_h * std::clamp(r.sim_ms / kTimeScaleMs, 0.0f, 1.0f);
      dl->AddRectFilled(ImVec2(x, time_y + time_h - ih), ImVec2(x + bar_w, time_y + time_h), interval > kFrameMs * 1.5f ? slow_col : interval_col);
      dl->AddRectFilled(ImVec2(x, time_y + time_h - wh2), ImVec2(x + bar_w, time_y + time_h), work_col);
    }
    if (r.flags & (2 | 4)) {
      const bool is_shed = (r.flags & 2) != 0;
      ++(is_shed ? shed : advanced);
      const ImU32 col = is_shed ? shed_col : advance_col;
      dl->AddRectFilled(ImVec2(x - 1.0f, time_y), ImVec2(x + 2.0f, time_y + 10.0f), col);
      dl->AddTriangleFilled(ImVec2(x - 4.0f, time_y + 10.0f), ImVec2(x + 5.0f, time_y + 10.0f), ImVec2(x + 0.5f, time_y + 16.0f), col);
    }
    if (r.rollbacks) {
      rollbacks += r.rollbacks;
      deepest = std::max<unsigned>(deepest, r.depth);
      const float depth = r.depth ? float(std::min<int>(r.depth, kDepthScale)) : 0.5f;
      const float rh = roll_h * depth / float(kDepthScale);
      dl->AddRectFilled(ImVec2(x, roll_y + roll_h - rh), ImVec2(x + std::max(2.0f, bar_w), roll_y + roll_h), roll_col);
    }
    const float px = x + bar_w * 0.5f;
    const float py = time_y + time_h - time_h * std::clamp(float(r.ping_ms) / kPingScaleMs, 0.0f, 1.0f);
    if (i > 0) dl->AddLine(ImVec2(px0, py0), ImVec2(px, py), ping_col, 1.0f);
    px0 = px; py0 = py;
    if (r.flags & (8 | 16 | 32 | 64)) {
      ++marks;
      dl->AddLine(ImVec2(x, time_y), ImVec2(x, roll_y + roll_h), mark_col, 1.0f);
      dl->AddTriangleFilled(ImVec2(x - 3, roll_y + roll_h - 5), ImVec2(x + 3, roll_y + roll_h - 5), ImVec2(x, roll_y + roll_h), mark_col);
    }
  }
  const float guide_y = time_y + time_h - time_h * (kFrameMs / kTimeScaleMs);
  dl->AddLine(ImVec2(plot_x, guide_y), ImVec2(plot_x + plot_w, guide_y), IM_COL32(210, 215, 225, 90), 1.0f);
  char line[160];
  std::snprintf(line, sizeof line, "Network and timing   ping %u ms   rollbacks %u (deepest %u)", unsigned(r0[n - 1].ping_ms), rollbacks, deepest);
  dl->PushClipRect(o, ImVec2(o.x + panel_w, o.y + panel_h), true);
  dl->AddText(ImVec2(plot_x, o.y + 6.0f), textc, line);
  std::snprintf(line, sizeof line, "waited %u ticks   shed %u   advanced %u   marks %u", waited, shed, advanced, marks);
  dl->AddText(ImVec2(plot_x, o.y + 6.0f + text_h), dim, line);
  const ImU32 colors[] = {interval_col, work_col, wait_col, roll_col, shed_col, advance_col, ping_col, mark_col};
  float kx = plot_x, ky = roll_y + roll_h + 4.0f;
  for (size_t i = 0; i < sizeof labels / sizeof labels[0]; ++i) {
    const float size = 21.0f + ImGui::CalcTextSize(labels[i]).x;
    if (kx > plot_x && kx + size > plot_x + plot_w) { kx = plot_x; ky += text_h + 3.0f; }
    dl->AddRectFilled(ImVec2(kx, ky + 3.0f), ImVec2(kx + 8.0f, ky + text_h - 3.0f), colors[i]);
    dl->AddText(ImVec2(kx + 11.0f, ky), dim, labels[i]);
    kx += size;
  }
  dl->PopClipRect();
}

// ---- over the match ----
float to_x(const Rect& g, float x640) { return g.x0 + x640 / 640.0f * g.w(); }
float to_y(const Rect& g, float y480) { return g.y0 + y480 / 480.0f * g.h(); }

void draw_names(ImDrawList* dl, const overlay::Snapshot& s, const Rect& g) {
  const float size = std::clamp(g.h() / 27.0f, 15.0f, 30.0f);
  for (int i = 0; i < 4; ++i) {
    const overlay::Player& p = s.players[i];
    if (!p.present || p.name.empty() || !std::isfinite(p.tag_x) || !std::isfinite(p.tag_y)) continue;
    const std::string label = "P" + std::to_string(i + 1) + "  " + p.name;
    const float w = text_width(size, label.c_str());
    const float x = std::clamp(to_x(g, p.tag_x) - w * 0.5f, g.x0 + 4, std::max(g.x0 + 4, g.x1 - w - 4));
    const float y = std::clamp(to_y(g, p.tag_y) - size * 2.4f, g.y0 + 4, g.y1 - size - 8);
    dl->AddRectFilled(ImVec2(x - 5, y - 3), ImVec2(x + w + 5, y + size + 4), IM_COL32(9, 11, 22, 215), 5.0f);
    dl->AddRect(ImVec2(x - 5, y - 3), ImVec2(x + w + 5, y + size + 4), kPort[i], 5.0f, 0, 1.5f);
    text(dl, size, ImVec2(x, y), IM_COL32(255, 255, 255, 255), label.c_str(), false);
  }
}

void draw_combos(ImDrawList* dl, const overlay::Snapshot& s, const Rect& g) {
  if (s.online) return;
  for (int victim = 0; victim < 4; ++victim) {
    const overlay::Combo& c = s.combos[victim];
    if (c.hits < 2 || c.attacker < 0 || c.attacker > 3 || !s.players[c.attacker].present) continue;
    // Holds while the combo lives, then fades over a third of a second; each hit pops the number for a tenth.
    const float fade = c.live ? 1.0f : 1.0f - std::clamp(float(s.retrace - c.ended) / 21.0f, 0.0f, 1.0f);
    if (fade <= 0.0f) continue;
    const float pop = 1.0f + 0.35f * (1.0f - std::clamp(float(s.retrace - c.changed) / 7.0f, 0.0f, 1.0f));
    const float px = std::clamp(g.h() / 18.0f, 18.0f, 60.0f) * pop;
    char label[24];
    std::snprintf(label, sizeof label, "%d HITS", c.hits);
    const float w = text_width(px, label);
    // Over the attacker's damage display: P1 a fifth of the way across, each next port 151 of 640 further.
    const float x = std::clamp(to_x(g, 128.0f + 151.0f * float(c.attacker)) - w * 0.5f, g.x0 + 4, std::max(g.x0 + 4, g.x1 - w - 4));
    const float y = g.y0 + g.h() * 0.775f - px;
    ImFont* font = ImGui::GetFont();
    for (int dx = -2; dx <= 2; dx += 2) for (int dy = -2; dy <= 2; dy += 2)
      if (dx || dy) dl->AddText(font, px, ImVec2(x + float(dx), y + float(dy)), with_alpha(IM_COL32(0, 0, 0, 255), 0.8f * fade), label);
    dl->AddText(font, px, ImVec2(x, y), with_alpha(kPort[c.attacker], fade), label);
  }
}

// The L-cancel flash: the renderer cannot tint one fighter's model here (melee-unlocked does it per draw through its
// owner tokens, which this decoder does not read), so a ring flashes around the fighter for ten frames instead.
void draw_lcancel(ImDrawList* dl, const overlay::Config& c, const overlay::Snapshot& s, const Rect& g) {
  for (int slot = 0; slot < 4; ++slot) {
    const overlay::Flash& f = s.flashes[slot];
    if (!f.on || (f.success && c.lcancel_success == 0)) continue;
    const overlay::Player& p = s.players[slot];
    if (!std::isfinite(p.tag_x) || !std::isfinite(p.tag_y)) continue;
    const uint32_t age = s.retrace - f.start;
    if (age >= 10) continue;
    const float fade = age < 6 ? 1.0f : 1.0f - float(age - 6) / 4.0f;
    const ImU32 col = !f.success ? IM_COL32(255, 40, 30, 255) : c.lcancel_success == 1 ? IM_COL32(255, 255, 255, 255) : IM_COL32(40, 255, 60, 255);
    const ImVec2 centre(to_x(g, p.tag_x), to_y(g, p.tag_y + 56.0f));   // the tag sits over the head: the ring goes on the body
    const float r = g.h() * 0.075f * (1.0f + 0.04f * float(age));
    dl->AddCircleFilled(centre, r, with_alpha(col, 0.22f * fade), 40);
    dl->AddCircle(centre, r, with_alpha(col, 0.95f * fade), 40, std::max(3.0f, g.h() / 160.0f));
    const char* label = f.success ? "L-CANCEL" : "MISSED L-CANCEL";
    const float size = std::clamp(g.h() / 34.0f, 12.0f, 26.0f);
    text(dl, size, ImVec2(centre.x - text_width(size, label) * 0.5f, centre.y - r - size - 2), with_alpha(col, fade), label);
  }
}
}  // namespace

// Render thread, inside the frame, before the app's menu and HUD (they are drawn over this).
void overlay_draw(int ww, int wh) {
  note_present();
  if (!&features::overlay_config || !&features::overlay_snapshot) return;
  if (!ImGui::GetCurrentContext()) return;
  const overlay::Config c = features::overlay_config();
  apply_picture(c);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const Rect g = game_rect(ww, wh, picture_aspect(c));
  draw_border(dl, c, g, ww, wh);
  overlay::Snapshot s;
  features::overlay_snapshot(s);
  if (!s.valid) return;
  const float base = std::max(0.6f, g.h() / 720.0f);
  if (s.latency_fill >= 0) dl->AddRectFilled(ImVec2(g.x0, g.y0), ImVec2(g.x1, g.y1), s.latency_fill ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255));
  if (s.match) {
    dl->PushClipRect(ImVec2(g.x0, g.y0), ImVec2(g.x1, g.y1), true);
    if (c.lcancel) draw_lcancel(dl, c, s, g);
    if (c.nicknames) draw_names(dl, s, g);
    if (c.combo) draw_combos(dl, s, g);
    dl->PopClipRect();
  }
  if (s.lab_panel) draw_training(dl, s, g, base);
  else draw_inputs(dl, c, s, g, base);
  draw_stats(dl, c, s, ww, base);
  // The network panel: top right, below the statistics box when that is on.
  draw_net(dl, ww, wh, (c.fps || c.graph || c.adapter_rate) ? 8.0f + 150.0f * base : 12.0f);
}
}  // namespace fe
