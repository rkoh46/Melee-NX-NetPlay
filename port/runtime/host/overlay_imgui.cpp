// Overlay drawing on the Switch: the in-game menu and HUD that game_menu.cpp
// lays out as OverlayFrame shapes and text are rasterised through Aurora's
// ImGui pass, which composites over the presented frame. There is no touch
// layer on the console; its hooks are inert here.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "overlay_imgui.h"

#include "game_menu.h"
#include "input_config.h"
#include "overlay.h"
#include "window.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace host {
namespace {
ImU32 colour(float r, float g, float b, float a) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, std::clamp(a, 0.0f, 1.0f)));
}
// The text size that fits `text` in `max_w`, never larger than `size`.
float fitted_size(ImFont* font, float size, const char* text, float max_w) {
  if (max_w <= 0.0f) return size;
  const float width = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
  return width > max_w && width > 0.0f ? size * (max_w / width) : size;
}
}  // namespace

bool game_overlay(OverlayFrame& out) {
  out = {};
  int w = 1280, h = 720;
  window_client_size(&w, &h);
  menu_overlay(out, w, h, false);
  return !out.shapes.empty() || !out.texts.empty();
}

void overlay_imgui_draw(const OverlayFrame& frame) {
  ImGuiContext* context = ImGui::GetCurrentContext();
  if (!context) return;
  ImDrawList* draw = ImGui::GetForegroundDrawList();
  ImFont* font = ImGui::GetFont();
  if (!draw || !font) return;
  for (const OverlayShape& s : frame.shapes) {
    const float a = s.a * frame.alpha;
    if (a <= 0.0f) continue;
    const ImVec2 p0(s.x0, s.y0), p1(s.x1, s.y1);
    const float lift = 0.35f * s.pressed;   // pressed controls brighten, as on the touch layer
    const ImU32 fill = colour(std::min(1.0f, s.r + lift), std::min(1.0f, s.g + lift), std::min(1.0f, s.b + lift), a);
    if (s.ring > 0.0f) draw->AddRect(p0, p1, fill, s.corner, 0, s.ring);
    else draw->AddRectFilled(p0, p1, fill, s.corner);
    if (s.label > 0 && s.label < kOverlayLabelCount && s.label_w > 0.0f && s.label_h > 0.0f) {
      const char* text = kOverlayLabels[s.label];
      const float size = fitted_size(font, s.label_h, text, s.label_w);
      const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
      const ImVec2 at((s.x0 + s.x1 - extent.x) * 0.5f, (s.y0 + s.y1 - extent.y) * 0.5f);
      draw->AddText(font, size, at, colour(1, 1, 1, a), text);
    }
  }
  for (const OverlayText& t : frame.texts) {
    if (t.a <= 0.0f || t.text.empty()) continue;
    const float size = fitted_size(font, t.size, t.text.c_str(), t.max_w);
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, t.text.c_str());
    float x = t.x;
    if (t.align == 1) x -= extent.x * 0.5f;
    else if (t.align == 2) x -= extent.x;
    // A soft shadow keeps the text legible over bright stages, like the Metal atlas path.
    draw->AddText(font, size, ImVec2(x + 1.0f, t.y + 1.0f), colour(0, 0, 0, 0.6f * t.a), t.text.c_str());
    draw->AddText(font, size, ImVec2(x, t.y), colour(t.r, t.g, t.b, t.a), t.text.c_str());
  }
}

// No touch screen in use: the console's own touch panel is not a game controller here.
bool touch_overlay(OverlayFrame& out) { out.shapes.clear(); out.alpha = 0.0f; return false; }
void touch_set_opacity(float) {}
void touch_force_visible(bool) {}
void touch_set_game_aspect(float) {}
void touch_set_scale(float) {}
void haptic_tap(bool) {}
int window_take_key_press() { return -1; }   // no keyboard on the console
// Window geometry the menu asks about: the console's display has no notch, fold or point scale.
void window_safe_insets(float& top, float& left, float& right, float& bottom) { top = left = right = bottom = 0.0f; }
bool window_division_region(float*) { return false; }
float window_pixels_per_point() { return 1.0f; }
// The Controls page's live controller list and remap capture need SDL gamepad
// access that Aurora owns on the console; until that is bridged the page lists
// nothing, and the GameCube layout Aurora PAD applies is what the game sees.
std::vector<ControllerInfo> window_list_controllers() { return {}; }
int window_capture_input(const std::string&) { return kUnbound; }
void window_test_rumble(const std::string&) {}
}  // namespace host
