// SPDX-License-Identifier: GPL-2.0-or-later
#include "switch_signin.h"

#include "host.h"
#include "metal_frontend.h"
#include "slippi_login.h"

#include <aurora/aurora.h>
#include <dolphin/pad.h>
#include <imgui.h>
#include <switch.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace host {
namespace {
// Melee's own grammar: dark blue field, one yellow header, white type. One accent, spent once.
constexpr ImVec4 kField{0.043f, 0.075f, 0.196f, 1.0f};        // the character-select blue
constexpr ImVec4 kPanel{0.071f, 0.118f, 0.275f, 1.0f};
constexpr ImVec4 kHairline{1.0f, 1.0f, 1.0f, 0.10f};
constexpr ImVec4 kYellow{0.98f, 0.80f, 0.16f, 1.0f};
constexpr ImVec4 kText{1.0f, 1.0f, 1.0f, 0.96f};
constexpr ImVec4 kMuted{1.0f, 1.0f, 1.0f, 0.55f};
constexpr ImVec4 kError{1.0f, 0.42f, 0.36f, 1.0f};
constexpr ImVec4 kOk{0.45f, 0.86f, 0.55f, 1.0f};

ImU32 u32(ImVec4 c, float alpha = 1.0f) { c.w *= alpha; return ImGui::ColorConvertFloat4ToU32(c); }

enum Row { ROW_EMAIL, ROW_PASSWORD, ROW_SIGN_IN, ROW_OFFLINE, ROW_COUNT };

struct State {
  std::string email, password;
  int row = ROW_EMAIL;
  std::atomic<bool> busy{false};
  std::mutex lock;
  std::string status;            // under the form: progress, error or success
  bool status_is_error = false;
  bool done = false;             // signed in
  slippi::login::Account account;
  std::thread worker;
};

bool keyboard(const char* header, const char* guide, bool password, std::string& value) {
  SwkbdConfig config;
  if (R_FAILED(swkbdCreate(&config, 0))) return false;
  if (password) swkbdConfigMakePresetPassword(&config);
  else swkbdConfigMakePresetDefault(&config);
  swkbdConfigSetHeaderText(&config, header);
  swkbdConfigSetGuideText(&config, guide);
  swkbdConfigSetStringLenMax(&config, 128);
  if (!password) swkbdConfigSetInitialText(&config, value.c_str());
  char out[256] = {};
  const Result rc = swkbdShow(&config, out, sizeof out);
  swkbdClose(&config);
  if (R_FAILED(rc)) return false;   // cancelled
  value = out;
  return true;
}

void start_sign_in(State& st, const std::string& user_dir) {
  if (st.busy.exchange(true)) return;
  if (st.worker.joinable()) st.worker.join();
  { std::lock_guard<std::mutex> g(st.lock); st.status = "Signing in..."; st.status_is_error = false; }
  st.worker = std::thread([&st, user_dir] {
    slippi::login::Account account;
    slippi::login::Session session;
    std::string error;
    bool ok = slippi::login::sign_in_session(st.email, st.password, account, session, error);
    if (ok) ok = slippi::login::write_user_file(user_dir, account, error);
    if (ok) slippi::login::write_session(user_dir, session);   // keeps the console signed in
    std::lock_guard<std::mutex> g(st.lock);
    if (ok) {
      st.account = account; st.done = true; st.status_is_error = false;
      st.status = "Signed in as " + account.display_name + "  " + account.connect_code;
      host::log("slippi: signed in as %s (%s)", account.display_name.c_str(), account.connect_code.c_str());
    } else {
      st.status = error.empty() ? "Sign-in failed" : error; st.status_is_error = true;
      host::log("slippi: sign-in failed: %s", st.status.c_str());
    }
    st.busy = false;
  });
}

// Pad edges: Aurora PAD gives GameCube-layout buttons for every connected controller.
struct Edges { bool up = false, down = false, a = false, b = false, plus = false; };
Edges read_edges(uint16_t& last_buttons, int8_t& last_y) {
  PADStatus status[4];
  std::memset(status, 0, sizeof status);
  PADRead(status);
  uint16_t buttons = 0; int8_t y = 0;
  for (int port = 0; port < 4; ++port) {
    if (status[port].err != 0) continue;
    buttons |= status[port].button;
    if (std::abs(status[port].stickY) > std::abs(y)) y = status[port].stickY;
  }
  Edges e;
  const uint16_t pressed = buttons & ~last_buttons;
  const bool stick_up = y > 50 && last_y <= 50, stick_down = y < -50 && last_y >= -50;
  e.up = (pressed & PAD_BUTTON_UP) || stick_up;
  e.down = (pressed & PAD_BUTTON_DOWN) || stick_down;
  e.a = pressed & PAD_BUTTON_A;
  e.b = pressed & PAD_BUTTON_B;
  e.plus = pressed & PAD_BUTTON_START;
  last_buttons = buttons; last_y = y;
  return e;
}

void draw(State& st, int w, int h) {
  ImDrawList* d = ImGui::GetBackgroundDrawList();
  ImFont* font = ImGui::GetFont();
  const float s = h / 720.0f;   // points at 720p; the same layout docked at 1080p
  d->AddRectFilled(ImVec2(0, 0), ImVec2((float)w, (float)h), u32(kField));
  // Dark blue grid, as behind Melee's menus.
  for (float x = 0; x < w; x += 64 * s) d->AddLine(ImVec2(x, 0), ImVec2(x, (float)h), u32(kHairline, 0.5f));
  for (float y = 0; y < h; y += 64 * s) d->AddLine(ImVec2(0, y), ImVec2((float)w, y), u32(kHairline, 0.5f));

  const float px = 160 * s, py = 120 * s, pw = w - 320 * s;
  // Angled yellow section header, Melee's own grammar.
  const float hh = 44 * s, slant = 22 * s;
  d->AddQuadFilled(ImVec2(px, py), ImVec2(px + 420 * s, py), ImVec2(px + 420 * s - slant, py + hh), ImVec2(px, py + hh), u32(kYellow));
  d->AddText(font, 26 * s, ImVec2(px + 20 * s, py + 9 * s), u32(ImVec4(0.08f, 0.08f, 0.1f, 1)), "SLIPPI ONLINE");
  d->AddText(font, 18 * s, ImVec2(px + 440 * s, py + 13 * s), u32(kMuted), "Sign in with your Slippi account to play online.");

  const float top = py + hh + 32 * s, rowh = 64 * s;
  const char* labels[ROW_COUNT] = {"Email", "Password", "Sign in", "Play offline"};
  for (int i = 0; i < ROW_COUNT; ++i) {
    const float y0 = top + i * (rowh + 12 * s), y1 = y0 + rowh;
    const bool selected = st.row == i;
    d->AddRectFilled(ImVec2(px, y0), ImVec2(px + pw, y1), u32(kPanel), 10 * s);
    if (selected) d->AddRect(ImVec2(px, y0), ImVec2(px + pw, y1), u32(kYellow), 10 * s, 0, 2.0f * s);
    else d->AddRect(ImVec2(px, y0), ImVec2(px + pw, y1), u32(kHairline), 10 * s, 0, 1.0f);
    const bool action = i >= ROW_SIGN_IN;
    d->AddText(font, 22 * s, ImVec2(px + 24 * s, y0 + 20 * s), u32(action ? kText : kMuted), labels[i]);
    if (i == ROW_EMAIL) {
      const std::string value = st.email.empty() ? "Not set" : st.email;
      d->AddText(font, 22 * s, ImVec2(px + 260 * s, y0 + 20 * s), u32(st.email.empty() ? kMuted : kText), value.c_str());
    } else if (i == ROW_PASSWORD) {
      const std::string value = st.password.empty() ? "Not set" : std::string(std::min<size_t>(st.password.size(), 24), '*');
      d->AddText(font, 22 * s, ImVec2(px + 260 * s, y0 + 20 * s), u32(st.password.empty() ? kMuted : kText), value.c_str());
    }
  }
  std::string status; bool error;
  { std::lock_guard<std::mutex> g(st.lock); status = st.status; error = st.status_is_error; }
  if (!status.empty()) {
    const float y = top + ROW_COUNT * (rowh + 12 * s) + 8 * s;
    d->AddText(font, 20 * s, ImVec2(px, y), u32(error ? kError : st.done ? kOk : kMuted), status.c_str());
  }
  d->AddText(font, 18 * s, ImVec2(px, h - 56 * s), u32(kMuted),
             st.done ? "A  Continue" : "A  Select     B  Play offline     +  Quit");
}
}  // namespace

SignInResult switch_signin_run(MetalFrontend& frontend, const std::string& user_dir, std::string& account_name,
                               std::string& connect_code) {
  State st;
  {
    slippi::login::Session saved;
    if (slippi::login::read_session(user_dir, saved)) st.email = saved.email;
  }
  PADInit();
  uint16_t last_buttons = 0xFFFF;   // ignore whatever is held at entry
  int8_t last_y = 0;
  SignInResult result = SignInResult::Quit;
  for (;;) {
    if (!frontend.pump_events() || frontend.close_requested()) { result = SignInResult::Quit; break; }
    const Edges e = read_edges(last_buttons, last_y);
    bool finished = false;
    { std::lock_guard<std::mutex> g(st.lock); finished = st.done; }
    if (finished && e.a) { result = SignInResult::SignedIn; break; }
    if (!st.busy && !finished) {
      if (e.up) st.row = (st.row + ROW_COUNT - 1) % ROW_COUNT;
      if (e.down) st.row = (st.row + 1) % ROW_COUNT;
      if (e.b) { result = SignInResult::PlayOffline; break; }
      if (e.plus) { result = SignInResult::Quit; break; }
      if (e.a) {
        switch (st.row) {
          case ROW_EMAIL: keyboard("Slippi account", "Email address", false, st.email); break;
          case ROW_PASSWORD: keyboard("Slippi account", "Password", true, st.password); break;
          case ROW_SIGN_IN:
            if (st.email.empty() || st.password.empty()) {
              std::lock_guard<std::mutex> g(st.lock);
              st.status = "Enter your email address and password first"; st.status_is_error = true;
            } else start_sign_in(st, user_dir);
            break;
          case ROW_OFFLINE: result = SignInResult::PlayOffline; break;
        }
        if (result == SignInResult::PlayOffline) break;
      }
    }
    const MetalWindowSize size = frontend.window_size();
    if (aurora_begin_frame()) {
      draw(st, (int)size.width, (int)size.height);
      aurora_end_frame();
    }
  }
  if (st.worker.joinable()) st.worker.join();
  if (result == SignInResult::SignedIn) { account_name = st.account.display_name; connect_code = st.account.connect_code; }
  return result;
}
}  // namespace host
