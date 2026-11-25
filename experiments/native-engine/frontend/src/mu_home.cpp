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
//   Multiplayer Lobby  its serverless peer lobby (src/lobby.cpp, src/mu_home_lobby.inl): roster, chat, friends, profile, match
//                  requests; an accepted match starts the game on Slippi Direct against that player
// Also its languages (mu_lang.cpp), What's new, the colour wheel, the rail picture, the Replay Viewer's search, queue, folders,
// compression and trash, and "Save crash report" (diagnostics.cpp) in place of Send logs. Left out: Build (no ISO verification or
// shader build on the console), everything of YouGame, Choose version. The scale follows the screen: 1.5 at 720p handheld, 2.25 at 1080p
// docked, with the rail and the gradient running to the screen's edges.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"
#include "mu_ui.h"
#include "feature_hooks.h"
#include "feature_calls.h"
#include "mu_mods.h"
#include "mu_lang.h"
#include "lobby.h"

#include "host.h"
#include "metal_frontend.h"
#include "self_update.h"
#include "switch_signin.h"

#include <SDL3/SDL.h>
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
#include <ctime>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>

namespace host { void signin_pad_read(PADStatus status[4]); }
namespace fe::features {
__attribute__((weak)) std::vector<std::string> library_list();
__attribute__((weak)) void replay_watch(const std::string& path);
__attribute__((weak)) std::string replay_folder();
// replay_library.cpp: search, the watch queue, "as it was played", compression, the trash, the session trace graph
__attribute__((weak)) void library_search();
__attribute__((weak)) std::string library_query();
__attribute__((weak)) void library_set_query(const std::string& text);
__attribute__((weak)) void library_queue_toggle();
__attribute__((weak)) std::string library_queue_state();
__attribute__((weak)) void library_queue_watch();
__attribute__((weak)) size_t library_queue_size();
__attribute__((weak)) std::string library_queue_next();
__attribute__((weak)) std::string library_queue_start();
__attribute__((weak)) void library_compress();
__attribute__((weak)) std::string library_compress_state();
__attribute__((weak)) void library_undo_delete();
__attribute__((weak)) size_t library_trash_size();
__attribute__((weak)) bool library_as_played();
__attribute__((weak)) void library_set_as_played(bool on);
__attribute__((weak)) std::string library_note();
__attribute__((weak)) bool library_trace(std::vector<std::array<float, 5>>* bins, std::string* summary);
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

// The launcher's page order (its Build page is left out): Play, Settings, Multiplayer Lobby, Replay Viewer, Mods.
enum Page { PAGE_PLAY, PAGE_SETTINGS, PAGE_LOBBY, PAGE_REPLAYS, PAGE_MODS, PAGE_COUNT };
constexpr const char* kNav[PAGE_COUNT] = {"Play", "Settings", "Multiplayer Lobby", "Replay Viewer", "Mods"};

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
// Fixed English text shows in the chosen language (mu_lang.cpp, upstream's lookup by wording); paths, names and the lines the
// Switch app adds come back unchanged.
void text(ImDrawList* d, ImFont* f, float size, ImVec2 a, ImVec2 b, COLORREF c, const std::string& english, int align = 0, bool wrap = false) {
  if (english.empty()) return;
  const std::string s = mu_lang::tx(english);
  if (wrap) { d->AddText(f, size, a, col(c), s.c_str(), nullptr, b.x - a.x); return; }
  std::string t = s;
  float w = f->CalcTextSizeA(size, FLT_MAX, 0, t.c_str()).x;
  while (w > b.x - a.x && t.size() > 3) {
    t.resize(t.size() - 4);
    while (!t.empty() && (uint8_t(t.back()) & 0xC0) == 0x80) t.pop_back();   // never half a UTF-8 character
    if (!t.empty() && uint8_t(t.back()) >= 0xC0) t.pop_back();
    t += "..."; w = f->CalcTextSizeA(size, FLT_MAX, 0, t.c_str()).x;
  }
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
  float msg_progress = -1.0f; std::string msg_progress_text;   // >= 0: a progress bar in place of the button (mu_home_progress)
  // Replay Viewer
  int replay_sel = 0, replay_scroll = 0;
  // Mods
  int mod_sel = 0, mod_scroll = 0, mod_button = 0;
  int frames = 0;
  bool pad_reset = false;
  // The language menu (upstream's Language button: each language named in itself) and the launcher colour's wheel (its
  // theme picker: hue around the wheel, saturation out from the middle, brightness below; Up/Down pick the row, Left/Right
  // change it, A keeps it, B puts the old colour back).
  bool lang_open = false; int lang_sel = 0;
  // The SD card browser (the Replays page's "Folder..." and "Open replay..."): folders and, for a file, .slp/.slpz.
  bool browse_open = false, browse_files = false; std::string browse_dir; std::vector<std::string> browse_list; int browse_sel = 0, browse_scroll = 0;
  bool theme_open = false; int theme_row = 0; double theme_h = 268, theme_s = .65, theme_v = .93; COLORREF theme_before = 0;
  // Multiplayer Lobby (mu_home_lobby.inl): the tab, the picked player and friend, the request that already took the focus
  int lobby_tab = 0, lobby_player = 0, lobby_scroll = 0, lobby_friend = 0, lobby_action = 0;   // lobby_action: a player row's Left / Right
  std::string lobby_seen_request, lobby_focus_said, lobby_name, lobby_code, lobby_room;   // lobby_room: "" the public chat
  std::set<std::string> lobby_rooms_seen;
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
const mu_ui::Art& rail_art();
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
  const mu_ui::Art& dog = rail_art();
  fit(dog, L.u(RAIL_W - 46), L.u(480 - (NAV_Y + 5 * NAV_GAP + NAV_H + 10) - 12), &dw, &dh);
  const float dx = L.ox + (L.u(RAIL_W) - dw) * 0.5f, dy = top + (std::min(L.h, L.oy + L.u(480)) - top - dh) * 0.5f;
  art(d, dog, ImVec2(dx, dy), ImVec2(dx + dw, dy + dh));
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
enum PlayItem { PI_BUILD, PI_PLAY, PI_NOTES, PI_UPDATE, PI_SIGNIN, PI_CRASH, PI_LANG, PI_THEME, PI_COUNT };
std::vector<int> play_items(const Home& h) {
  std::vector<int> v{PI_BUILD, PI_PLAY, PI_NOTES};
  { std::lock_guard<std::mutex> g(h.update->lock); if (h.update->offer && !h.update->downloading) v.push_back(PI_UPDATE); }
  v.push_back(PI_SIGNIN);
  if (&mu_ui::crash_report_available && mu_ui::crash_report_available()) v.push_back(PI_CRASH);
  v.push_back(PI_LANG);
  v.push_back(PI_THEME);
  return v;
}

// The launcher colour: launcher.ini accent=#RRGGBB (upstream's key and form); before it, the app's six presets (mu_accent).
COLORREF current_accent() {
  const std::string a = app_ini("accent", "");
  if (a.size() == 7 && a[0] == '#') {
    const unsigned v = unsigned(std::strtoul(a.c_str() + 1, nullptr, 16));
    return RGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
  }
  return kAccents[std::clamp(std::atoi(app_ini("mu_accent", "0").c_str()), 0, 5)];
}

// What's new: the notes of this version (whats_new.inc, the app's own, newest first), opened by itself at the first start
// of a new version (launcher.ini notesseen, upstream's key) and from the version line.
const char* const kWhatsNew[] = {
#include "whats_new.inc"
};
void open_notes(Home& h) {
  h.msg_title = "What's new";
  h.msg_lines.clear();
  for (const char* line : kWhatsNew) h.msg_lines.push_back(line);
  h.msg_quits = false;
}

// A picture of the player's own on the rail in Bailey's place (upstream's rail-image.*): rail-image.png or .bmp in the app
// folder; none, or one that cannot be read: Bailey.
const mu_ui::Art& rail_art() {
  static int loaded = -1;
  static mu_ui::Art art;
  if (loaded != mu_ui::ui_generation()) {   // once per Aurora (the home screen comes back after a lobby match)
    loaded = mu_ui::ui_generation(); art = mu_ui::Art{};
    for (const char* ext : {".png", ".bmp"}) {
      const std::string path = options.base_dir + "/rail-image" + ext;
      if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fclose(f);
        art = mu_ui::picture_file(path);
        say("home: rail picture %s %s", path.c_str(), art.texture ? "shown" : "could not be opened (Bailey stays)");
        break;
      }
    }
  }
  return art.texture ? art : mu_ui::bailey();
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
  if (!link.empty()) version += "   |   " + mu_lang::tx(link);
  dot(d, L, CX, version_y + 3, vdot);
  {   // "Version x | ... | What's new": the last part is the link (upstream's version line)
    const std::string notes = mu_lang::tx("What's new");
    const float vw = std::min(f.body->CalcTextSizeA(L.u(12), FLT_MAX, 0, version.c_str()).x / L.s, 334.0f - 15 - 90);
    text(d, f.body, L.u(12), L.p(CX + 15, version_y), L.p(CX + 15 + vw + 2, version_y + 17), C_DIM, version);
    const float nx = CX + 15 + vw + 8;
    text(d, f.body, L.u(12), L.p(nx, version_y), L.p(nx + 14, version_y + 17), C_FAINT, "|");
    text(d, f.body, L.u(12), L.p(nx + 14, version_y), L.p(CX + 334, version_y + 17), focused == PI_NOTES ? C_TEXT : C_ACC_HI, notes);
    if (focused == PI_NOTES) {
      const float uw = std::min(f.body->CalcTextSizeA(L.u(12), FLT_MAX, 0, notes.c_str()).x / L.s, CX + 334 - nx - 14);
      d->AddRectFilled(L.p(nx + 14, version_y + 16), L.p(nx + 14 + uw, version_y + 17), col(C_ACC_HI));
    }
  }
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
  if (!status.empty()) text(d, f.body, L.u(11), L.p(CX, note_y), L.p(CX + CW, note_y + 32), status_bad ? C_BAD : C_DIM, status, 0, true);
  else text(d, f.body, L.u(11), L.p(CX, note_y), L.p(CX + CW, note_y + 32), C_FAINT,
            "L + R + Start, or Start + D-pad Down + Z, opens settings in game. GameCube adapters work automatically.", 0, true);
  // The bottom row (upstream's Play page): the crash report, the Language button, the launcher colour (its theme wheel).
  {
    const ImVec2 c = L.p(666 + 16, buttons_y + 16);
    for (int k = 0; k < 24; ++k) {
      const float a0 = k * 3.14159f / 12.0f, a1 = a0 + 3.14159f / 12.0f;
      d->PathLineTo(c); d->PathArcTo(c, L.u(13), a0, a1, 4); d->PathFillConvex(col(launcher::theme::hsv(360.0 - k * 15.0, .75, 1)));
    }
    d->AddCircle(c, L.u(13), col(focused == PI_THEME ? C_TEXT : C_BTN_BORDER), 24, focused == PI_THEME ? 2.0f : 1.0f);
    d->AddCircleFilled(c, L.u(5), col(current_accent()), 16);
  }
  {
    const auto* lang = mu_lang::find(mu_lang::current());
    button(d, L, f, 548, buttons_y, 112, 32, lang ? lang->native : "English", false, focused == PI_LANG, false);
  }
  bool crash_on = false;
  for (int i : items) crash_on = crash_on || i == PI_CRASH;
  if (crash_on) button(d, L, f, 392, buttons_y, 150, 32, "Save crash report", false, focused == PI_CRASH, false);
  text(d, f.body, L.u(11), L.p(CX, buttons_y), L.p(CX + 170, buttons_y + 32), C_FAINT, "A  Select     B  Pages");
}

#include "mu_home_lobby.inl"

// The Replays page's buttons: two rows (Left/Right go through them, Up/Down pick the replay).
enum ReplayButton { RB_WATCH, RB_ASPLAYED, RB_QUEUE, RB_QUEUE_WATCH, RB_DELETE, RB_SEARCH, RB_FOLDER, RB_OPEN, RB_COMPRESS, RB_UNDO, RB_COUNT };
std::string replay_button_label(int i) {
  using namespace features;
  switch (i) {
    case RB_WATCH: return "Watch Replay";
    case RB_ASPLAYED: return std::string("As played: ") + (&library_as_played && library_as_played() ? "On" : "Off");
    case RB_QUEUE: return &library_queue_state && library_queue_state().rfind("Number", 0) == 0 ? "Remove from queue" : "Add to queue";
    case RB_QUEUE_WATCH: return "Watch queue (" + std::to_string(&library_queue_size ? library_queue_size() : 0) + ")";
    case RB_DELETE: return "Delete";
    case RB_SEARCH: return "Search";
    case RB_FOLDER: return "Folder...";
    case RB_OPEN: return "Open replay...";
    case RB_COMPRESS: return "Compress all (.slpz)";
    case RB_UNDO: return "Undo delete";
  }
  return "";
}

void paint_replays(ImDrawList* d, const Layout& L, const Fonts& f, Home& h) {
  text(d, f.bold, L.u(10), L.p(CX, 26), L.p(CX + CW, 44), C_FAINT, "REPLAY VIEWER");
  {   // the folder and the search, at the top right
    std::string top = &features::replay_folder ? features::replay_folder() : std::string();
    if (&features::library_query) { const std::string q = features::library_query(); if (!q.empty()) top = "Search " + q + "   |   " + top; }
    text(d, f.body, L.u(10), L.p(CX + 110, 26), L.p(CX + CW, 44), C_FAINT, top, 2);
  }
  const std::vector<std::string> list = &features::library_list ? features::library_list() : std::vector<std::string>{};
  const auto rows = page_entries("Replays");
  if (list.empty()) {
    text(d, f.body, L.u(12), L.p(CX, 50), L.p(CX + CW, 82), C_DIM,
         &features::library_list ? "No replays yet. Every match you play is saved in switch/melee-nx-netplay/Replays." : "Replays are not part of this build.", 0, true);
  } else {
    h.replay_sel = std::clamp(h.replay_sel, 0, int(list.size()) - 1);
    // The list: one row a game; the picked one carries its line, date and stats (launcher_replays.inl's card).
    const int visible = 5, row_h = 24;
    if (h.replay_sel < h.replay_scroll) h.replay_scroll = h.replay_sel;
    if (h.replay_sel >= h.replay_scroll + visible) h.replay_scroll = h.replay_sel - visible + 1;
    for (int i = 0; i < visible && h.replay_scroll + i < int(list.size()); ++i) {
      const int k = h.replay_scroll + i, y = 48 + i * (row_h + 3);
      const bool sel = k == h.replay_sel;
      round_rect(d, L.p(CX, y), L.p(CX + CW, y + row_h), L.u(7), sel ? C_NAV_ON : C_FIELD, sel ? C_NAV_ON : C_FIELD, sel && !h.rail ? C_ACC_HI : C_FIELD_BORDER);
      std::string name = list[size_t(k)];
      if (sel) if (const auto* e = entry(rows, "Replay")) if (e->value) name = e->value();
      text(d, f.body, L.u(12), L.p(CX + 12, y), L.p(CX + CW - 12, y + row_h), sel ? C_TEXT : C_DIM, name);
    }
  }
  const int sy = 48 + 5 * 27 + 2;
  d->AddRectFilled(L.p(CX, sy), L.p(CX + CW, sy + 1), col(C_SEP));
  // Stats of the picked game, two columns.
  const char* stats[] = {"Recorded", "Players", "Kills", "Damage done", "Openings per kill", "Neutral wins", "L-cancels", "Inputs per minute"};
  for (int i = 0; i < 8 && !list.empty(); ++i) {
    const auto* e = entry(rows, stats[i]);
    if (!e || !e->value) continue;
    const int x = CX + (i % 2) * (CW / 2), y = sy + 5 + (i / 2) * 19;
    text(d, f.bold, L.u(10), L.p(x, y), L.p(x + 110, y + 17), C_FAINT, stats[i]);
    text(d, f.body, L.u(11), L.p(x + 112, y), L.p(x + CW / 2 - 6, y + 17), C_TEXT, e->value());
  }
  // The buttons, two rows of five.
  const int by = sy + 5 + 4 * 19 + 4, bw = (CW - 4 * 6) / 5;
  for (int i = 0; i < RB_COUNT; ++i) {
    const int x = CX + (i % 5) * (bw + 6), y = by + (i / 5) * 30;
    std::string label = replay_button_label(i);
    if (i == RB_DELETE) if (const auto* e = entry(rows, "Delete this replay")) if (e->value) { const std::string v = e->value(); if (v.rfind("Press A", 0) == 0) label = "Press A again"; }
    const bool disabled = (list.empty() && (i == RB_WATCH || i == RB_QUEUE || i == RB_DELETE)) ||
                          (i == RB_UNDO && (!&features::library_trash_size || features::library_trash_size() == 0)) ||
                          (i == RB_QUEUE_WATCH && (!&features::library_queue_size || features::library_queue_size() == 0));
    button(d, L, f, x, y, bw, 26, label.c_str(), i == RB_WATCH, !h.rail && h.focus == i, disabled);
  }
  // The network and timing graph of the picked game's session trace (launcher_trace_view.cpp's overview), and the page's note.
  const int gy = by + 64;
  std::vector<std::array<float, 5>> bins; std::string summary;
  if (!list.empty() && &features::library_trace && features::library_trace(&bins, &summary) && !bins.empty()) {
    const float gx0 = CX, gx1 = CX + CW, gh = 30;
    round_rect(d, L.p(gx0, gy), L.p(gx1, gy + gh), L.u(5), C_FIELD, C_FIELD, C_FIELD_BORDER);
    float top = 33.4f;
    for (const auto& b : bins) top = std::max(top, std::max(b[0], b[1]));
    const float bw2 = (gx1 - gx0 - 4) / float(bins.size());
    for (size_t i = 0; i < bins.size(); ++i) {
      const auto& b = bins[i];
      const float x = gx0 + 2 + bw2 * float(i);
      const float hi = std::min(gh - 4, (gh - 4) * b[0] / top), wk = std::min(gh - 4, (gh - 4) * b[1] / top);
      d->AddRectFilled(L.p(x, gy + gh - 2 - hi), L.p(x + std::max(bw2, 0.6f), gy + gh - 2), col(b[2] > 0 ? C_WARN : C_ACC_LO, 0.9f));
      d->AddRectFilled(L.p(x, gy + gh - 2 - wk), L.p(x + std::max(bw2, 0.6f), gy + gh - 2), col(C_OK, 0.8f));
      if (b[3] > 0) d->AddRectFilled(L.p(x, gy + 2), L.p(x + std::max(bw2, 0.6f), gy + 5), col(C_BAD));
      if (int(b[4]) & 0x78) d->AddRectFilled(L.p(x, gy), L.p(x + std::max(bw2, 0.8f), gy + 2), IM_COL32_WHITE);
    }
    d->AddLine(L.p(gx0 + 2, gy + gh - 2 - (gh - 4) * 16.7f / top), L.p(gx1 - 2, gy + gh - 2 - (gh - 4) * 16.7f / top), IM_COL32(255, 255, 255, 60));
    text(d, f.body, L.u(10), L.p(gx0, gy + gh + 1), L.p(gx1, gy + gh + 15), C_FAINT, "Session trace: " + summary + "   (green work, purple frame time, amber waits, red rollbacks)");
  } else {
    std::string note = &features::library_note ? features::library_note() : std::string();
    if (&features::library_compress_state) { const std::string c = features::library_compress_state(); if (!c.empty()) note = c; }
    text(d, f.body, L.u(10), L.p(CX, gy), L.p(CX + CW, gy + 16), C_FAINT, note.empty() ? "No session trace beside this replay (online matches with a mark keep one)." : note);
  }
}

// The SD card browser: folders first, then (opening a file) the replays; A enters or opens, X uses this folder, B closes.
void browse_read(Home& h) {
  h.browse_list.clear();
  if (h.browse_dir.size() > 1 && h.browse_dir != "sdmc:/") h.browse_list.push_back("..");
  std::vector<std::string> dirs, files;
  if (DIR* dir = opendir(h.browse_dir.c_str())) {
    while (dirent* e = readdir(dir)) {
      const std::string n = e->d_name;
      if (n.empty() || n[0] == '.') continue;
      struct stat st{};
      const std::string full = h.browse_dir + (h.browse_dir.back() == '/' ? "" : "/") + n;
      if (stat(full.c_str(), &st) != 0) continue;
      if (S_ISDIR(st.st_mode)) dirs.push_back(n + "/");
      else if (h.browse_files && ((n.size() > 4 && n.compare(n.size() - 4, 4, ".slp") == 0) || (n.size() > 5 && n.compare(n.size() - 5, 5, ".slpz") == 0))) files.push_back(n);
    }
    closedir(dir);
  }
  std::sort(dirs.begin(), dirs.end()); std::sort(files.begin(), files.end());
  for (auto& x : dirs) h.browse_list.push_back(x);
  for (auto& x : files) h.browse_list.push_back(x);
  h.browse_sel = 0; h.browse_scroll = 0;
}
void paint_browser(ImDrawList* d, const Layout& L, const Fonts& f, Home& h) {
  d->AddRectFilled(ImVec2(0, 0), ImVec2(L.w, L.h), IM_COL32(0, 0, 0, 150));
  const float x = CX, y = 40, w = CW, hh = 400;
  round_rect(d, L.p(x, y), L.p(x + w, y + hh), L.u(10), RGB(0x14, 0x1C, 0x2C), RGB(0x14, 0x1C, 0x2C), C_ACC_LO);
  text(d, f.bold, L.u(14), L.p(x + 16, y + 8), L.p(x + w - 16, y + 30), C_TEXT, h.browse_files ? "Choose a Slippi replay" : "Choose the replay folder");
  text(d, f.body, L.u(11), L.p(x + 16, y + 30), L.p(x + w - 16, y + 46), C_DIM, h.browse_dir);
  const int visible = 13, row = 24;
  if (h.browse_sel < h.browse_scroll) h.browse_scroll = h.browse_sel;
  if (h.browse_sel >= h.browse_scroll + visible) h.browse_scroll = h.browse_sel - visible + 1;
  for (int i = 0; i < visible && h.browse_scroll + i < int(h.browse_list.size()); ++i) {
    const int k = h.browse_scroll + i;
    const float ry = y + 52 + i * row;
    if (k == h.browse_sel) round_rect(d, L.p(x + 10, ry), L.p(x + w - 10, ry + row - 2), L.u(5), C_NAV_ON, C_NAV_ON, C_ACC_HI);
    text(d, f.body, L.u(12), L.p(x + 20, ry), L.p(x + w - 20, ry + row - 2), C_TEXT, h.browse_list[size_t(k)]);
  }
  if (h.browse_list.empty()) text(d, f.body, L.u(12), L.p(x + 20, y + 52), L.p(x + w - 20, y + 74), C_DIM, h.browse_files ? "No replays or folders here." : "No folders here.");
  text(d, f.body, L.u(10), L.p(x + 16, y + hh - 22), L.p(x + w - 16, y + hh - 6), C_FAINT,
       h.browse_files ? "A  Open     B  Close" : "A  Open the folder     X  Use this folder     B  Close");
}

// The Mods page, as melee-unlocked's launcher draws its mod cards (launcher_mods.inl paint_page): a check, the name and the
// latest version, the credits, the state (green once installed), the last line (what is happening, or the size and
// licence) with a progress bar; the picked card carries its buttons (Get / Update / Cancel / Play it at the next start,
// Remove). Under the cards: the profile the next start loads.
// Each mod disc's costume skins, as the last game start counted them (features/cosmetics.cpp: mods-cache/disc-skins.txt).
size_t disc_skins(const std::string& profile) {
  std::ifstream in(options.out_dir + "/mods-cache/disc-skins.txt");
  std::string line;
  while (std::getline(in, line)) {
    const size_t eq = line.find('=');
    if (eq != std::string::npos && line.substr(0, eq) == profile) return size_t(std::strtoul(line.c_str() + eq + 1, nullptr, 10));
  }
  return 0;
}
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
    const size_t skins = disc_skins(m.id);
    if (skins) {   // upstream's card line once the game has listed the disc's costumes
      const std::string line = std::to_string(skins) + (skins == 1 ? " skin" : " skins");
      text(d, f.body, L.u(11), L.p(tx, y + 8), L.p(tx + tw, y + 28), C_OK, line, 2);
    }
    if (sel) {   // the picked card's buttons, at its foot
      button(d, L, f, CX + CW - 16 - 120 - (c.can_remove ? 86 : 0), y + card_h - 22, 120, 18, c.primary.c_str(), true, h.mod_button == 0, !c.primary_enabled);
      if (c.can_remove) button(d, L, f, CX + CW - 16 - 80, y + card_h - 22, 80, 18, c.remove_armed ? "Press A again" : "Remove", false, h.mod_button == 1, false);
      // "Choose skins": the settings panel on its Choose skins rows (upstream opens Settings on the Mods tab).
      button(d, L, f, CX + 44, y + card_h - 22, 112, 18, "Choose skins", false, h.mod_button == 2, skins == 0);
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

// The Language button's menu, opened above it: each language named in itself, the current one checked.
void paint_lang(ImDrawList* d, const Layout& L, const Fonts& f, const Home& h) {
  const auto& list = mu_lang::languages();
  const float x = 548, w = 150, row = 24, hh = row * float(list.size()) + 8, y = 380 - hh - 6;
  round_rect(d, L.p(x, y), L.p(x + w, y + hh), L.u(8), RGB(0x14, 0x1C, 0x2C), RGB(0x14, 0x1C, 0x2C), C_ACC_LO);
  for (size_t i = 0; i < list.size(); ++i) {
    const float ry = y + 4 + row * float(i);
    if (int(i) == h.lang_sel) round_rect(d, L.p(x + 4, ry), L.p(x + w - 4, ry + row), L.u(5), C_NAV_ON, C_NAV_ON, C_ACC_HI);
    if (mu_lang::current() == list[i].code) dot(d, L, x + 10, ry + 8, C_OK);
    text(d, f.body, L.u(12), L.p(x + 26, ry), L.p(x + w - 8, ry + row), C_TEXT, list[i].native);
  }
}

// The launcher colour's wheel (launcher_theme_picker.inl's popover, driven by the D-pad): the wheel with the picked point, a
// brightness bar, the three values and the hex code.
void paint_theme(ImDrawList* d, const Layout& L, const Fonts& f, const Home& h) {
  const float x = 430, y = 92, w = 268, hh = 248;
  round_rect(d, L.p(x, y), L.p(x + w, y + hh), L.u(10), RGB(0x14, 0x1C, 0x2C), RGB(0x14, 0x1C, 0x2C), C_ACC_LO);
  text(d, f.bold, L.u(13), L.p(x + 14, y + 8), L.p(x + w - 14, y + 28), C_TEXT, "Launcher color");
  const ImVec2 c = L.p(x + 70, y + 100);
  const float r = L.u(52);
  for (int ring = 6; ring >= 1; --ring)   // saturation rings, hue around
    for (int k = 0; k < 48; ++k) {
      const float a0 = -k * 3.14159265f / 24.0f, a1 = a0 - 3.14159265f / 24.0f;
      d->PathLineTo(c); d->PathArcTo(c, r * ring / 6.0f, a0, a1, 3);
      d->PathFillConvex(col(launcher::theme::hsv(k * 7.5, ring / 6.0, h.theme_v)));
    }
  const float a = float(h.theme_h * 3.14159265 / 180.0);
  const ImVec2 p(c.x + std::cos(a) * r * float(h.theme_s), c.y - std::sin(a) * r * float(h.theme_s));
  d->AddCircle(p, L.u(5), IM_COL32_WHITE, 16, 2.0f);
  d->AddCircle(p, L.u(6), IM_COL32(0, 0, 0, 200), 16, 1.0f);
  const COLORREF now = launcher::theme::hsv(h.theme_h, h.theme_s, h.theme_v);
  round_rect(d, L.p(x + 140, y + 48), L.p(x + w - 14, y + 80), L.u(6), now, now, C_BTN_BORDER);
  char hex[16]; std::snprintf(hex, sizeof hex, "#%02X%02X%02X", GetRValue(now), GetGValue(now), GetBValue(now));
  text(d, f.body, L.u(12), L.p(x + 140, y + 84), L.p(x + w - 14, y + 102), C_DIM, hex, 1);
  const char* names[3] = {"Hue", "Saturation", "Brightness"};
  char values[3][16];
  std::snprintf(values[0], 16, "%.0f", h.theme_h); std::snprintf(values[1], 16, "%.0f%%", h.theme_s * 100); std::snprintf(values[2], 16, "%.0f%%", h.theme_v * 100);
  for (int i = 0; i < 3; ++i) {
    const float ry = y + 162 + i * 24;
    if (i == h.theme_row) round_rect(d, L.p(x + 10, ry), L.p(x + w - 10, ry + 22), L.u(5), C_NAV_ON, C_NAV_ON, C_ACC_HI);
    text(d, f.body, L.u(12), L.p(x + 18, ry), L.p(x + 140, ry + 22), C_TEXT, names[i]);
    text(d, f.body, L.u(12), L.p(x + 140, ry), L.p(x + w - 18, ry + 22), C_TEXT, std::string("<  ") + values[i] + "  >", 2);
  }
  // brightness bar
  for (int k = 0; k < 32; ++k)
    d->AddRectFilled(L.p(x + 140 + k * 3.6f, y + 112), L.p(x + 140 + (k + 1) * 3.6f + 0.5f, y + 124), col(launcher::theme::hsv(h.theme_h, h.theme_s, k / 31.0)));
  const float vx = x + 140 + float(h.theme_v) * 114;
  d->AddRect(L.p(vx - 2, y + 110), L.p(vx + 2, y + 126), IM_COL32_WHITE);
  text(d, f.body, L.u(10), L.p(x + 14, y + hh - 18), L.p(x + w - 14, y + hh - 4), C_FAINT, "A  Apply     B  Cancel");
}

void paint_message(ImDrawList* d, const Layout& L, const Fonts& f, const Home& h) {
  d->AddRectFilled(ImVec2(0, 0), ImVec2(L.w, L.h), IM_COL32(0, 0, 0, 150));
  // Long lines wrap: each takes the rows it needs.
  const float x = CX, w = CW, wrap = L.u(w - 36);
  std::vector<float> rows;
  float body = 0;
  for (const auto& line : h.msg_lines) {
    const std::string t = mu_lang::tx(line);
    const float th = f.body->CalcTextSizeA(L.u(12), FLT_MAX, wrap, t.c_str()).y / L.s;
    rows.push_back(std::max(18.0f, th + 4)); body += rows.back() + 4;
  }
  const float hh = 40 + body + 60, y = std::max(20.0f, 240 - hh * 0.5f);
  round_rect(d, L.p(x, y), L.p(x + w, y + hh), L.u(10), RGB(0x14, 0x1C, 0x2C), RGB(0x14, 0x1C, 0x2C), C_ACC_LO);
  text(d, f.bold, L.u(16), L.p(x + 18, y + 10), L.p(x + w - 18, y + 36), C_TEXT, h.msg_title);
  float ly = y + 44;
  for (size_t i = 0; i < h.msg_lines.size(); ++i) {
    text(d, f.body, L.u(12), L.p(x + 18, ly), L.p(x + w - 18, ly + rows[i]), C_DIM, h.msg_lines[i], 0, rows[i] > 22);
    ly += rows[i] + 4;
  }
  if (h.msg_progress >= 0.0f) {
    // A long job (the RVZ conversion): its bar and its count where the button would be; no button, nothing to press.
    const float bx = x + 18, bw = w - 36, by = y + hh - 40, bh = 12;
    round_rect(d, L.p(bx, by), L.p(bx + bw, by + bh), L.u(6), RGB(0x0B, 0x11, 0x1E), RGB(0x0B, 0x11, 0x1E), C_ACC_LO);
    const float fill = std::clamp(h.msg_progress, 0.0f, 1.0f) * bw;
    if (fill > 1.0f) round_rect(d, L.p(bx, by), L.p(bx + fill, by + bh), L.u(6), C_ACC_LO, C_ACC_LO, C_ACC_LO);
    text(d, f.body, L.u(12), L.p(bx, by - 22), L.p(bx + bw, by - 2), C_TEXT, h.msg_progress_text);
    return;
  }
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
        case PI_THEME:
          h.theme_before = current_accent();
          launcher::theme::to_hsv(h.theme_before, h.theme_h, h.theme_s, h.theme_v);
          h.theme_open = true; h.theme_row = 0;
          say("home: Launcher color");
          break;
        case PI_LANG: {
          h.lang_open = true; h.lang_sel = 0;
          const auto& list = mu_lang::languages();
          for (size_t i = 0; i < list.size(); ++i) if (mu_lang::current() == list[i].code) h.lang_sel = int(i);
          say("home: Language");
          break;
        }
        case PI_NOTES: open_notes(h); say("home: What's new"); break;
        case PI_CRASH:
          if (&mu_ui::crash_report_save) {
            std::string where, error;
            const bool ok = mu_ui::crash_report_save(&where, &error);
            h.msg_title = ok ? "Crash report saved" : "No crash report";
            h.msg_lines = ok ? std::vector<std::string>{"The report is on the SD card:", where,
                                                        "Codes, addresses and names in it were replaced before it was written. Nothing was sent anywhere."}
                             : std::vector<std::string>{error};
            h.msg_quits = false;
          }
          break;
      }
      break;
    case PAGE_REPLAYS: {
      using namespace features;
      const auto rows = page_entries("Replays");
      const std::vector<std::string> list = &library_list ? library_list() : std::vector<std::string>{};
      switch (item) {
        case RB_WATCH:
          if (!list.empty()) {
            // The library's own Watch asks play mode to start again with the replay; before the game, the first start is it.
            const std::string name = list[size_t(std::clamp(h.replay_sel, 0, int(list.size()) - 1))];
            home_replay = (&replay_folder ? replay_folder() + "/" : std::string()) + name;
            say("home: watch %s", home_replay.c_str());
            h.done = true; h.result = AppStart::PlayOffline;
          }
          break;
        case RB_ASPLAYED: if (&library_set_as_played) library_set_as_played(!library_as_played()); break;
        case RB_QUEUE: if (&library_queue_toggle && !list.empty()) library_queue_toggle(); break;
        case RB_QUEUE_WATCH:
          // The queue's first replay starts the game; the rest follow one after another (replays.cpp, at each replay's end).
          if (&library_queue_size && library_queue_size() > 0) {
            home_replay = &library_queue_start ? library_queue_start() : std::string();
            if (!home_replay.empty()) { say("home: watch the queue from %s", home_replay.c_str()); h.done = true; h.result = AppStart::PlayOffline; }
          }
          break;
        case RB_DELETE: if (const auto* e = entry(rows, "Delete this replay")) if (e->press) e->press(); break;
        case RB_SEARCH: if (&library_search) library_search(); say("home: Search replays"); break;
        case RB_FOLDER: case RB_OPEN:
          h.browse_open = true; h.browse_files = item == RB_OPEN;
          h.browse_dir = &replay_folder ? replay_folder() : options.base_dir;
          browse_read(h);
          say("home: %s", item == RB_OPEN ? "Open replay" : "Replay folder");
          break;
        case RB_COMPRESS: if (&library_compress) library_compress(); break;
        case RB_UNDO: if (&library_undo_delete) library_undo_delete(); break;
      }
      break;
    }
    case PAGE_MODS: {
      const std::vector<mu_mods::Card> cards = mu_mods::cards();
      if (h.mod_sel < int(cards.size())) {
        const auto& c = cards[size_t(h.mod_sel)];
        if (h.mod_button == 1 && c.can_remove) mu_mods::remove(c.status.mod.id);
        else if (h.mod_button == 2) { if (disc_skins(c.status.mod.id)) { mu_ui::panel_open_on("Skins"); say("home: Choose skins (%s)", c.status.mod.id.c_str()); } }
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
    } else if (h.browse_open) {
      const int n = int(h.browse_list.size());
      if (e.up && n) h.browse_sel = (h.browse_sel + n - 1) % n;
      if (e.down && n) h.browse_sel = (h.browse_sel + 1) % n;
      if (e.b) h.browse_open = false;
      if (e.x && !h.browse_files) {
        // The replay folder (settings.ini replay_dir, replays.cpp): recordings go there and the list reads it.
        settings_change("replay_dir", h.browse_dir);
        say("home: replay folder %s", h.browse_dir.c_str());
        h.browse_open = false;
        for (const auto& en : page_entries("Replays")) if (en.label && std::strcmp(en.label, "Sort") == 0 && en.change) { en.change(1); en.change(-1); }
      }
      if (e.a && n) {
        const std::string pick = h.browse_list[size_t(h.browse_sel)];
        if (pick == "..") {
          std::string d2 = h.browse_dir;
          while (d2.size() > 1 && d2.back() == '/') d2.pop_back();
          const size_t slash = d2.find_last_of('/');
          h.browse_dir = slash == std::string::npos ? d2 : d2.substr(0, std::max<size_t>(slash, d2.find(':') != std::string::npos ? d2.find(':') + 2 : 1));
          browse_read(h);
        } else if (pick.back() == '/') {
          h.browse_dir = h.browse_dir + (h.browse_dir.back() == '/' ? "" : "/") + pick.substr(0, pick.size() - 1);
          browse_read(h);
        } else {
          // Any replay file: watched as the Replays page's own (upstream's "Open replay").
          home_replay = h.browse_dir + (h.browse_dir.back() == '/' ? "" : "/") + pick;
          say("home: open replay %s", home_replay.c_str());
          h.browse_open = false; h.done = true; h.result = AppStart::PlayOffline;
        }
      }
    } else if (h.lang_open) {
      const int n = int(mu_lang::languages().size());
      if (e.up) h.lang_sel = (h.lang_sel + n - 1) % n;
      if (e.down) h.lang_sel = (h.lang_sel + 1) % n;
      if (e.b) h.lang_open = false;
      if (e.a) {
        const std::string code = mu_lang::languages()[size_t(h.lang_sel)].code;
        mu_lang::set(code);
        app_ini_set("language", code);
        say("home: language %s", code.c_str());
        h.lang_open = false;
      }
    } else if (h.theme_open) {
      if (e.up) h.theme_row = (h.theme_row + 2) % 3;
      if (e.down) h.theme_row = (h.theme_row + 1) % 3;
      const int dir = e.left ? -1 : e.right ? 1 : 0;
      if (dir) {
        if (h.theme_row == 0) h.theme_h = std::fmod(h.theme_h + dir * 10.0 + 360.0, 360.0);
        if (h.theme_row == 1) h.theme_s = std::clamp(h.theme_s + dir * 0.05, 0.0, 1.0);
        if (h.theme_row == 2) h.theme_v = std::clamp(h.theme_v + dir * 0.05, 0.0, 1.0);
      }
      if (e.b) { h.theme_open = false; }
      if (e.a) {
        const COLORREF c = launcher::theme::hsv(h.theme_h, h.theme_s, h.theme_v);
        char hex[16]; std::snprintf(hex, sizeof hex, "#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
        app_ini_set("accent", hex);
        say("home: launcher color %s", hex);
        h.theme_open = false;
      }
    } else {
      // Rail: Up/Down pick a page (it shows at once), Right or A enters it. Page: Up/Down move, A acts, B or Left on the
      // first column returns to the rail.
      if (h.rail) {
        if (e.up) h.focus = (h.focus + PAGE_COUNT - 1) % PAGE_COUNT;
        if (e.down) h.focus = (h.focus + 1) % PAGE_COUNT;
        if (e.up || e.down) { if (h.focus != PAGE_SETTINGS) h.page = h.focus; say("home: rail %s", kNav[h.focus]); }
        if (e.a || e.right) {
          if (h.focus == PAGE_SETTINGS) { mu_ui::panel_open_from_launcher(); say("home: Settings"); }
          else { h.page = h.focus; h.rail = false; h.focus = h.page == PAGE_PLAY ? 1 : 0; }
        }
      } else if (h.page == PAGE_LOBBY) {
        const bool took_left = lobby_input(h, e, true);
        if ((e.left && !took_left) || e.b) { h.rail = true; h.focus = h.page; }
      } else {
        int count = 1;
        if (h.page == PAGE_PLAY) count = int(play_items(h).size());
        else if (h.page == PAGE_REPLAYS) count = RB_COUNT;
        else if (h.page == PAGE_MODS) count = int(mu_mods::cards().size()) + 1;
        if (h.page == PAGE_REPLAYS) {
          if (e.up) replay_step(h, -1);
          if (e.down) replay_step(h, 1);
          if (e.right) h.focus = (h.focus + 1) % RB_COUNT;
          if (e.left) { if (h.focus == 0) { h.rail = true; h.focus = h.page; } else h.focus = h.focus - 1; }
        }
        else if (h.page == PAGE_MODS) {
          if (e.up) { h.mod_sel = std::max(0, h.mod_sel - 1); h.mod_button = 0; }
          if (e.down) { h.mod_sel = std::min(count - 1, h.mod_sel + 1); h.mod_button = 0; }
          // Left/Right: Choose skins, the main button, Remove (left of the main button: Choose skins; past it: the rail).
          if (h.mod_sel == count - 1 && (e.left || e.right)) profile_step(e.left ? -1 : 1);
          else if (e.right) h.mod_button = h.mod_button == 2 ? 0 : 1;
          else if (e.left && h.mod_button == 1) h.mod_button = 0;
          else if (e.left && h.mod_button == 0) h.mod_button = 2;
          else if (e.left) { h.rail = true; h.focus = h.page; h.mod_button = 0; }
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
    // A new request rings and private chat requests come to the front whatever page shows (upstream's process_invites).
    if (!(h.page == PAGE_LOBBY && !h.rail)) lobby_input(h, Edges{}, false);
    // An accepted lobby match: the home screen closes and the game starts it (lobby.cpp hands it over).
    {
      lobby::Match m;
      if (!h.done && lobby::take_match(&m)) {
        lobby::hand_to_game(m);
        say("home: lobby match against %s (%s): the game starts", m.name.c_str(), m.code.c_str());
        h.done = true; h.result = AppStart::Play;
      }
    }
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
      launcher::theme::accent = h.theme_open ? launcher::theme::hsv(h.theme_h, h.theme_s, h.theme_v) : current_accent();
      Fonts f{mu_ui::body_font() ? mu_ui::body_font() : ImGui::GetFont(), mu_ui::body_font() ? mu_ui::body_font() : ImGui::GetFont()};
      ImDrawList* d = ImGui::GetBackgroundDrawList();
      vgrad(d, ImVec2(0, 0), ImVec2(L.w, L.h), C_CONTENT_TOP, C_CONTENT_BOT);
      paint_rail(d, L, f, h);
      if (h.page == PAGE_PLAY) paint_play(d, L, f, h);
      else if (h.page == PAGE_REPLAYS) paint_replays(d, L, f, h);
      else if (h.page == PAGE_MODS) paint_mods(d, L, f, h);
      else if (h.page == PAGE_LOBBY) paint_lobby(d, L, f, h);
      if (h.browse_open) paint_browser(ImGui::GetForegroundDrawList(), L, f, h);
      if (h.lang_open) paint_lang(ImGui::GetForegroundDrawList(), L, f, h);
      if (h.theme_open) paint_theme(ImGui::GetForegroundDrawList(), L, f, h);
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

namespace {
// A message with a progress bar while `job` runs on its own thread (the RVZ conversion): drawn until it returns; nothing to press.
struct ProgressScreen {
  std::string headline; std::vector<std::string> lines;
  std::atomic<double> fraction{0.0}; std::atomic<bool> done{false};
  std::mutex mutex; std::string text;
};
void progress_job(void* frontend_v, void* user) {
  auto& p = *static_cast<ProgressScreen*>(user);
  auto& frontend = *static_cast<host::MetalFrontend*>(frontend_v);
  Home h; h.msg_title = p.headline; h.msg_lines = p.lines;
  while (!p.done.load()) {
    if (!frontend.pump_events()) continue;
    const host::MetalWindowSize size = frontend.window_size();
    if (!aurora_begin_frame()) continue;
    h.msg_progress = float(p.fraction.load());
    { std::lock_guard<std::mutex> lock(p.mutex); h.msg_progress_text = p.text; }
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

// Runs `job` on a thread of its own and shows `headline` and `lines` with a bar of job's progress (report(fraction, text))
// until it returns; false when the screens are not running (the job then runs here, without a picture).
bool mu_home_progress(const std::string& headline, const std::vector<std::string>& lines,
                      const std::function<void(const std::function<void(double, const std::string&)>&)>& job) {
  ProgressScreen p;
  p.headline = headline; p.lines = lines;
  const auto report = [&p](double fraction, const std::string& text) {
    p.fraction.store(fraction);
    std::lock_guard<std::mutex> lock(p.mutex); p.text = text;
  };
  if (!screens_running()) { job(report); return false; }
  std::thread worker([&] { job(report); p.done.store(true); });
  screens_run(progress_job, &p);
  worker.join();
  return true;
}

bool mu_home_message(const std::string& headline, const std::vector<std::string>& lines) {
  Message m{headline, lines};
  if (!screens_run(message_job, &m)) return true;
  return m.ok;
}

// The home screen: until PLAY (or Watch, or the app is closed). `update_check` starts the build computer's check in the
// background: its answer shows on the Play page's version line, and "Update and restart" installs it there.
AppStart mu_home_run(const std::string& user_dir, bool update_check) {
  diagnostics_start();
  Home h;
  h.user_dir = user_dir;
  h.account = read_account(user_dir, &h.signed_in);
  h.focus = 1;   // PLAY
  {
    const std::string code = app_ini("language", "");
    if (!code.empty() && !mu_lang::set(code)) say("home: language %s is not one of the launcher's; English", code.c_str());
    else if (!code.empty()) say("home: language %s", code.c_str());
  }
  // The first start of a new version opens What's new by itself (upstream: notesseen).
  // A first install has nothing to compare with: the version is noted quietly, and the next new version opens its notes.
  {
    const std::string seen = app_ini("notesseen", "");
    if (seen != MU_APP_VERSION) {
      app_ini_set("notesseen", MU_APP_VERSION);
      if (!seen.empty()) { open_notes(h); say("home: What's new opened for %s (last seen %s)", MU_APP_VERSION, seen.c_str()); }
    }
  }
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
  // The Multiplayer Lobby's thread (idle until Go Online): the Lobby folder, the account's name and code, ready to play.
  account_parts(user_dir, &h.lobby_name, &h.lobby_code);
  {
    struct stat st{};
    lobby::start(options.base_dir + "/Lobby", h.lobby_name, h.lobby_code, h.signed_in && stat(options.iso.c_str(), &st) == 0);
  }
  // Back from a lobby match (play_main.cpp's visits): on the lobby page, as upstream's launcher shows again after its game.
  if (lobby::returning()) { h.page = PAGE_LOBBY; h.rail = false; h.focus = 0; say("home: back on the lobby page after the lobby match"); }
  say("home: melee-unlocked's launcher (Play, Settings, Multiplayer Lobby, Replay Viewer, Mods)%s", h.signed_in ? "" : "; no Slippi account on this console");
  if (!screens_run(home_job, &h)) return h.signed_in ? AppStart::Play : AppStart::PlayOffline;
  return h.result;
}
}  // namespace fe
