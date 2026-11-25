// A USB keyboard plugged into the console (the dock's or an adapter's USB port), read through libnx's HID keyboard
// (hidInitializeKeyboard, hidGetKeyboardStates): as a controller and for typing.
//
// As a controller: melee-unlocked's keyboard (port/runtime/host/input_bindings.h default_key_bindings and
// keyboard_ui_navigation.h keyboard_apply_bindings, GPL-2.0-or-later): Z X C V are A B X Y, Q W E are L R Z, Enter is
// Start, T F G H the D-pad, I J K L the C-stick, the arrows the stick and Shift walks (upstream's stick modifier, half a
// push). Every one of the 27 actions can be rebound on the menu's Controller bindings page (device "USB keyboard"), the
// stick modifier push and the rest of the setup on Controller setup, saved in launcher.ini (padx.kb) and as upstream's
// "keyboard" profiles (Windows virtual-key codes, so a profile saved by melee-unlocked on a PC loads here). The keyboard
// plays as port 1 (settings.ini keyboard_port 1..4), added to whatever controller is on that port; while the in-game menu
// is open Enter and Escape are A and B there, as upstream's ImGui navigation has them.
//
// For typing: every key and character goes to the ImGui frame (io.AddKeyEvent / AddInputCharacter), which is how
// melee-unlocked's own text boxes take a keyboard: the Practice menu's connect-code keyboard (Direct, Teams and the codes
// skipped in Unranked) takes typed letters, digits and Backspace. The screens that ask the console's software keyboard for
// words (fe::software_keyboard: the settings search, the Replay library's search) get a line typed on the keyboard instead
// once a key has been pressed this session: a box over the game, Enter takes the words, Escape cancels. While a box or the
// Practice menu's code keyboard is open the keyboard does not reach the game.
//
// A keyboard counts as there once one of its keys has been pressed (the HID keyboard has no connection flag). The game
// thread reads it at each controller poll; the render thread reads the events that poll left.
// Off the console (Linux tests): MU_TEST_KEYS is a script of keys instead, keyed on the controller polls:
//   "START:KEYS:LENGTH ..."  KEYS held for LENGTH polls from poll START; KEYS are key names joined by '+' (A..Z, 0..9, Enter,
//                            Escape, Backspace, Tab, Space, Up, Down, Left, Right, Shift, Ctrl, Minus, F1, F11)
//   "START:type=TEXT"        TEXT typed from poll START, one character every 4 polls (Shift added for '#' and capitals)
// and MU_TEST_KEY_FILE names a file re-read at every poll (as the app's test pad file): the key names held now, separated by
// spaces, or "type TEXT" (typed from then on).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"
#include "host.h"                // shim: PadState
#include "controls_core.h"
#include "app/game_menu.h"       // menu_is_open (the build-time copy with patches/0104)

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace fe {
void controls_apply_keyboard(const uint8_t* keys, void* pad, int lane);   // controls.cpp: the keyboard's bindings and setup

namespace {
// ---- HID usage codes (USB HID usage page 7, the codes libnx's HidKeyboardKey and SDL's scancodes use) ----
// Each usage's Windows virtual-key code (upstream's binding numbers), its character and its Shift character (US layout).
struct Usage { uint8_t vk; char plain, shifted; ImGuiKey imgui; };
Usage usage_info(int u) {
  if (u >= 0x04 && u <= 0x1D) { const char c = char('a' + u - 0x04); return {uint8_t('A' + u - 0x04), c, char(c - 32), ImGuiKey(ImGuiKey_A + (u - 0x04))}; }
  if (u >= 0x1E && u <= 0x27) {
    static const char shifted[] = "!@#$%^&*()";
    const int d = u == 0x27 ? 0 : u - 0x1E + 1;
    return {uint8_t('0' + d), char('0' + d), shifted[u - 0x1E], ImGuiKey(ImGuiKey_0 + d)};
  }
  if (u >= 0x3A && u <= 0x45) return {uint8_t(0x70 + u - 0x3A), 0, 0, ImGuiKey(ImGuiKey_F1 + (u - 0x3A))};   // F1..F12
  if (u >= 0x59 && u <= 0x61) return {uint8_t(0x61 + u - 0x59), char('1' + u - 0x59), char('1' + u - 0x59), ImGuiKey(ImGuiKey_Keypad1 + (u - 0x59))};
  switch (u) {
    case 0x28: return {0x0D, 0, 0, ImGuiKey_Enter};        case 0x29: return {0x1B, 0, 0, ImGuiKey_Escape};
    case 0x2A: return {0x08, 0, 0, ImGuiKey_Backspace};    case 0x2B: return {0x09, 0, 0, ImGuiKey_Tab};
    case 0x2C: return {0x20, ' ', ' ', ImGuiKey_Space};    case 0x2D: return {0xBD, '-', '_', ImGuiKey_Minus};
    case 0x2E: return {0xBB, '=', '+', ImGuiKey_Equal};    case 0x2F: return {0xDB, '[', '{', ImGuiKey_LeftBracket};
    case 0x30: return {0xDD, ']', '}', ImGuiKey_RightBracket}; case 0x31: return {0xDC, '\\', '|', ImGuiKey_Backslash};
    case 0x33: return {0xBA, ';', ':', ImGuiKey_Semicolon}; case 0x34: return {0xDE, '\'', '"', ImGuiKey_Apostrophe};
    case 0x35: return {0xC0, '`', '~', ImGuiKey_GraveAccent}; case 0x36: return {0xBC, ',', '<', ImGuiKey_Comma};
    case 0x37: return {0xBE, '.', '>', ImGuiKey_Period};   case 0x38: return {0xBF, '/', '?', ImGuiKey_Slash};
    case 0x39: return {0x14, 0, 0, ImGuiKey_CapsLock};
    case 0x49: return {0x2D, 0, 0, ImGuiKey_Insert};       case 0x4A: return {0x24, 0, 0, ImGuiKey_Home};
    case 0x4B: return {0x21, 0, 0, ImGuiKey_PageUp};       case 0x4C: return {0x2E, 0, 0, ImGuiKey_Delete};
    case 0x4D: return {0x23, 0, 0, ImGuiKey_End};          case 0x4E: return {0x22, 0, 0, ImGuiKey_PageDown};
    case 0x4F: return {0x27, 0, 0, ImGuiKey_RightArrow};   case 0x50: return {0x25, 0, 0, ImGuiKey_LeftArrow};
    case 0x51: return {0x28, 0, 0, ImGuiKey_DownArrow};    case 0x52: return {0x26, 0, 0, ImGuiKey_UpArrow};
    case 0x54: return {0x6F, '/', '/', ImGuiKey_KeypadDivide}; case 0x55: return {0x6A, '*', '*', ImGuiKey_KeypadMultiply};
    case 0x56: return {0x6D, '-', '-', ImGuiKey_KeypadSubtract}; case 0x57: return {0x6B, '+', '+', ImGuiKey_KeypadAdd};
    case 0x58: return {0x0D, 0, 0, ImGuiKey_KeypadEnter};  case 0x62: return {0x60, '0', '0', ImGuiKey_Keypad0};
    case 0x63: return {0x6E, '.', '.', ImGuiKey_KeypadDecimal};
    case 0xE0: return {0xA2, 0, 0, ImGuiKey_LeftCtrl};     case 0xE1: return {0xA0, 0, 0, ImGuiKey_LeftShift};
    case 0xE2: return {0xA4, 0, 0, ImGuiKey_LeftAlt};      case 0xE3: return {0x5B, 0, 0, ImGuiKey_LeftSuper};
    case 0xE4: return {0xA3, 0, 0, ImGuiKey_RightCtrl};    case 0xE5: return {0xA1, 0, 0, ImGuiKey_RightShift};
    case 0xE6: return {0xA5, 0, 0, ImGuiKey_RightAlt};     case 0xE7: return {0x5C, 0, 0, ImGuiKey_RightSuper};
    default: return {0, 0, 0, ImGuiKey_None};
  }
}
// The generic virtual keys upstream binds (VK_SHIFT, VK_CONTROL, VK_MENU) for the side-specific ones.
uint8_t generic_vk(uint8_t vk) {
  if (vk == 0xA0 || vk == 0xA1) return 0x10;
  if (vk == 0xA2 || vk == 0xA3) return 0x11;
  if (vk == 0xA4 || vk == 0xA5) return 0x12;
  return 0;
}

struct Event { int usage; bool down; char ch; };

std::mutex mu;                       // everything below
uint8_t held_usage[32] = {};         // the keys down now, by HID usage (256 bits)
uint8_t held_vk[32] = {};            // the same keys as virtual keys (and the generic Shift / Ctrl / Alt)
bool shift = false, caps = false;
std::atomic<bool> seen{false};       // a key has been pressed this session
std::deque<Event> events;            // for the render thread's ImGui frame
std::atomic<int64_t> overlay_ns{0};  // the last time the in-game overlay ran (the box can only be shown while it does)
std::atomic<bool> typing{false};     // a box or a text field takes the keys: the keyboard does not reach the game
std::atomic<bool> swallow{false};    // a box just closed: its Enter or Escape is kept from the game and the menu until every key is up
uint64_t polls = 0;

int64_t now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
bool bit(const uint8_t* b, int i) { return i >= 0 && i < 256 && (b[i >> 3] >> (i & 7) & 1); }
void set_bit(uint8_t* b, int i, bool on) { if (i <= 0 || i >= 256) return; if (on) b[i >> 3] |= uint8_t(1u << (i & 7)); else b[i >> 3] &= uint8_t(~(1u << (i & 7))); }

// The new set of keys down (by usage): edges become events, the virtual-key set follows. Under mu.
void take_locked(const uint8_t now[32], bool caps_lock) {
  caps = caps_lock;
  shift = bit(now, 0xE1) || bit(now, 0xE5);
  for (int u = 1; u < 256; ++u) {
    const bool was = bit(held_usage, u), is = bit(now, u);
    if (was == is) continue;
    const Usage info = usage_info(u);
    char ch = 0;
    if (is && info.plain) {
      const bool letter = info.plain >= 'a' && info.plain <= 'z';
      ch = (shift != (letter && caps)) ? info.shifted : info.plain;
    }
    if (is && !seen.exchange(true)) {
      char line[96];
      std::snprintf(line, sizeof line, "keyboard: a USB keyboard is in use (its first key: usage %02X)", u);
      if (!log_queue_push(line)) say("%s", line);
    }
    events.push_back({u, is, ch});
    if (events.size() > 256) events.pop_front();
  }
  std::memcpy(held_usage, now, sizeof held_usage);
  std::memset(held_vk, 0, sizeof held_vk);
  for (int u = 1; u < 256; ++u) {
    if (!bit(held_usage, u)) continue;
    const uint8_t vk = usage_info(u).vk;
    set_bit(held_vk, vk, true);
    set_bit(held_vk, generic_vk(vk), true);
  }
}

// ---- Linux tests: MU_TEST_KEYS ----
struct Step { uint64_t from, length; std::vector<int> usages; };
int usage_by_name(const std::string& n) {
  if (n.size() == 1 && n[0] >= 'A' && n[0] <= 'Z') return 0x04 + n[0] - 'A';
  if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z') return 0x04 + n[0] - 'a';
  if (n.size() == 1 && n[0] >= '1' && n[0] <= '9') return 0x1E + n[0] - '1';
  if (n == "0") return 0x27;
  static const struct { const char* name; int usage; } names[] = {{"Enter", 0x28}, {"Escape", 0x29}, {"Backspace", 0x2A}, {"Tab", 0x2B},
      {"Space", 0x2C}, {"Minus", 0x2D}, {"Right", 0x4F}, {"Left", 0x50}, {"Down", 0x51}, {"Up", 0x52}, {"Shift", 0xE1}, {"Ctrl", 0xE0},
      {"RShift", 0xE5}, {"F1", 0x3A}, {"F11", 0x44}};
  for (const auto& e : names) if (n == e.name) return e.usage;
  return 0;
}
std::vector<Step> parse_steps(const std::string& all) {
    std::vector<Step> out;
    size_t at = 0;
    while (at < all.size()) {
      size_t end = all.find(' ', at);
      if (end == std::string::npos) end = all.size();
      const std::string item = all.substr(at, end - at);
      at = end + 1;
      const size_t c1 = item.find(':');
      if (c1 == std::string::npos) continue;
      const uint64_t from = std::strtoull(item.c_str(), nullptr, 10);
      const std::string rest = item.substr(c1 + 1);
      if (rest.compare(0, 5, "type=") == 0) {   // one character every 4 polls, held 2
        uint64_t t = from;
        for (const char c : rest.substr(5)) {
          Step s{t, 2, {}};
          if (c == '#') s.usages = {0xE1, 0x20};                      // Shift + 3
          else if (c >= 'A' && c <= 'Z') s.usages = {0xE1, 0x04 + c - 'A'};
          else if (c == '_') s.usages = {0xE1, 0x2D};
          else { const int u = usage_by_name(std::string(1, c)); if (u) s.usages = {u}; else if (c == ' ') s.usages = {0x2C}; }
          if (!s.usages.empty()) out.push_back(s);
          t += 4;
        }
        continue;
      }
      const size_t c2 = rest.find(':');
      Step s{from, c2 == std::string::npos ? 2 : std::strtoull(rest.c_str() + c2 + 1, nullptr, 10), {}};
      const std::string keys = rest.substr(0, c2);
      size_t k = 0;
      while (k <= keys.size()) {
        size_t plus = keys.find('+', k);
        if (plus == std::string::npos) plus = keys.size();
        if (const int u = usage_by_name(keys.substr(k, plus - k))) s.usages.push_back(u);
        k = plus + 1;
      }
      if (!s.usages.empty()) out.push_back(s);
    }
    return out;
}
const std::vector<Step>& test_steps() {
  static const std::vector<Step> steps = [] {
    const char* script = std::getenv("MU_TEST_KEYS");
    if (!script) return std::vector<Step>{};
    std::vector<Step> out = parse_steps(script);
    say("keyboard: test: %zu key steps (MU_TEST_KEYS)", out.size());
    return out;
  }();
  return steps;
}

// One read of the keyboard. Game thread (each controller poll) or a box's wait (when the game thread is not polling).
void read_keyboard() {
  uint8_t now[32] = {};
  bool caps_lock = false;
#ifdef __SWITCH__
  static bool initialized = false;
  if (!initialized) { hidInitializeKeyboard(); initialized = true; }
  HidKeyboardState state{};
  if (hidGetKeyboardStates(&state, 1) == 0) return;
  for (int w = 0; w < 4; ++w)
    for (int b = 0; b < 64; ++b)
      if (state.keys[w] >> b & 1) set_bit(now, w * 64 + b, true);
  caps_lock = (state.modifiers & HidKeyboardModifier_CapsLock) != 0;
#else
  const auto& steps = test_steps();
  static const char* key_file = std::getenv("MU_TEST_KEY_FILE");
  if (steps.empty() && !key_file) return;
  for (const Step& s : steps)
    if (polls >= s.from && polls < s.from + s.length) for (const int u : s.usages) set_bit(now, u, true);
  if (key_file) {   // the keys held now, re-read at every poll: names separated by spaces, or "type TEXT" (typed from this poll on)
    static std::string last;
    static std::vector<Step> typed;
    static std::vector<int> held;
    std::string text;
    if (FILE* f = std::fopen(key_file, "r")) { char b[256]; const size_t n = std::fread(b, 1, sizeof b - 1, f); b[n] = 0; text = b; std::fclose(f); }
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    if (text != last) {
      last = text; held.clear();
      if (text.compare(0, 5, "type ") == 0) typed = parse_steps(std::to_string(polls) + ":type=" + text.substr(5));
      else for (size_t at = 0; at < text.size();) {
        size_t end = text.find(' ', at); if (end == std::string::npos) end = text.size();
        if (const int u = usage_by_name(text.substr(at, end - at))) held.push_back(u);
        at = end + 1;
      }
    }
    for (const int u : held) set_bit(now, u, true);
    for (const Step& s : typed)
      if (polls >= s.from && polls < s.from + s.length) for (const int u : s.usages) set_bit(now, u, true);
  }
#endif
  std::lock_guard<std::mutex> lock(mu);
  ++polls;
  take_locked(now, caps_lock);
}

// ---- the box a screen's words are typed in (software_keyboard) ----
struct Box { bool open = false, done = false, ok = false, focus = false; std::string header, guide; char text[128] = {}; };
Box box;
std::condition_variable box_cv;
std::atomic<int64_t> game_poll_ns{0};
}  // namespace

bool kb_seen() { return seen.load(std::memory_order_relaxed); }
void kb_keys(uint8_t out[32]) { std::lock_guard<std::mutex> lock(mu); std::memcpy(out, held_vk, 32); }

// Simulation thread (lane 0, the game's poll) and the input-suggestion reads (lane 1): the keyboard's pad onto its port.
void kb_merge_pads(host::PadState pads[4], int lane) {
  if (lane == 0) { read_keyboard(); game_poll_ns.store(now_ns(), std::memory_order_relaxed); }
  if (!seen.load(std::memory_order_relaxed)) return;
  uint8_t keys[32];
  { std::lock_guard<std::mutex> lock(mu); std::memcpy(keys, held_vk, sizeof keys); }
  if (typing.load(std::memory_order_relaxed)) return;   // a box or a text field has the keys
  if (swallow.load(std::memory_order_relaxed)) {
    bool any = false;
    for (const uint8_t b : keys) any = any || b;
    if (any) return;
    swallow.store(false, std::memory_order_relaxed);
  }
  host::PadState k{};
  controls_apply_keyboard(keys, &k, lane);
  if (host::menu_is_open()) {   // upstream's ImGui navigation: Enter is A, Escape is B
    if (bit(keys, 0x0D)) { k.button = uint16_t(k.button & ~0x1000) | 0x0100; }
    if (bit(keys, 0x1B)) k.button |= 0x0200;
  }
  static const int port = [] { const int p = std::atoi(settings_get("keyboard_port").c_str()); return p >= 1 && p <= 4 ? p - 1 : 0; }();
  static host::PadState logged_pad{}; static int logged = 0;   // the first changes, for the log (tests read them)
  if (lane == 0 && logged < 80 && std::memcmp(&k, &logged_pad, sizeof k) != 0) {
    logged_pad = k; ++logged;
    char line[160];
    std::snprintf(line, sizeof line, "keyboard: port %d gets buttons %04X, stick %d %d, C-stick %d %d, L %u R %u", port + 1, k.button, k.stick_x, k.stick_y,
                  k.sub_x, k.sub_y, unsigned(k.trig_l), unsigned(k.trig_r));
    if (!log_queue_push(line)) say("%s", line);
  }
  host::PadState& p = pads[port];
  if (p.err != 0) { p = k; p.err = 0; return; }
  p.button |= k.button;
  if (k.stick_x || k.stick_y) { p.stick_x = k.stick_x; p.stick_y = k.stick_y; }
  if (k.sub_x || k.sub_y) { p.sub_x = k.sub_x; p.sub_y = k.sub_y; }
  p.trig_l = std::max(p.trig_l, k.trig_l);
  p.trig_r = std::max(p.trig_r, k.trig_r);
}

// Render thread, inside the in-game overlay's ImGui frame (after everything else is drawn): the keyboard's events to ImGui and
// the box, if a screen is waiting for words. `text_wanted`: some other text field takes the keys this frame (the Practice
// menu's code keyboard).
void kb_frame(int window_w, int window_h, bool text_wanted) {
  overlay_ns.store(now_ns(), std::memory_order_relaxed);
  ImGuiIO& io = ImGui::GetIO();
  std::unique_lock<std::mutex> lock(mu);
  while (!events.empty()) {
    const Event e = events.front(); events.pop_front();
    const Usage info = usage_info(e.usage);
    if (info.imgui != ImGuiKey_None) io.AddKeyEvent(info.imgui, e.down);
    if (e.usage == 0xE1 || e.usage == 0xE5) io.AddKeyEvent(ImGuiMod_Shift, shift);
    if (e.usage == 0xE0 || e.usage == 0xE4) io.AddKeyEvent(ImGuiMod_Ctrl, e.down);
    if (e.ch) io.AddInputCharacter(static_cast<unsigned>(static_cast<unsigned char>(e.ch)));
  }
  typing.store(box.open || text_wanted || io.WantTextInput, std::memory_order_relaxed);
  if (!box.open) return;
  const float u = float(window_h) / 720.0f;
  ImGui::SetNextWindowPos(ImVec2(window_w * 0.5f, window_h * 0.42f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(float(window_w) - 40.0f, 760.0f * u), 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.94f);
  ImGui::Begin("##keyboard_box", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
  ImGui::SetWindowFontScale(1.6f * u);
  ImGui::TextUnformatted(box.header.c_str());
  ImGui::SetWindowFontScale(1.2f * u);
  ImGui::TextDisabled("%s", box.guide.c_str());
  ImGui::SetWindowFontScale(1.6f * u);
  ImGui::PushItemWidth(-1.0f);
  if (box.focus) { ImGui::SetKeyboardFocusHere(); box.focus = false; }
  const bool entered = ImGui::InputText("##keyboard_text", box.text, sizeof box.text, ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::PopItemWidth();
  ImGui::SetWindowFontScale(1.0f * u);
  ImGui::TextDisabled("Enter: done    Escape: cancel");
  ImGui::End();
  if (entered || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    box.done = true; box.ok = entered; box.open = false;
    swallow.store(true, std::memory_order_relaxed);
    lock.unlock();
    box_cv.notify_all();
  }
}

// software_keyboard.cpp: a screen asks for words. With a keyboard in use and the in-game overlay running, the box above;
// false when the console's software keyboard should be used instead.
bool kb_prompt(const char* header, const char* guide, std::string& text, bool* ok_out) {
  if (!seen.load() || now_ns() - overlay_ns.load() > 500'000'000LL) return false;
  std::unique_lock<std::mutex> lock(mu);
  if (box.open) return false;
  box = Box{};
  box.open = box.focus = true; box.header = header ? header : ""; box.guide = guide ? guide : "";
  std::snprintf(box.text, sizeof box.text, "%s", text.c_str());
  say("keyboard: %s: typed on the USB keyboard", box.header.c_str());
  // The render thread draws the box and takes Enter or Escape; the keys come from the game's polls, or from here while the
  // game thread is not polling (the game paused behind the menu still polls; this is for the rest).
  while (!box.done) {
    box_cv.wait_for(lock, std::chrono::milliseconds(16));
    if (now_ns() - overlay_ns.load() > 2'000'000'000LL) { box.open = false; say("keyboard: %s: the picture went away; cancelled", box.header.c_str()); return true; }
    if (now_ns() - game_poll_ns.load() > 100'000'000LL) { lock.unlock(); read_keyboard(); lock.lock(); }
  }
  *ok_out = box.ok;
  if (box.ok) text = box.text;
  say("keyboard: %s: %s", box.header.c_str(), box.ok ? ("\"" + text + "\"").c_str() : "cancelled");
  return true;
}

// The names the binding rows show (upstream's virtual keys).
std::string kb_key_name(uint32_t vk) {
  if (vk >= 'A' && vk <= 'Z') return std::string(1, char(vk));
  if (vk >= '0' && vk <= '9') return std::string(1, char(vk));
  if (vk >= 0x70 && vk <= 0x7B) return "F" + std::to_string(vk - 0x6F);
  if (vk >= 0x60 && vk <= 0x69) return "Keypad " + std::to_string(vk - 0x60);
  switch (vk) {
    case 0x0D: return "Enter"; case 0x1B: return "Escape"; case 0x08: return "Backspace"; case 0x09: return "Tab"; case 0x20: return "Space";
    case 0x10: return "Shift"; case 0x11: return "Ctrl"; case 0x12: return "Alt"; case 0x14: return "Caps Lock";
    case 0xA0: return "Left Shift"; case 0xA1: return "Right Shift"; case 0xA2: return "Left Ctrl"; case 0xA3: return "Right Ctrl";
    case 0xA4: return "Left Alt"; case 0xA5: return "Right Alt";
    case 0x25: return "Left arrow"; case 0x26: return "Up arrow"; case 0x27: return "Right arrow"; case 0x28: return "Down arrow";
    case 0x2D: return "Insert"; case 0x2E: return "Delete"; case 0x24: return "Home"; case 0x23: return "End"; case 0x21: return "Page Up"; case 0x22: return "Page Down";
    case 0xBA: return ";"; case 0xBB: return "="; case 0xBC: return ","; case 0xBD: return "-"; case 0xBE: return "."; case 0xBF: return "/";
    case 0xC0: return "`"; case 0xDB: return "["; case 0xDC: return "\\"; case 0xDD: return "]"; case 0xDE: return "'";
    default: { char b[16]; std::snprintf(b, sizeof b, "Key %02X", unsigned(vk)); return b; }
  }
}
}  // namespace fe
