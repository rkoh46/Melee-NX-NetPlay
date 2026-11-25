// melee-unlocked's launcher as the Switch app's home screen before the game (services program; app_services.cpp runs it).
//
// melee-unlocked's launcher (port/app/launcher.cpp) is Win32 GDI. Its look is rebuilt here in ImGui from its own paint
// code: the 720 x 480 layout units, the left rail with the Melee Unlocked mark, the page nav and Bailey, the content
// column on its dark gradient, its palette and layout constants and its accent theme taken verbatim from the fetched tree
// (scripts/mu_ui_extract.py -> gen/mu_ui/mu_launcher_parts.inc), its artwork (melee_mark.bmp, bailey_cut.bmp, embedded
// by build.sh sync) and its English labels. Pages that apply on the Switch:
//   Play           the disc, GAME BUILD (Source Port or Vanilla), PLAY, the version and the self-update from the player's
//                  build computer, the Slippi account and its sign-in (switch_signin.cpp), the mod profile in use
//   Settings       opens melee-unlocked's settings panel (mu_panel.cpp), as its launcher opens the game's F1 panel
//   Replay Viewer  the replays (src/features/replay_library.cpp's list and per-game stats), Watch and Delete
//   Mods           the mod profiles (settings.ini mod_profile)
// Left out: Build (no ISO verification or shader build on the console), Multiplayer Lobby and everything of YouGame,
// Choose version, the language picker and Send logs. The scale follows the screen: 1.5 at 720p handheld, 2.25 at 1080p
// docked, with the rail and the gradient running to the screen's edges.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"
#include "mu_ui.h"
#include "feature_hooks.h"
#include "feature_calls.h"
#include "mu_mods.h"

#include "host.h"
#include "metal_frontend.h"
#include "self_update.h"
#include "switch_signin.h"

#include <aurora/aurora.h>
#include <dolphin/pad.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace host { void signin_pad_read(PADStatus status[4]); }
namespace fe::features {
__attribute__((weak)) std::vector<std::string> library_list();
__attribute__((weak)) void replay_watch(const std::string& path);
__attribute__((weak)) std::string replay_folder();
}


namespace fe {
namespace {
// ---- melee-unlocked's launcher constants and accent theme (verbatim), on ImGui colours ----
using COLORREF = uint32_t;
constexpr COLORREF RGB(int r, int g, int b) { return COLORREF(r) | COLORREF(g) << 8 | COLORREF(b) << 16; }
constexpr int GetRValue(COLORREF c) { return int(c & 0xFF); }
constexpr int GetGValue(COLORREF c) { return int((c >> 8) & 0xFF); }
constexpr int GetBValue(COLORREF c) { return int((c >> 16) & 0xFF); }
constexpr COLORREF CLR_INVALID = 0xFFFFFFFFu;
#include "mu_launcher_parts.inc"
ImU32 col(COLORREF c, float alpha = 1.0f) { return IM_COL32(GetRValue(c), GetGValue(c), GetBValue(c), int(alpha * 255)); }

// The launcher's accent presets (its theme picker's wheel, as a short list on a controller).
constexpr COLORREF kAccents[6] = {RGB(142, 84, 239), RGB(58, 132, 240), RGB(46, 170, 110), RGB(229, 150, 40), RGB(222, 72, 72), RGB(236, 72, 153)};

enum Page { PAGE_PLAY, PAGE_SETTINGS, PAGE_REPLAYS, PAGE_MODS, PAGE_COUNT };
constexpr const char* kNav[PAGE_COUNT] = {"Play", "Settings", "Replay Viewer", "Mods"};

struct Fonts { ImFont* body; ImFont* bold; };

// Layout: the launcher's 720 x 480 units on the screen.
struct Layout {
  float s = 1, ox = 0, oy = 0, w = 1280, h = 720;
  ImVec2 p(float x, float y) const { return ImVec2(ox + x * s, oy + y * s); }
  float u(float v) const { return v * s; }
};

void vgrad(ImDrawList* d, ImVec2 a, ImVec2 b, COLORREF top, COLORREF bot) {
  d->AddRectFilledMultiColor(a, b, col(top), col(top), col(bot), col(bot));
}
// round_rect: a rounded rectangle with an optional vertical gradient and a one-pixel border.
void round_rect(ImDrawList* d, ImVec2 a, ImVec2 b, float radius, COLORREF top, COLORREF bot, COLORREF border) {
  if (top != NO_FILL) {
    const int v0 = d->VtxBuffer.Size;
    d->AddRectFilled(a, b, col(top), radius);
    if (top != bot) ImGui::ShadeVertsLinearColorGradientKeepAlpha(d, v0, d->VtxBuffer.Size, a, ImVec2(a.x, b.y), col(top), col(bot));
  }
  if (border != NO_FILL) d->AddRect(a, b, col(border), radius, 0, 1.0f);
}
// draw_text in a box: left, centred or right, vertically centred, ellipsis when it does not fit.
void text(ImDrawList* d, ImFont* f, float size, ImVec2 a, ImVec2 b, COLORREF c, const std::string& s, int align = 0, bool wrap = false) {
  if (s.empty()) return;
  if (wrap) { d->AddText(f, size, a, col(c), s.c_str(), nullptr, b.x - a.x); return; }
  std::string t = s;
  float w = f->CalcTextSizeA(size, FLT_MAX, 0, t.c_str()).x;
  while (w > b.x - a.x && t.size() > 3) { t.resize(t.size() - 4); t += "..."; w = f->CalcTextSizeA(size, FLT_MAX, 0, t.c_str()).x; }
  const float x = align == 1 ? (a.x + b.x - w) * 0.5f : align == 2 ? b.x - w : a.x;
  const float y = (a.y + b.y - size) * 0.5f;
  d->AddText(f, size, ImVec2(x, y), col(c), t.c_str());
}
void dot(ImDrawList* d, const Layout& L, float x, float y, COLORREF c) {
  d->AddCircleFilled(L.p(x + 4, y + 4), L.u(4), col(c), 16);
}
void art(ImDrawList* d, const mu_ui::Art& a, ImVec2 p0, ImVec2 p1) {
  if (a.texture) d->AddImage(ImTextureID(reinterpret_cast<uintptr_t>(a.texture)), p0, p1);
}
// A box of max_w x max_h holding the art at its own aspect.
void fit(const mu_ui::Art& a, float max_w, float max_h, float* w, float* h) {
  if (a.w <= 0 || a.h <= 0) { *w = *h = 0; return; }
  const float k = std::min(max_w / a.w, max_h / a.h);
  *w = a.w * k; *h = a.h * k;
}

// ---- controller ----
struct Edges { bool up = false, down = false, left = false, right = false, a = false, b = false, x = false, plus = false; };
struct PadReader {
  uint16_t last = 0xFFFF;   // whatever is held at entry is ignored until let go
  int8_t lx = 0, ly = 0;
  int repeat = 0, dir = 0;
  host::PadState pads[4] = {};
  Edges read() {
    PADStatus status[4];
    std::memset(status, 0, sizeof status);
    host::signin_pad_read(status);
    uint16_t buttons = 0; int8_t x = 0, y = 0;
    for (int i = 0; i < 4; ++i) {
      pads[i] = {};
      pads[i].err = status[i].err == PAD_ERR_NONE ? 0 : -1;
      if (status[i].err != PAD_ERR_NONE) continue;
      pads[i].button = status[i].button; pads[i].stick_x = status[i].stickX; pads[i].stick_y = status[i].stickY;
      buttons |= status[i].button;
      if (std::abs(status[i].stickX) > std::abs(x)) x = status[i].stickX;
      if (std::abs(status[i].stickY) > std::abs(y)) y = status[i].stickY;
    }
    if (last == 0xFFFF) { if (buttons == 0) last = 0; return {}; }
    Edges e;
    const uint16_t pressed = buttons & ~last;
    int d = (buttons & PAD_BUTTON_UP) || y > 50 ? 1 : (buttons & PAD_BUTTON_DOWN) || y < -50 ? 2 : (buttons & PAD_BUTTON_LEFT) || x < -50 ? 3 : (buttons & PAD_BUTTON_RIGHT) || x > 50 ? 4 : 0;
    bool fire = false;
    if (d != dir) { repeat = 0; fire = d != 0; } else if (d && ++repeat >= 20 && (repeat - 20) % 6 == 0) fire = true;
    dir = d;
    if (fire) { e.up = d == 1; e.down = d == 2; e.left = d == 3; e.right = d == 4; }
    e.a = pressed & PAD_BUTTON_A; e.b = pressed & PAD_BUTTON_B; e.x = pressed & PAD_BUTTON_X; e.plus = pressed & PAD_BUTTON_START;
    last = buttons; lx = x; ly = y;
    return e;
  }
};

namespace su = host::self_update;

// ---- the home screen's state ----
struct Home {
  std::string user_dir;
  host::MetalFrontend* frontend = nullptr;
  int page = PAGE_PLAY;
  bool rail = false;          // the focus is on the page nav
  int focus = 0;              // the focused control on the page
  bool done = false;
  AppStart result = AppStart::Play;
  // Slippi account
  bool signed_in = false; std::string account;
  // Update: the build computer's latest.json (checked in the background), the download
  struct Update {
    std::mutex lock;
    bool checked = false, offer = false, ok = false, downloading = false, finished = false, failed = false, cancelled = false;
    su::Manifest manifest; std::string error, status;
    std::atomic<uint64_t> done{0}; uint64_t size = 0;
  };
  std::shared_ptr<Update> update = std::make_shared<Update>();
  std::thread download;
  // A message over the page (an installed update, an error): OK closes it, or quits
  std::string msg_title; std::vector<std::string> msg_lines; bool msg_quits = false;
  // Replay Viewer
  int replay_sel = 0, replay_scroll = 0;
  // Mods
  int mod_sel = 0, mod_scroll = 0, mod_button = 0;
  int frames = 0;
  bool pad_reset = false;
};

std::string read_account(const std::string& user_dir, bool* ok) {
  // user.json's display name and connect code only (never its key): what the Play page names.
  std::ifstream in(user_dir + "/user.json");
  *ok = false;
  if (!in) return {};
  std::stringstream ss; ss << in.rdbuf();
  const std::string t = ss.str();
  auto field = [&](const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    const size_t at = t.find(k);
    if (at == std::string::npos) return std::string();
    const size_t q1 = t.find('"', t.find(':', at + k.size()) + 1);
    const size_t q2 = q1 == std::string::npos ? q1 : t.find('"', q1 + 1);
    return q1 == std::string::npos || q2 == std::string::npos ? std::string() : t.substr(q1 + 1, q2 - q1 - 1);
  };
  const std::string name = field("displayName"), code = field("connectCode");
  *ok = !code.empty();
  return name + (code.empty() ? "" : " (" + code + ")");
}

std::vector<features::MenuEntry> page_entries(const char* page) {
  std::vector<features::MenuEntry> out;
  for (const auto& e : features::call_menu_entries()) if (e.page && std::strcmp(e.page, page) == 0) out.push_back(e);
  return out;
}
const features::MenuEntry* entry(const std::vector<features::MenuEntry>& list, const char* label) {
  for (const auto& e : list) if (e.label && std::strcmp(e.label, label) == 0) return &e;
  return nullptr;
}

// ---- painting ----
void paint_rail(ImDrawList* d, const Layout& L, const Fonts& f, const Home& h) {
  const float rail_r = L.ox + L.u(RAIL_W);
  vgrad(d, ImVec2(0, 0), ImVec2(rail_r, L.h), C_RAIL_TOP, C_RAIL_BOT);
  d->AddRectFilled(ImVec2(rail_r - 1, 0), ImVec2(rail_r, L.h), col(C_DIVIDER));
  // The game's own mark, drawn as artwork, centred at the top of the rail.
  float mw, mh;
  fit(mu_ui::mark(), L.u(RAIL_W - 44), L.u(NAV_Y - 22), &mw, &mh);
  const ImVec2 m0(L.ox + (L.u(RAIL_W) - mw) * 0.5f, L.oy + L.u(14));
  art(d, mu_ui::mark(), m0, ImVec2(m0.x + mw, m0.y + mh));
  for (int i = 0; i < PAGE_COUNT; ++i) {
    const ImVec2 a = L.p(12, NAV_Y + i * NAV_GAP), b = L.p(RAIL_W - 12, NAV_Y + i * NAV_GAP + NAV_H);
    const bool on = h.page == i;
    const bool hot = h.rail && h.focus == i;
    if (on) {
      round_rect(d, a, b, L.u(8), C_NAV_ON, C_NAV_ON, NO_FILL);
      round_rect(d, L.p(12, NAV_Y + i * NAV_GAP + 8), L.p(16, NAV_Y + i * NAV_GAP + 26), L.u(2), C_ACC_HI, C_ACC_LO, NO_FILL);
    } else if (hot) {
      round_rect(d, a, b, L.u(8), C_NAV_HOT, C_NAV_HOT, NO_FILL);
    }
    if (hot) round_rect(d, ImVec2(a.x + L.u(2), a.y + L.u(2)), ImVec2(b.x - L.u(2), b.y - L.u(2)), L.u(6), NO_FILL, NO_FILL, C_ACC_LO);
    text(d, f.bold, L.u(14), ImVec2(a.x + L.u(18), a.y), b, on ? C_TEXT : C_DIM, kNav[i]);
  }
  // Bailey, centred in what is left of the rail under the nav.
  const float top = L.oy + L.u(NAV_Y + 5 * NAV_GAP + NAV_H + 10);
  float dw, dh;
  fit(mu_ui::bailey(), L.u(RAIL_W - 46), L.u(480 - (NAV_Y + 5 * NAV_GAP + NAV_H + 10) - 12), &dw, &dh);
  const float dx = L.ox + (L.u(RAIL_W) - dw) * 0.5f, dy = top + (std::min(L.h, L.oy + L.u(480)) - top - dh) * 0.5f;
  art(d, mu_ui::bailey(), ImVec2(dx, dy), ImVec2(dx + dw, dy + dh));
}

// An owner-drawn button (draw_button): PLAY's purple gradient or the navy secondary, with its focus ring.
void button(ImDrawList* d, const Layout& L, const Fonts& f, float x, float y, float w, float hh, const char* label,
            bool primary, bool focused, bool disabled) {
  COLORREF top, bot, border = NO_FILL, ink;
  if (primary) {
    top = C_PLAY_HI; bot = C_PLAY_LO; ink = C_PLAY_TEXT;
    if (disabled) { top = RGB(0x36, 0x33, 0x40); bot = RGB(0x30, 0x2D, 0x3A); ink = RGB(0x78, 0x74, 0x84); }
  } else {
    top = bot = C_BTN; border = C_BTN_BORDER; ink = C_TEXT;
    if (disabled) { top = bot = RGB(0x1A, 0x21, 0x32); border = RGB(0x28, 0x31, 0x47); ink = RGB(0x5C, 0x68, 0x7E); }
  }
  const ImVec2 a = L.p(x, y), b = L.p(x + w, y + hh);
  round_rect(d, a, b, L.u(primary ? 10 : 7), top, bot, border);
  if (focused) round_rect(d, L.p(x + 3, y + 3), L.p(x + w - 3, y + hh - 3), L.u(primary ? 8 : 5), NO_FILL, NO_FILL, primary ? C_PLAY_TEXT : C_ACC_HI);
  text(d, f.bold, L.u(primary && hh >= 40 ? 21 : 12), a, b, ink, label, 1);
}

// The Play page's focusable controls, in order.
enum PlayItem { PI_BUILD, PI_PLAY, PI_UPDATE, PI_SIGNIN, PI_THEME, PI_COUNT };
std::vector<int> play_items(const Home& h) {
  std::vector<int> v{PI_BUILD, PI_PLAY};
  { std::lock_guard<std::mutex> g(h.update->lock); if (h.update->offer && !h.update->downloading) v.push_back(PI_UPDATE); }
  v.push_back(PI_SIGNIN);
  v.push_back(PI_THEME);
  return v;
}

void paint_play(ImDrawList* d, const Layout& L, const Fonts& f, Home& h) {
  const std::vector<int> items = play_items(h);
  const int focused = h.rail ? -1 : items[size_t(std::clamp(h.focus, 0, int(items.size()) - 1))];
  text(d, f.bold, L.u(10), L.p(CX, 32), L.p(CX + CW, 50), C_FAINT, "MELEE NTSC 1.02 DISC IMAGE");
  round_rect(d, L.p(CX, 58), L.p(CX + CW, 92), L.u(7), C_FIELD, C_FIELD, C_FIELD_BORDER);
  text(d, f.body, L.u(12), L.p(CX + 10, 58), L.p(CX + CW - 10, 92), C_TEXT, options.iso);
  // GAME BUILD: the Source Port is what runs on the Switch; Vanilla is the Source Port as the retail game (settings.ini
  // vanilla 1). Static Recomp is not on the console: drawn as not installed, as the launcher draws a missing build.
  text(d, f.bold, L.u(10), L.p(CX, 102), L.p(CX + 130, 121), C_FAINT, "GAME BUILD");
  const bool vanilla = settings_get("vanilla") == "1";
  {
    const int left = CX + 84, total = CW - 84, gap = 6, narrow = 78, w = (total - 2 * gap - narrow) / 2;
    const char* names[3] = {"Source Port", "Static Recomp (not installed)", "Vanilla"};
    const int on = vanilla ? 2 : 0;
    for (int i = 0; i < 3; ++i) {
      const int x = i == 0 ? left : i == 1 ? left + w + gap : left + 2 * (w + gap);
      const int sw = i == 2 ? narrow : w;
      COLORREF top = C_BTN, bot = C_BTN, border = C_BTN_BORDER, ink = C_DIM;
      if (i == on) { top = C_ACC_HI; bot = C_ACC_LO; border = NO_FILL; ink = C_PLAY_TEXT; }
      else if (i == 1) { top = bot = RGB(0x1A, 0x21, 0x32); border = RGB(0x28, 0x31, 0x47); ink = RGB(0x5C, 0x68, 0x7E); }
      round_rect(d, L.p(x, 98), L.p(x + sw, 126), L.u(7), top, bot, border);
      if (focused == PI_BUILD && i == on) round_rect(d, L.p(x + 2, 100), L.p(x + sw - 2, 124), L.u(5), NO_FILL, NO_FILL, C_PLAY_TEXT);
      text(d, f.body, L.u(11), L.p(x, 98), L.p(x + sw, 126), ink, names[i], 1);
    }
  }
  button(d, L, f, CX, 134, CW, 62, "PLAY", true, focused == PI_PLAY, false);
  // Status rows: one even list, a dot then text, every row on the same pitch (play_layout without the YouGame rows).
  const int pitch = 24;
  int y = 210;
  const int version_y = y; y += pitch;
  const int slippi_y = y; const int slippi_h = h.signed_in ? 17 : 34; y += slippi_h + (pitch - 17);
  const int mods_y = y; y += pitch;
  const int sep_y = y + 1, note_y = sep_y + 9, buttons_y = note_y + 32 + 8;
  // Version and the update
  std::string version = std::string("Version ") + MU_APP_VERSION, link;
  COLORREF vdot = C_OK;
  std::string status;
  bool status_bad = false;
  {
    std::lock_guard<std::mutex> g(h.update->lock);
    const auto& u = *h.update;
    if (u.downloading) {
      char line[96];
      std::snprintf(line, sizeof line, "Downloading %s: %llu of %llu MB", u.manifest.app_version.c_str(), (unsigned long long)(u.done.load() >> 20), (unsigned long long)(u.size >> 20));
      status = line; vdot = C_WARN;
    } else if (u.offer) { link = "Update available: " + u.manifest.app_version; vdot = C_WARN; }
    else if (u.checked && u.ok) link = "Up to date";
    if (u.failed) { status = u.status; status_bad = true; }
  }
  if (!link.empty()) version += "   |   " + link;
  dot(d, L, CX, version_y + 3, vdot);
  text(d, f.body, L.u(12), L.p(CX + 15, version_y), L.p(CX + 334, version_y + 17), C_DIM, version);
  bool update_on = false;
  for (int i : items) update_on = update_on || i == PI_UPDATE;
  if (update_on) button(d, L, f, 554, version_y - 4, 144, 25, "Update and restart", false, focused == PI_UPDATE, false);
  // Slippi account
  dot(d, L, CX, slippi_y + 3, h.signed_in ? C_OK : C_WARN);
  const std::string slippi = h.signed_in ? "Slippi: " + h.account
                                         : "No Slippi account on this console. Sign in to play online; without one, Online Play shows Log-in.";
  text(d, f.body, L.u(12), L.p(CX + 15, slippi_y), L.p(CX + 334, slippi_y + slippi_h), C_DIM, slippi, 0, !h.signed_in);
  button(d, L, f, 554, slippi_y + 2, 144, 30, h.signed_in ? "Sign in again" : "Sign in", false, focused == PI_SIGNIN, false);
  // Mods
  const std::string profile = settings_get("mod_profile");
  dot(d, L, CX, mods_y + 3, profile.empty() ? C_FAINT : C_OK);
  text(d, f.body, L.u(12), L.p(CX + 15, mods_y), L.p(CX + CW, mods_y + 17), C_DIM,
       profile.empty() ? "Mods: none active   |   Mods page" : "Mods: " + profile + " active   |   Mods page");
  d->AddRectFilled(L.p(CX, sep_y), L.p(CX + CW, sep_y + 1), col(C_SEP));
  text(d, f.body, L.u(11), L.p(CX, note_y), L.p(CX + CW, note_y + 32), C_FAINT,
       "L + R + Start, or Start + D-pad Down + Z, opens settings in game. GameCube adapters work automatically.", 0, true);
  // The launcher colour (its theme wheel), and what is playing.
  const int accent = std::clamp(std::atoi(app_ini("mu_accent", "0").c_str()), 0, 5);
  {
    const ImVec2 c = L.p(666 + 16, buttons_y + 16);
    for (int k = 0; k < 6; ++k) {
      const float a0 = k * 3.14159f / 3.0f, a1 = a0 + 3.14159f / 3.0f;
      d->PathLineTo(c); d->PathArcTo(c, L.u(13), a0, a1, 8); d->PathFillConvex(col(kAccents[k]));
    }
    d->AddCircle(c, L.u(13), col(focused == PI_THEME ? C_TEXT : C_BTN_BORDER), 24, focused == PI_THEME ? 2.0f : 1.0f);
    d->AddCircleFilled(c, L.u(5), col(kAccents[accent]), 16);
  }
  text(d, f.body, L.u(11), L.p(398, buttons_y), L.p(586 + 70, buttons_y + 32), status_bad ? C_BAD : C_DIM,
       status.empty() ? std::string("Playing ") + MU_APP_VERSION : status, 2);
  text(d, f.body, L.u(11), L.p(CX, buttons_y), L.p(CX + 190, buttons_y + 32), C_FAINT, "A  Select     B  Pages");
}

void paint_replays(ImDrawList* d, const Layout& L, const Fonts& f, Home& h) {
  text(d, f.bold, L.u(10), L.p(CX, 32), L.p(CX + CW, 50), C_FAINT, "REPLAY VIEWER");
  const std::vector<std::string> list = &features::library_list ? features::library_list() : std::vector<std::string>{};
  const auto rows = page_entries("Replays");
  if (list.empty()) {
    text(d, f.body, L.u(12), L.p(CX, 58), L.p(CX + CW, 90), C_DIM,
         &features::library_list ? "No replays yet. Every match you play is saved in switch/melee-nx-netplay/Replays." : "Replays are not part of this build.", 0, true);
    return;
  }
  h.replay_sel = std::clamp(h.replay_sel, 0, int(list.size()) - 1);
  // The list: one row a game; the picked one opens with its line, date and stats (launcher_replays.inl's card).
  const int visible = 6, row_h = 26;
  if (h.replay_sel < h.replay_scroll) h.replay_scroll = h.replay_sel;
  if (h.replay_sel >= h.replay_scroll + visible) h.replay_scroll = h.replay_sel - visible + 1;
  for (int i = 0; i < visible && h.replay_scroll + i < int(list.size()); ++i) {
    const int k = h.replay_scroll + i, y = 58 + i * (row_h + 4);
    const bool sel = k == h.replay_sel;
    round_rect(d, L.p(CX, y), L.p(CX + CW, y + row_h), L.u(7), sel ? C_NAV_ON : C_FIELD, sel ? C_NAV_ON : C_FIELD, sel && !h.rail ? C_ACC_HI : C_FIELD_BORDER);
    std::string name = list[size_t(k)];
    if (sel) if (const auto* e = entry(rows, "Replay")) if (e->value) name = e->value();
    text(d, f.body, L.u(12), L.p(CX + 12, y), L.p(CX + CW - 12, y + row_h), sel ? C_TEXT : C_DIM, name);
  }
  const int sy = 58 + visible * (row_h + 4) + 6;
  d->AddRectFilled(L.p(CX, sy), L.p(CX + CW, sy + 1), col(C_SEP));
  // Stats of the picked game, two columns.
  const char* stats[] = {"Recorded", "Players", "Kills", "Damage done", "Openings per kill", "Neutral wins", "L-cancels", "Inputs per minute"};
  for (int i = 0; i < 8; ++i) {
    const auto* e = entry(rows, stats[i]);
    if (!e || !e->value) continue;
    const int x = CX + (i % 2) * (CW / 2), y = sy + 8 + (i / 2) * 22;
    text(d, f.bold, L.u(10), L.p(x, y), L.p(x + 110, y + 18), C_FAINT, stats[i]);
    text(d, f.body, L.u(11), L.p(x + 112, y), L.p(x + CW / 2 - 6, y + 18), C_TEXT, e->value());
  }
  const int by = sy + 8 + 4 * 22 + 6;
  button(d, L, f, CX, by, 160, 32, "Watch", false, !h.rail && h.focus == 0, false);
  std::string del = "Delete";
  if (const auto* e = entry(rows, "Delete this replay")) if (e->value) { const std::string v = e->value(); if (!v.empty()) del = v; }
  button(d, L, f, CX + 170, by, 200, 32, del.c_str(), false, !h.rail && h.focus == 1, false);
  text(d, f.body, L.u(11), L.p(CX + 380, by), L.p(CX + CW, by + 32), C_FAINT, "Up/Down pick", 2);
}

// The Mods page, as melee-unlocked's launcher draws its mod cards (launcher_mods.inl paint_page): a check, the name and the
// latest version, the credits, the state (green once installed), the last line (what is happening, or the size and
// licence) with a progress bar; the picked card carries its buttons (Get / Update / Cancel / Play it at the next start,
// Remove). Under the cards: the profile the next start loads.
void paint_mods(ImDrawList* d, const Layout& L, const Fonts& f, Home& h) {
  const COLORREF C_CARD = RGB(0x14, 0x1C, 0x2C), C_CARD_BORDER = RGB(0x27, 0x31, 0x4A);   // launcher_mods.inl's card colours
  text(d, f.bold, L.u(10), L.p(CX, 26), L.p(CX + CW, 44), C_FAINT, "MODS");
  text(d, f.body, L.u(11), L.p(CX, 44), L.p(CX + CW, 60), C_DIM, "Get downloads each mod from its official release and builds it from your own Melee disc.");
  std::string error;
  const std::vector<mu_mods::Card> cards = mu_mods::cards(&error);
  const int count = int(cards.size()) + 1;   // the cards, then the profile row
  h.mod_sel = std::clamp(h.mod_sel, 0, count - 1);
  if (!error.empty()) text(d, f.body, L.u(12), L.p(CX, 70), L.p(CX + CW, 90), C_WARN, error);
  const int card_h = 92, gap = 6, visible = 3, top = 66;
  if (h.mod_sel < h.mod_scroll) h.mod_scroll = h.mod_sel;
  if (h.mod_sel >= h.mod_scroll + visible && h.mod_sel < int(cards.size())) h.mod_scroll = h.mod_sel - visible + 1;
  h.mod_scroll = std::clamp(h.mod_scroll, 0, std::max(0, int(cards.size()) - visible));
  for (int slot = 0; slot < visible && h.mod_scroll + slot < int(cards.size()); ++slot) {
    const int k = h.mod_scroll + slot, y = top + slot * (card_h + gap);
    const auto& c = cards[size_t(k)];
    const auto& m = c.status.mod;
    const bool sel = !h.rail && h.mod_sel == k;
    round_rect(d, L.p(CX, y), L.p(CX + CW, y + card_h), L.u(10), C_CARD, C_CARD, sel ? C_ACC_HI : C_CARD_BORDER);
    // draw_check
    const ImVec2 cc = L.p(CX + 16 + 9, y + 14 + 9);
    if (c.status.state == modinstall::State::Installed || c.status.state == modinstall::State::UpdateAvailable) {
      d->AddCircleFilled(cc, L.u(9), col(C_OK), 20);
      d->AddPolyline(std::array<ImVec2, 3>{L.p(CX + 21, y + 23), L.p(CX + 24, y + 26), L.p(CX + 29, y + 20)}.data(), 3, col(RGB(0x0E, 0x1A, 0x14)), 0, L.u(2));
    } else d->AddCircle(cc, L.u(9), col(C_FAINT), 20, 1.0f);
    const int tx = CX + 44, tw = CW - 44 - 16;
    const float name_w = f.bold->CalcTextSizeA(L.u(14), FLT_MAX, 0, m.name.c_str()).x / L.s;
    text(d, f.bold, L.u(14), L.p(tx, y + 8), L.p(tx + tw, y + 28), C_TEXT, m.name);
    if (!m.version.empty() && name_w + 8 < tw) text(d, f.body, L.u(11), L.p(tx + name_w + 8, y + 8), L.p(tx + tw, y + 28), C_FAINT, "Latest " + m.version);
    text(d, f.body, L.u(11), L.p(tx, y + 27), L.p(tx + tw, y + 42), C_FAINT, m.credits);
    text(d, f.body, L.u(12), L.p(tx, y + 42), L.p(tx + tw, y + 58), c.state_ok ? C_OK : C_DIM, c.state);
    const std::string last = c.status.percent >= 0 || c.status.state == modinstall::State::Working ? c.status.line : !c.get_it.empty() ? c.get_it : c.detail;
    text(d, f.body, L.u(11), L.p(tx, y + 57), L.p(tx + tw, y + 72), c.status.failed ? C_WARN : C_DIM, last);
    if (c.status.percent >= 0) {
      round_rect(d, L.p(tx, y + 73), L.p(tx + tw, y + 77), L.u(2), C_FIELD, C_FIELD, NO_FILL);
      const float done = float(tw) * std::clamp(c.status.percent, 0, 100) / 100.0f;
      if (done > 4) round_rect(d, L.p(tx, y + 73), L.p(tx + int(done), y + 77), L.u(2), C_ACC_HI, C_ACC_LO, NO_FILL);
    }
    if (sel) {   // the picked card's buttons, at its foot
      button(d, L, f, CX + CW - 16 - 120 - (c.can_remove ? 86 : 0), y + card_h - 22, 120, 18, c.primary.c_str(), true, h.mod_button == 0, !c.primary_enabled);
      if (c.can_remove) button(d, L, f, CX + CW - 16 - 80, y + card_h - 22, 80, 18, c.remove_armed ? "Press A again" : "Remove", false, h.mod_button == 1, false);
    }
  }
  // The profile the next start loads.
  const int py = top + visible * (card_h + gap) + 2;
  const std::string next = mu_mods::profile_next();
  const bool psel = !h.rail && h.mod_sel == int(cards.size());
  round_rect(d, L.p(CX, py), L.p(CX + CW, py + 30), L.u(7), C_FIELD, C_FIELD, psel ? C_ACC_HI : C_FIELD_BORDER);
  dot(d, L, CX + 10, py + 11, next.empty() ? C_FAINT : C_OK);
  text(d, f.body, L.u(12), L.p(CX + 28, py), L.p(CX + CW - 12, py + 30), C_TEXT,
       "Next start: " + (next.empty() ? std::string("the retail game (no mod)") : next) + (psel ? "      < Left / Right >" : ""));
  text(d, f.body, L.u(10), L.p(CX, py + 34), L.p(CX + CW, py + 50), C_FAINT,
       "Up/Down: pick   Left/Right: button   A: press.   Saves stay when you remove a mod. Online play uses no mod.");
}

void paint_message(ImDrawList* d, const Layout& L, const Fonts& f, const Home& h) {
  d->AddRectFilled(ImVec2(0, 0), ImVec2(L.w, L.h), IM_COL32(0, 0, 0, 150));
  const float x = CX, y = 120, w = CW, hh = 40 + 22 * float(h.msg_lines.size()) + 60;
  round_rect(d, L.p(x, y), L.p(x + w, y + hh), L.u(10), RGB(0x14, 0x1C, 0x2C), RGB(0x14, 0x1C, 0x2C), C_ACC_LO);
  text(d, f.bold, L.u(16), L.p(x + 18, y + 10), L.p(x + w - 18, y + 36), C_TEXT, h.msg_title);
  float ly = y + 44;
  for (const auto& line : h.msg_lines) { text(d, f.body, L.u(12), L.p(x + 18, ly), L.p(x + w - 18, ly + 18), C_DIM, line); ly += 22; }
  button(d, L, f, x + w - 140, y + hh - 46, 122, 32, h.msg_quits ? "Close app" : "OK", true, true, false);
}

// ---- actions ----
void start_download(Home& h, const std::string& update_host) {
  auto u = h.update;
  {
    std::lock_guard<std::mutex> g(u->lock);
    if (u->downloading || !u->offer) return;
    u->downloading = true; u->failed = false; u->done = 0;
    u->size = u->manifest.files.at("game").size;
  }
  const std::string nro = options.self_path;
  say("update: the player chose to install %s", u->manifest.app_version.c_str());
  h.download = std::thread([u, update_host, nro] {
    su::Manifest m; { std::lock_guard<std::mutex> g(u->lock); m = u->manifest; }
    const su::FileEntry entry = m.files.at("game");
    std::string error;
    const su::Fetch fetch = su::curl_fetch(3000, 20);
    const bool ok = su::download_verified(fetch, su::join_url(update_host, entry.name), entry, nro,
                                          [&](uint64_t got, uint64_t) { u->done = got; return true; }, error);
    std::lock_guard<std::mutex> g(u->lock);
    u->downloading = false; u->finished = true;
    if (!ok) { u->failed = true; u->status = "The download did not finish: " + error; }
  });
}

void profile_step(int dir);
void activate(Home& h, int item, const std::string& update_host) {
  switch (h.page) {
    case PAGE_PLAY:
      switch (item) {
        case PI_PLAY: h.done = true; h.result = h.signed_in ? AppStart::Play : AppStart::PlayOffline; say("home: PLAY"); break;
        case PI_BUILD: {
          const bool vanilla = settings_get("vanilla") == "1";
          settings_change("vanilla", vanilla ? "0" : "1");
          say("home: game build %s", vanilla ? "Source Port" : "Vanilla");
          break;
        }
        case PI_UPDATE: start_download(h, update_host); break;
        case PI_SIGNIN: {
          std::string name, code;
          say("home: Sign in");
          const host::SignInResult r = host::switch_signin_run(*h.frontend, h.user_dir, name, code);
          if (r == host::SignInResult::SignedIn) say("app: sign-in: signed in as %s (%s)", name.c_str(), code.c_str());
          else if (r == host::SignInResult::PlayOffline) say("app: sign-in: playing offline");
          else { say("app: sign-in: quit"); h.done = true; h.result = AppStart::Quit; }
          h.account = read_account(h.user_dir, &h.signed_in);
          h.pad_reset = true;   // what was pressed on the sign-in screen (its B) is not the home screen's
          break;
        }
        case PI_THEME: {
          const int next = (std::clamp(std::atoi(app_ini("mu_accent", "0").c_str()), 0, 5) + 1) % 6;
          app_ini_set("mu_accent", std::to_string(next));
          break;
        }
      }
      break;
    case PAGE_REPLAYS: {
      const auto rows = page_entries("Replays");
      if (item == 0) {
        const std::vector<std::string> list = &features::library_list ? features::library_list() : std::vector<std::string>{};
        if (!list.empty() && &features::replay_watch) {
          // The library's own Watch asks play mode to start again with the replay; before the game, the first start is it.
          const std::string name = list[size_t(std::clamp(h.replay_sel, 0, int(list.size()) - 1))];
          home_replay = (&features::replay_folder ? features::replay_folder() + "/" : std::string()) + name;
          say("home: watch %s", home_replay.c_str());
          h.done = true; h.result = AppStart::PlayOffline;
        }
      } else if (item == 1) {
        if (const auto* e = entry(rows, "Delete this replay")) if (e->press) e->press();
      }
      break;
    }
    case PAGE_MODS: {
      const std::vector<mu_mods::Card> cards = mu_mods::cards();
      if (h.mod_sel < int(cards.size())) {
        const auto& c = cards[size_t(h.mod_sel)];
        if (h.mod_button == 1 && c.can_remove) mu_mods::remove(c.status.mod.id);
        else if (c.primary_enabled) mu_mods::primary(c.status.mod.id);
      } else profile_step(1);
      break;
    }
  }
}

// The profile row of the Mods page: the next start's profile, stepped through the choices.
void profile_step(int dir) {
  const std::vector<std::string> choices = mu_mods::profile_choices();
  if (choices.empty()) return;
  const auto it = std::find(choices.begin(), choices.end(), mu_mods::profile_next());
  const int n = int(choices.size()), at = it == choices.end() ? 0 : int(it - choices.begin());
  mu_mods::profile_set(choices[size_t(((at + dir) % n + n) % n)]);
}

// Moves the replay picker by `dir` (the library's own "Replay" row steps its picked game).
void replay_step(Home& h, int dir) {
  const std::vector<std::string> list = &features::library_list ? features::library_list() : std::vector<std::string>{};
  if (list.empty()) return;
  const int next = std::clamp(h.replay_sel + dir, 0, int(list.size()) - 1);
  if (next == h.replay_sel) return;
  h.replay_sel = next;
  const auto rows = page_entries("Replays");
  if (const auto* e = entry(rows, "Replay")) if (e->change) e->change(dir);
}

void home_job(void* frontend_v, void* user) {
  Home& h = *static_cast<Home*>(user);
  auto& frontend = *static_cast<host::MetalFrontend*>(frontend_v);
  h.frontend = &frontend;
  PADInit();
  PadReader pad;
  mu_ui::panel_home_mode(true);
  const std::string update_host = app_ini("update_host", "");
  // The Replay Viewer's library reads its folder on a worker thread: ask it now (its "Sort" row value pokes it).
  for (const auto& e : page_entries("Replays")) if (e.value) e.value();
  auto next_frame = std::chrono::steady_clock::now();
  while (!h.done) {
    // One frame a 60th of a second: the screen has nothing to animate faster, and a loop that spins as fast as the
    // renderer returns kept the window system's connection busy (a stall seen under Xvfb with the software driver).
    next_frame += std::chrono::microseconds(16667);
    const auto now = std::chrono::steady_clock::now();
    if (next_frame > now) std::this_thread::sleep_until(next_frame); else next_frame = now;
    if (!frontend.pump_events() || frontend.close_requested()) { h.result = AppStart::Quit; break; }
    if (h.pad_reset) { pad.last = 0xFFFF; h.pad_reset = false; }
    const Edges e = pad.read();
    const bool panel = mu_ui::panel_open();
    if (panel) {
      mu_ui::panel_feed(pad.pads);
    } else if (!h.msg_title.empty()) {
      if (e.a || e.b) { if (h.msg_quits) { h.result = AppStart::Quit; h.done = true; } h.msg_title.clear(); }
    } else {
      // Rail: Up/Down pick a page (it shows at once), Right or A enters it. Page: Up/Down move, A acts, B or Left on the
      // first column returns to the rail.
      if (h.rail) {
        if (e.up) h.focus = (h.focus + PAGE_COUNT - 1) % PAGE_COUNT;
        if (e.down) h.focus = (h.focus + 1) % PAGE_COUNT;
        if (e.up || e.down) { if (h.focus != PAGE_SETTINGS) h.page = h.focus; }
        if (e.a || e.right) {
          if (h.focus == PAGE_SETTINGS) { mu_ui::panel_open_on(""); say("home: Settings"); }
          else { h.page = h.focus; h.rail = false; h.focus = h.page == PAGE_PLAY ? 1 : 0; }
        }
      } else {
        int count = 1;
        if (h.page == PAGE_PLAY) count = int(play_items(h).size());
        else if (h.page == PAGE_REPLAYS) count = 2;
        else if (h.page == PAGE_MODS) count = int(mu_mods::cards().size()) + 1;
        if (h.page == PAGE_REPLAYS) { if (e.up) replay_step(h, -1); if (e.down) replay_step(h, 1); if (e.left) h.focus = 0; if (e.right) h.focus = 1; }
        else if (h.page == PAGE_MODS) {
          if (e.up) { h.mod_sel = std::max(0, h.mod_sel - 1); h.mod_button = 0; }
          if (e.down) { h.mod_sel = std::min(count - 1, h.mod_sel + 1); h.mod_button = 0; }
          if (h.mod_sel == count - 1 && (e.left || e.right)) profile_step(e.left ? -1 : 1);
          else if (e.right) h.mod_button = 1;
          else if (e.left && h.mod_button == 1) h.mod_button = 0;
          else if (e.left) { h.rail = true; h.focus = h.page; }
        }
        else { if (e.up) h.focus = std::max(0, h.focus - 1); if (e.down) h.focus = std::min(count - 1, h.focus + 1); }
        if (h.page == PAGE_PLAY && h.focus >= 0 && h.focus < count && play_items(h)[size_t(h.focus)] == PI_BUILD && (e.left || e.right)) activate(h, PI_BUILD, update_host);
        else if (e.left && h.page != PAGE_REPLAYS && h.page != PAGE_MODS) { h.rail = true; h.focus = h.page; }
        if (e.b) { h.rail = true; h.focus = h.page; }
        if (e.a) {
          if (h.page == PAGE_PLAY) activate(h, play_items(h)[size_t(std::clamp(h.focus, 0, count - 1))], update_host);
          else activate(h, h.focus, update_host);
        }
      }
    }
    mu_ui::panel_service(false);
    if (mu_ui::panel_take_quit()) { h.result = AppStart::Quit; h.done = true; }
    // A finished update: installed (the app closes) or not (the old version stays).
    {
      bool finished = false, failed = false; std::string version;
      { std::lock_guard<std::mutex> g(h.update->lock); finished = h.update->finished; failed = h.update->failed; version = h.update->manifest.app_version; h.update->finished = false; }
      if (finished) {
        if (h.download.joinable()) h.download.join();
        const std::string nro = options.self_path;
        if (failed) {
          std::remove(su::marker_path(nro).c_str()); std::remove(su::staged_path(nro).c_str());
          say("update: the game not installed");
        } else {
          std::string swap_error;
          const su::Swap own = su::apply_pending(nro, swap_error);
          say("update: the game downloaded and verified; %s", own == su::Swap::Done ? "the new version is in place" : ("the new version is swapped in at the next start (" + swap_error + ")").c_str());
          h.msg_title = "Update installed";
          h.msg_lines = {"Version " + version + " has been downloaded and checked.",
                         own == su::Swap::Done ? "The app closes now. Start it again." : "The app closes now. Start it again to finish the update."};
          h.msg_quits = true;
        }
      }
    }
    const host::MetalWindowSize size = frontend.window_size();
    if (!aurora_begin_frame()) continue;
    {
      Layout L;
      L.w = float(size.width ? size.width : 1280); L.h = float(size.height ? size.height : 720);
      L.s = std::min(L.w / WIN_W, L.h / WIN_H);
      L.ox = std::floor((L.w - WIN_W * L.s) * 0.5f); L.oy = std::floor((L.h - WIN_H * L.s) * 0.5f);
      launcher::theme::accent = kAccents[std::clamp(std::atoi(app_ini("mu_accent", "0").c_str()), 0, 5)];
      Fonts f{mu_ui::body_font() ? mu_ui::body_font() : ImGui::GetFont(), mu_ui::body_font() ? mu_ui::body_font() : ImGui::GetFont()};
      ImDrawList* d = ImGui::GetBackgroundDrawList();
      vgrad(d, ImVec2(0, 0), ImVec2(L.w, L.h), C_CONTENT_TOP, C_CONTENT_BOT);
      paint_rail(d, L, f, h);
      if (h.page == PAGE_PLAY) paint_play(d, L, f, h);
      else if (h.page == PAGE_REPLAYS) paint_replays(d, L, f, h);
      else if (h.page == PAGE_MODS) paint_mods(d, L, f, h);
      if (!h.msg_title.empty()) paint_message(ImGui::GetForegroundDrawList(), L, f, h);
      mu_ui::panel_draw(int(L.w), int(L.h));
      if (++h.frames == 30) say("home: drawn (%s page)", kNav[h.page]);
    }
    aurora_end_frame();
  }
  if (h.download.joinable()) h.download.join();
  if (mu_ui::panel_open()) mu_ui::panel_close();
  mu_ui::panel_service(false);
  mu_ui::panel_home_mode(false);
}

// A message on the launcher's background (the disc is missing, the layout failed): OK continues, + quits.
struct Message { std::string headline; std::vector<std::string> lines; bool ok = true; };
void message_job(void* frontend_v, void* user) {
  auto& m = *static_cast<Message*>(user);
  auto& frontend = *static_cast<host::MetalFrontend*>(frontend_v);
  PADInit();
  PadReader pad;
  Home h; h.msg_title = m.headline; h.msg_lines = m.lines;
  for (;;) {
    if (!frontend.pump_events() || frontend.close_requested()) { m.ok = false; return; }
    const Edges e = pad.read();
    if (e.a) { m.ok = true; return; }
    if (e.plus || e.b) { m.ok = false; return; }
    const host::MetalWindowSize size = frontend.window_size();
    if (!aurora_begin_frame()) continue;
    Layout L;
    L.w = float(size.width ? size.width : 1280); L.h = float(size.height ? size.height : 720);
    L.s = std::min(L.w / WIN_W, L.h / WIN_H);
    L.ox = std::floor((L.w - WIN_W * L.s) * 0.5f); L.oy = std::floor((L.h - WIN_H * L.s) * 0.5f);
    Fonts f{mu_ui::body_font() ? mu_ui::body_font() : ImGui::GetFont(), mu_ui::body_font() ? mu_ui::body_font() : ImGui::GetFont()};
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    vgrad(d, ImVec2(0, 0), ImVec2(L.w, L.h), C_CONTENT_TOP, C_CONTENT_BOT);
    h.page = -1;
    paint_rail(d, L, f, h);
    paint_message(ImGui::GetForegroundDrawList(), L, f, h);
    aurora_end_frame();
  }
}
}  // namespace

bool mu_home_message(const std::string& headline, const std::vector<std::string>& lines) {
  Message m{headline, lines};
  if (!screens_run(message_job, &m)) return true;
  return m.ok;
}

// The home screen: until PLAY (or Watch, or the app is closed). `update_check` starts the build computer's check in the
// background: its answer shows on the Play page's version line, and "Update and restart" installs it there.
AppStart mu_home_run(const std::string& user_dir, bool update_check) {
  Home h;
  h.user_dir = user_dir;
  h.account = read_account(user_dir, &h.signed_in);
  h.focus = 1;   // PLAY
  const std::string update_host = app_ini("update_host", "");
  if (update_check && !update_host.empty()) {
    const std::string url = su::join_url(update_host, "latest.json");
    if (url.empty()) say("update: update_host is not an http:// address; ignored");
    else {
      auto u = h.update;
      std::thread([url, u] {
        std::string text, error;
        const bool fetched = su::curl_fetch(1500, 3)(url, [&](const void* data, size_t size) {
          if (text.size() + size > 64 * 1024) return false;
          text.append(static_cast<const char*>(data), size);
          return true;
        }, error);
        su::Manifest manifest;
        const bool ok = fetched && su::parse_manifest(text, manifest, error);
        std::lock_guard<std::mutex> g(u->lock);
        u->checked = true; u->ok = ok; u->manifest = manifest; u->error = error;
        if (!ok) say("update: no answer from the build computer (%s)", error.c_str());
        else {
          say("update: the build computer has version %s (Slippi %s); this is %s (Slippi %s)", manifest.app_version.c_str(), manifest.slippi_version.c_str(), MU_APP_VERSION, MU_SLIPPI_VERSION);
          u->offer = su::should_offer(manifest, MU_APP_VERSION, MU_SLIPPI_VERSION) && manifest.files.count("game");
          if (u->offer) say("update: offered on the Play page");
        }
      }).detach();
    }
  }
  say("home: melee-unlocked's launcher (Play, Settings, Replay Viewer, Mods)%s", h.signed_in ? "" : "; no Slippi account on this console");
  if (!screens_run(home_job, &h)) return h.signed_in ? AppStart::Play : AppStart::PlayOffline;
  return h.result;
}
}  // namespace fe
