// Checks of the controller setup (frontend/src/controls_core.h, controls_files.cpp) without a game or a controller:
// box coordinates and the opposing-button rule, snapback, deadzone, radius, trigger click and cap, adapter and SDL
// bindings, profiles (ours and one in melee-unlocked's own gcadapter form) and Dolphin profile import.
// Build and run: tools/controls_check.sh (in the aarch64 Linux container, no network).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "controls_core.h"
#include "controls_files.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>

using namespace fe::controls;
static int checks = 0, failed = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++failed; std::printf("FAILED line %d: %s\n", __LINE__, #cond); } } while (0)

static Pad neutral() { return Pad{0, 0, 0, 0, 0, 0, 0}; }
static Pad run(const Config& c, uint32_t buttons, State& st, double now = 0, Pad p = neutral(), uint8_t lt = 0, uint8_t rt = 0) {
  Raw raw; raw.buttons = buttons; raw.lt = lt; raw.rt = rt;
  apply(c, raw, 200, now, st, p);
  return p;
}

int main(int argc, char** argv) {
  const std::string tmp = argc > 1 ? argv[1] : "/tmp/controls-check";
  mkdir(tmp.c_str(), 0777);

  // Defaults change nothing and serialize to nothing.
  for (Kind k : {Kind::Sdl, Kind::Adapter}) {
    const Config d = Config::defaults(k);
    CHECK(d.is_default());
    CHECK(d.serialize().empty());
    State st;
    Pad in{0x0140, 40, -90, 10, 0, 120, 0};
    const Pad out = run(d, k == Kind::Adapter ? 0x0140 : 0, st, 0, in, 120, 0);
    CHECK(std::memcmp(&in, &out, sizeof in) == 0);
  }

  // An SDL pad as a box: D-pad (SDL 11..14) is the stick, L shoulder (9) Mod X, R shoulder (10) Mod Y.
  Config box = Config::defaults(Kind::Sdl);
  box.mask[SUp] = 1u << 11; box.mask[SDown] = 1u << 12; box.mask[SLeft] = 1u << 13; box.mask[SRight] = 1u << 14;
  box.mask[ModX] = 1u << 9; box.mask[ModY] = 1u << 10; box.mask[LightShield] = 1u << 4;
  box.mask[CUp] = 1u << 3; box.mask[CRight] = 1u << 1;
  CHECK(box.box_mode());
  {
    State st;
    Pad p = run(box, 1u << 14, st);                         // right alone: full
    CHECK(p.stick_x == 80 && p.stick_y == 0);
    p = run(box, (1u << 14) | (1u << 12), st);              // right + down: the diagonal
    CHECK(p.stick_x == 56 && p.stick_y == -56);
    p = run(box, (1u << 14) | (1u << 9), st);               // Mod X + right: walk
    CHECK(p.stick_x == 53 && p.stick_y == 0);
    p = run(box, (1u << 14) | (1u << 12) | (1u << 4), st);  // shield + down-right: shield drop angle
    CHECK(p.stick_x == 56 && p.stick_y == -55 && p.trig_r == box::kLightShield);
    p = run(box, (1u << 14) | (1u << 12) | (1u << 9) | (1u << 4), st);   // Mod X + shield diagonal: the long wavedash
    CHECK(p.stick_x == 51 && p.stick_y == -30);
    p = run(box, (1u << 9) | (1u << 10) | (1u << 3), st);   // Mod X + Mod Y: the C buttons are the D-pad
    CHECK((p.button & 0x0008) && p.sub_x == 0 && p.sub_y == 0);
    p = run(box, 1u << 1, st);                              // C-right
    CHECK(p.sub_x == 80 && p.sub_y == 0);
  }
  {   // second input priority without reactivation
    State st;
    Pad p = run(box, 1u << 13, st); CHECK(p.stick_x == -80);
    p = run(box, (1u << 13) | (1u << 14), st); CHECK(p.stick_x == 80);   // right pressed last wins
    p = run(box, 1u << 13, st); CHECK(p.stick_x == 0);                   // left stays dead until let go
    p = run(box, 0, st); p = run(box, 1u << 13, st); CHECK(p.stick_x == -80);
  }
  {   // without Mod X / Mod Y: plain pushes, the modifier walks
    Config plain = box;
    plain.mask[ModX] = plain.mask[ModY] = 0; plain.mask[StickMod] = 1u << 9;
    State st;
    Pad p = run(plain, 1u << 14, st); CHECK(p.stick_x == 127);
    p = run(plain, (1u << 14) | (1u << 9), st); CHECK(p.stick_x == 64);
    plain.modifier_percent = 40;
    p = run(plain, (1u << 14) | (1u << 9), st); CHECK(p.stick_x == 51);
    p = run(plain, 1u << 3, st); CHECK(p.sub_y == 127);
  }

  // Trigger travel from a button, and an SDL trigger as a button.
  {
    Config c = Config::defaults(Kind::Sdl);
    c.mask[LAnalog] = 1u << 9;   // L shoulder presses L a light-shield depth
    State st;
    Pad p = run(c, 1u << 9, st); CHECK(p.trig_l == 100);
    c.level[LAnalog] = 60; p = run(c, 1u << 9, st); CHECK(p.trig_l == 60);
    c.mask[CDown] = kSrcRT;     // ZR past the press point pushes the C-stick down
    p = run(c, 0, st, 0, neutral(), 0, 210); CHECK(p.sub_y == -127);
    p = run(c, 0, st, 0, neutral(), 0, 150); CHECK(p.sub_y == 0);
  }

  // Adapter: A and B swapped, L travel to a button.
  {
    Config c = Config::defaults(Kind::Adapter);
    c.mask[A] = kPadBit[B]; c.mask[B] = kPadBit[A];
    State st;
    Pad in{0x0100, 0, 0, 0, 0, 0, 0};
    Pad p = run(c, 0x0100, st, 0, in); CHECK(p.button == 0x0200);
    c.mask[LAnalog] = kPadBit[X]; c.mask[X] = 0;   // X presses L travel 100 and is no longer X
    p = run(c, 0x0400, st, 0, in); CHECK(p.trig_l == 100 && !(p.button & 0x0400));
    // L switch with the trigger at rest: a full press
    p = run(Config::defaults(Kind::Adapter), 0x0040, st, 0, Pad{0x0040, 0, 0, 0, 0, 0, 0}, 0, 0); CHECK(p.button == 0x0040);
  }

  // Click point and cap, deadzone, radius, inversion.
  {
    Config c = Config::defaults(Kind::Adapter);
    c.click_l = 100;
    State st;
    Pad p = run(c, 0, st, 0, Pad{0, 0, 0, 0, 0, 120, 0}, 120, 0); CHECK(p.button & kPadL);
    p = run(c, 0x0040, st, 0, Pad{0x0040, 0, 0, 0, 0, 90, 0}, 90, 0); CHECK(!(p.button & kPadL));
    c = Config::defaults(Kind::Adapter); c.cap_r = 80;
    p = run(c, 0x0020, st, 0, Pad{0x0020, 0, 0, 0, 0, 0, 230}, 0, 230); CHECK(p.trig_r == 80 && !(p.button & kPadR));
    c = Config::defaults(Kind::Sdl); c.dz_main = 10;
    p = run(c, 0, st, 0, Pad{0, 5, 7, 0, 0, 0, 0}); CHECK(p.stick_x == 0 && p.stick_y == 0);
    p = run(c, 0, st, 0, Pad{0, 9, 7, 0, 0, 0, 0}); CHECK(p.stick_x == 9 && p.stick_y == 7);
    c = Config::defaults(Kind::Sdl); c.radius_main = 90;
    p = run(c, 0, st, 0, Pad{0, 127, -127, 0, 0, 0, 0}); CHECK(p.stick_x == 114 && p.stick_y == -114);
    c = Config::defaults(Kind::Sdl); c.invert = kInvertStickY | kInvertCX;
    p = run(c, 0, st, 0, Pad{0, 20, 30, 40, 50, 0, 0}); CHECK(p.stick_x == 20 && p.stick_y == -30 && p.sub_x == -40 && p.sub_y == 50);
  }

  // Snapback: a bounce past centre within the window reads as centre; a real dashback passes.
  {
    Config c = Config::defaults(Kind::Sdl); c.snap_x = 40; c.snap_ms_x = 20;
    State st;
    Pad p = run(c, 0, st, 0.000, Pad{0, 127, 0, 0, 0, 0, 0}); CHECK(p.stick_x == 127);
    p = run(c, 0, st, 0.004, Pad{0, -30, 0, 0, 0, 0, 0}); CHECK(p.stick_x == 0);
    p = run(c, 0, st, 0.008, Pad{0, -100, 0, 0, 0, 0, 0}); CHECK(p.stick_x == -100);
    p = run(c, 0, st, 0.100, Pad{0, 127, 0, 0, 0, 0, 0});
    p = run(c, 0, st, 0.101, Pad{0, 10, 0, 0, 0, 0, 0});
    p = run(c, 0, st, 0.140, Pad{0, -30, 0, 0, 0, 0, 0}); CHECK(p.stick_x == -30);   // after the window
    Config a = Config::defaults(Kind::Adapter); a.snap_x = 40;   // not on a GameCube controller
    State sa;
    run(a, 0, sa, 0.0, Pad{0, 127, 0, 0, 0, 0, 0});
    p = run(a, 0, sa, 0.004, Pad{0, -30, 0, 0, 0, 0, 0}); CHECK(p.stick_x == -30);
  }

  // launcher.ini form.
  {
    Config c = box; c.dz_main = 8; c.radius_c = 95; c.snap_y = 30; c.level[LAnalog] = 70;
    const Config back = Config::parse(Kind::Sdl, c.serialize());
    CHECK(back.same_as(c));
    CHECK(Config::parse(Kind::Adapter, "").is_default());
  }

  // Profiles: ours round-trip; one written as melee-unlocked writes a gcadapter profile loads.
  {
    const std::string folder = tmp + "/ControllerProfiles";
    Profile p;
    p.config = box; p.has_app = true; p.trigger_press = 60; p.swap_sticks = true;
    for (int i = 0; i < 12; ++i) p.config.mask[i] = 1u << i;
    CHECK(profile_save(folder, "Box 1", p));
    CHECK(!profile_save(folder, "../escape", p));
    Profile q; q.config = Config::defaults(Kind::Sdl);
    CHECK(profile_load(folder, "Box 1", q));
    CHECK(q.config.same_as(p.config) && q.trigger_press == 60 && q.swap_sticks);
    CHECK(profile_list(folder, Kind::Sdl).size() == 1 && profile_list(folder, Kind::Adapter).empty());
    Profile wrong; wrong.config = Config::defaults(Kind::Adapter);
    CHECK(!profile_load(folder, "Box 1", wrong));   // another kind's file
    {
      std::ofstream up(folder + "/Upstream GC.profile");
      up << "# Melee Unlocked controller profile\ndevice gcadapter\n";
      const uint32_t v[kActions] = {512, 256, 1024, 2048, 16, 4096, 64, 32, 8, 4, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 64, 32, 0, 0, 0, 0, 0};
      for (int i = 0; i < kActions; ++i) up << kActionNames[i] << " " << v[i] << "\n";
    }
    Profile g; g.config = Config::defaults(Kind::Adapter);
    CHECK(profile_load(folder, "Upstream GC", g));
    CHECK(g.config.mask[A] == kPadBit[B] && g.config.mask[B] == kPadBit[A] && g.config.mask[LAnalog] == kPadL);
    CHECK(profile_free_name(folder, "Box") == "Box 2");
    CHECK(profile_delete(folder, "Box 1", Kind::Sdl) && profile_list(folder, Kind::Sdl).empty());
  }

  // Dolphin.
  {
    const std::string xinput =
        "[Profile]\nDevice = XInput/0/Gamepad\nButtons/A = `Button A`\nButtons/B = `Button X`\nButtons/Z = `Shoulder R` | Back\n"
        "Buttons/Start = Start\nTriggers/L = `Trigger L`\nTriggers/R = `Trigger R`\nTriggers/Threshold = 85.0\n"
        "D-Pad/Up = `Pad N`\nMain Stick/Up = `Left Y+`\nMain Stick/Down = `Left Y-`\nMain Stick/Left = `Left X-`\nMain Stick/Right = `Left X+`\n"
        "Main Stick/Dead Zone = 12.0\nC-Stick/Up = `Right Y-`\nC-Stick/Down = `Right Y+`\nC-Stick/Left = `Right X-`\nC-Stick/Right = `Right X+`\n"
        "Main Stick/Modifier = `Thumb L`\nButtons/X = !`Button Y` & `Button B`\n";
    const DolphinResult r = dolphin_read(xinput, "");
    CHECK(r.ok);
    const Config& c = r.profile.config;
    CHECK(c.mask[A] == 1u << 0 && c.mask[B] == 1u << 2 && c.mask[Z] == ((1u << 10) | (1u << 4)) && c.mask[Start] == 1u << 6);
    CHECK(c.mask[L] == kSrcLT && c.mask[R] == kSrcRT && c.mask[LAnalog] == kSrcLT && c.mask[RAnalog] == kSrcRT);
    CHECK(c.mask[DUp] == 1u << 11 && c.mask[StickMod] == 1u << 7);
    CHECK(c.mask[SUp] == 0 && !r.profile.swap_sticks);
    CHECK((c.invert & kInvertCY) && !(c.invert & kInvertStickY));
    CHECK(r.profile.trigger_press == 85 && r.profile.stick_deadzone == 12);
    CHECK(r.skipped == 1);   // Buttons/X: a modifier expression
    const std::string pads =
        "[GCPad1]\nDevice = SDL/0/Nintendo Switch Pro Controller\nButtons/A = `Button E`\nMain Stick/Up = `Right Y+`\nMain Stick/Down = `Right Y-`\n"
        "C-Stick/Up = `Left Y+`\nC-Stick/Down = `Left Y-`\nMain Stick/Left = `Pad W`\n[GCPad2]\nDevice = DInput/0/Keyboard Mouse\nButtons/A = X\n";
    const DolphinResult s = dolphin_read(pads, "GCPad1");
    CHECK(s.ok && s.profile.config.mask[A] == 1u << 1 && s.profile.swap_sticks && s.profile.config.mask[SLeft] == 1u << 13);
    const DolphinResult k = dolphin_read(pads, "GCPad2");
    CHECK(!k.ok && k.message.find("Keyboard") != std::string::npos);
    const std::string folder = tmp + "/Dolphin";
    mkdir(folder.c_str(), 0777);
    { std::ofstream f(folder + "/GCPadNew.ini"); f << pads; }
    { std::ofstream f(folder + "/Melee.ini"); f << xinput; }
    const auto list = dolphin_list(folder);
    CHECK(list.size() == 3 && list[0].section == "GCPad1" && list[2].name == "Melee");
    CHECK(dolphin_read_entry(list[2]).ok);
  }

  // A USB keyboard (Kind::Keyboard, usb_keyboard.cpp): upstream's default_key_bindings in virtual-key codes, Shift walks, the
  // box rule with Mod X bound, and a "keyboard" profile in upstream's format.
  {
    const Config kb = Config::defaults(Kind::Keyboard);
    CHECK(kb.mask[A] == 'Z' && kb.mask[B] == 'X' && kb.mask[Start] == 0x0D && kb.mask[L] == 'Q' && kb.mask[SLeft] == 0x25 && kb.mask[StickMod] == 0x10);
    uint8_t keys[32] = {};
    auto key = [&](int vk, bool on) { if (on) keys[vk >> 3] |= uint8_t(1u << (vk & 7)); else keys[vk >> 3] &= uint8_t(~(1u << (vk & 7))); };
    auto press = [&](const Config& c) { State st; Raw raw; raw.keys = keys; Pad p = neutral(); apply(c, raw, 0, 0, st, p); return p; };
    key('Z', true); key('Q', true);
    Pad p = press(kb); CHECK(p.button == (0x0100 | 0x0040) && p.trig_l == 255);
    key('Z', false); key('Q', false); key(0x27, true);
    p = press(kb); CHECK(p.stick_x == 127 && p.stick_y == 0);
    key(0x10, true);
    p = press(kb); CHECK(p.stick_x == 64);   // half a push, upstream's stick modifier default
    key(0x10, false); key(0x27, false); key('I', true);
    p = press(kb); CHECK(p.sub_y == 127 && p.button == 0);
    key('I', false);
    Config moved = kb; moved.mask[A] = 'N';
    key('Z', true); p = press(moved); CHECK(p.button == 0);
    key('Z', false); key('N', true); p = press(moved); CHECK(p.button == 0x0100);
    key('N', false);
    Config boxed = kb; boxed.mask[ModX] = 'M';
    key(0x27, true); key('M', true); p = press(boxed); CHECK(p.stick_x == 53);   // B0XX Mod X horizontal
    key(0x27, false); key('M', false);
    CHECK(moved.serialize() == "m0=78" && Config::parse(Kind::Keyboard, "m0=78").same_as(moved));
    Profile prof; prof.config = moved;
    CHECK(profile_save(tmp, "Keyboard 1", prof));
    Profile back; back.config = Config::defaults(Kind::Keyboard);
    CHECK(profile_load(tmp, "Keyboard 1", back) && back.config.mask[A] == 'N' && back.config.mask[B] == 'X');
    { std::ifstream f(tmp + "/Keyboard 1.profile"); std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); CHECK(all.find("device keyboard") != std::string::npos && all.find("A 78") != std::string::npos); }
    Profile wrong; wrong.config = Config::defaults(Kind::Sdl);
    CHECK(!profile_load(tmp, "Keyboard 1", wrong));   // never another kind's file
  }

  std::printf("controls_check: %d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}
