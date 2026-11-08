// See gc_diagram.h. The controller itself is artwork (port/app/art/controller: ControllerOverlays by Kat21,
// GPL-3.0), rendered in layers so pressed shoulders show under the body and the stick caps can move. Everything
// drawn on top of it uses coordinates measured in the artwork's own units (3828 x 2689).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gc_diagram.h"
#include <CoreFoundation/CoreFoundation.h>
#include <ImageIO/ImageIO.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace host {
namespace {
constexpr CGFloat kW = 3828, kH = 2689;
struct Fit {
  CGFloat s, ox, oy;
  CGFloat x(CGFloat v) const { return ox + v * s; }
  CGFloat y(CGFloat v) const { return oy + v * s; }
  CGFloat d(CGFloat v) const { return v * s; }
  CGPoint design(CGPoint p) const { return CGPointMake((p.x - ox) / s, (p.y - oy) / s); }
};
Fit fit(CGRect b) {
  const CGFloat s = std::min(b.size.width / kW, b.size.height / kH);
  return {s, b.origin.x + (b.size.width - kW * s) / 2, b.origin.y + (b.size.height - kH * s) / 2};
}
struct Box {
  CGFloat x0, y0, x1, y1;
  bool contains(CGPoint p) const { return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1; }
};
struct Circle {
  CGFloat x, y, r;
  bool contains(CGPoint p) const { return std::hypot(p.x - x, p.y - y) <= r; }
};

// Measured from gamecube_indigo.svg.
constexpr Circle kA{3097, 984, 224}, kB{2658, 1216, 131}, kStart{1915, 1160, 83};
constexpr Box kX{3357, 638, 3613, 1068}, kY{2750, 479, 3178, 737}, kZ{2759, 188, 3566, 531};
constexpr Circle kL{705, 476, 395}, kR{3133, 477, 395};
constexpr Circle kStickGate{755, 1002, 299}, kCStickGate{2514, 1850, 299};
constexpr Circle kStickCap{754, 1005, 250}, kCStickCap{2513, 1856, 145};
constexpr CGFloat kStickTravel = 140, kCStickTravel = 115;   // caps stay inside their gates
constexpr Box kDUp{1243, 1627, 1391, 1789}, kDDown{1243, 1930, 1391, 2092}, kDLeft{1084, 1786, 1245, 1931}, kDRight{1389, 1789, 1550, 1928};
constexpr CGFloat kShoulderVisibleBottom = 760;   // below this the body covers L, R and Z

// ---- artwork
struct Artwork { CGImageRef l = nullptr, r = nullptr, z = nullptr, body = nullptr, stick = nullptr, cstick = nullptr; bool tried = false; };
Artwork g_art;
CGImageRef load_png(const std::string& path) {
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)path.c_str(), (CFIndex)path.size(), false);
  if (!url) return nullptr;
  CGImageSourceRef source = CGImageSourceCreateWithURL(url, nullptr);
  CFRelease(url);
  if (!source) return nullptr;
  CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
  CFRelease(source);
  return image;
}
std::string resources_dir() {
  std::string dir;
  if (CFBundleRef bundle = CFBundleGetMainBundle()) {
    if (CFURLRef url = CFBundleCopyResourcesDirectoryURL(bundle)) {
      char buf[4096];
      if (CFURLGetFileSystemRepresentation(url, true, (UInt8*)buf, sizeof buf)) dir = buf;
      CFRelease(url);
    }
  }
  return dir;
}
const Artwork& artwork() {
  if (g_art.tried) return g_art;
  g_art.tried = true;
  std::vector<std::string> dirs;
  if (const char* e = std::getenv("MELEE_CONTROLLER_ART")) dirs.push_back(e);
  const std::string res = resources_dir();
  if (!res.empty()) dirs.push_back(res + "/controller");
#ifdef MELEE_SOURCE_ART_DIR
  dirs.push_back(MELEE_SOURCE_ART_DIR);   // running the bare development binary
#endif
  for (const std::string& d : dirs) {
    CGImageRef body = load_png(d + "/gc_body.png");
    if (!body) continue;
    g_art.body = body;
    g_art.l = load_png(d + "/gc_l.png");
    g_art.r = load_png(d + "/gc_r.png");
    g_art.z = load_png(d + "/gc_z.png");
    g_art.stick = load_png(d + "/gc_stick.png");
    g_art.cstick = load_png(d + "/gc_cstick.png");
    break;
  }
  return g_art;
}
// Top-down contexts (flipped NSView, UIView) draw CGImages upside down unless the CTM is flipped for the image.
void draw_image(CGContextRef ctx, CGImageRef image, CGRect r) {
  if (!image) return;
  CGContextSaveGState(ctx);
  CGContextTranslateCTM(ctx, r.origin.x, r.origin.y + r.size.height);
  CGContextScaleCTM(ctx, 1, -1);
  CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
  CGContextDrawImage(ctx, CGRectMake(0, 0, r.size.width, r.size.height), image);
  CGContextRestoreGState(ctx);
}

// ---- drawing helpers
CGColorRef rgba(CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
  static CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  const CGFloat c[4] = {r, g, b, a};
  return CGColorCreate(space, c);
}
void set_fill(CGContextRef ctx, CGFloat r, CGFloat g, CGFloat b, CGFloat a) { CGColorRef c = rgba(r, g, b, a); CGContextSetFillColorWithColor(ctx, c); CGColorRelease(c); }
void set_stroke(CGContextRef ctx, CGFloat r, CGFloat g, CGFloat b, CGFloat a) { CGColorRef c = rgba(r, g, b, a); CGContextSetStrokeColorWithColor(ctx, c); CGColorRelease(c); }
void add_circle(CGContextRef ctx, const Fit& f, const Circle& c) { CGContextAddEllipseInRect(ctx, CGRectMake(f.x(c.x - c.r), f.y(c.y - c.r), f.d(2 * c.r), f.d(2 * c.r))); }
void add_box(CGContextRef ctx, const Fit& f, const Box& b, CGFloat radius) {
  const CGRect r = CGRectMake(f.x(b.x0), f.y(b.y0), f.d(b.x1 - b.x0), f.d(b.y1 - b.y0));
  const CGFloat k = std::min({f.d(radius), r.size.width / 2, r.size.height / 2});
  CGPathRef p = CGPathCreateWithRoundedRect(r, k, k, nullptr);
  CGContextAddPath(ctx, p); CGPathRelease(p);
}
CGPoint direction_point(int part) {
  const bool c = part >= DP_CSTICK_UP;
  const Circle g = c ? kCStickGate : kStickGate;
  const CGFloat off = g.r + (c ? 105 : 175);   // clear of the cap at full travel
  switch ((part - DP_STICK_UP) % 4) {
    case 0: return CGPointMake(g.x, g.y - off);
    case 1: return CGPointMake(g.x, g.y + off);
    case 2: return CGPointMake(g.x - off, g.y);
    default: return CGPointMake(g.x + off, g.y);
  }
}
// The outline of a part, added to the current path.
void add_part(CGContextRef ctx, const Fit& f, int part) {
  switch (part) {
    case DP_A: add_circle(ctx, f, {kA.x, kA.y, kA.r + 10}); break;
    case DP_B: add_circle(ctx, f, {kB.x, kB.y, kB.r + 10}); break;
    case DP_X: add_box(ctx, f, {kX.x0 - 8, kX.y0 - 8, kX.x1 + 8, kX.y1 + 8}, 130); break;
    case DP_Y: add_box(ctx, f, {kY.x0 - 8, kY.y0 - 8, kY.x1 + 8, kY.y1 + 8}, 130); break;
    case DP_Z: add_box(ctx, f, kZ, 110); break;
    case DP_L: add_circle(ctx, f, kL); break;
    case DP_R: add_circle(ctx, f, kR); break;
    case DP_START: add_circle(ctx, f, {kStart.x, kStart.y, kStart.r + 10}); break;
    case DP_DUP: add_box(ctx, f, kDUp, 24); break;
    case DP_DDOWN: add_box(ctx, f, kDDown, 24); break;
    case DP_DLEFT: add_box(ctx, f, kDLeft, 24); break;
    case DP_DRIGHT: add_box(ctx, f, kDRight, 24); break;
    default:
      if (part >= DP_STICK_UP && part <= DP_CSTICK_RIGHT) { const CGPoint p = direction_point(part); add_circle(ctx, f, {p.x, p.y, 95}); }
      break;
  }
}
// Pressed: a yellow wash and glow. Being assigned: a pulsing white ring.
void highlight(CGContextRef ctx, const Fit& f, const DiagramState& s, int part) {
  const bool pressed = part >= 0 && part < DP_COUNT && s.pressed[part];
  if (pressed) {
    CGContextSaveGState(ctx);
    set_fill(ctx, 0.97, 0.79, 0.28, 0.42); add_part(ctx, f, part); CGContextFillPath(ctx);
    CGColorRef glow = rgba(0.97, 0.79, 0.28, 1);
    CGContextSetShadowWithColor(ctx, CGSizeZero, f.d(60), glow); CGColorRelease(glow);
    set_stroke(ctx, 0.97, 0.79, 0.28, 1); CGContextSetLineWidth(ctx, f.d(18));
    add_part(ctx, f, part); CGContextStrokePath(ctx);
    CGContextRestoreGState(ctx);
  }
  if (s.selected == part) {
    CGContextSaveGState(ctx);
    set_stroke(ctx, 1, 1, 1, 0.45 + 0.55 * s.pulse); CGContextSetLineWidth(ctx, f.d(24));
    add_part(ctx, f, part); CGContextStrokePath(ctx);
    CGContextRestoreGState(ctx);
  }
}
void deadzone_ring(CGContextRef ctx, const Fit& f, const Circle& gate, CGFloat travel, float dz) {
  if (dz <= 0) return;
  CGContextSaveGState(ctx);
  const CGFloat dash[] = {f.d(30), f.d(26)};
  CGContextSetLineDash(ctx, 0, dash, 2);
  set_stroke(ctx, 1, 1, 1, 0.55); CGContextSetLineWidth(ctx, f.d(12));
  add_circle(ctx, f, {gate.x, gate.y, travel * dz}); CGContextStrokePath(ctx);
  CGContextRestoreGState(ctx);
}
}  // namespace

void gc_diagram_draw(CGContextRef ctx, CGRect bounds, const DiagramState& s) {
  const Fit f = fit(bounds);
  const Artwork& art = artwork();
  const CGRect canvas = CGRectMake(f.x(0), f.y(0), f.d(kW), f.d(kH));
  CGContextSaveGState(ctx);

  // 1. Shoulders under the body, each tinted along its own outline: analog travel fills it, a press lights it
  //    up with a glow, and the control being assigned pulses white.
  auto shoulder = [&](CGImageRef image, int part, float amount, const Circle& fallback) {
    const bool pressed = s.pressed[part];
    CGContextSaveGState(ctx);
    if (pressed) { CGColorRef glow = rgba(0.97, 0.79, 0.28, 1); CGContextSetShadowWithColor(ctx, CGSizeZero, f.d(70), glow); CGColorRelease(glow); }
    CGContextBeginTransparencyLayer(ctx, nullptr);
    if (image) draw_image(ctx, image, canvas);
    else { set_fill(ctx, 0.93, 0.93, 0.93, 1); add_circle(ctx, f, fallback); CGContextFillPath(ctx); }
    CGContextSetBlendMode(ctx, kCGBlendModeSourceAtop);
    const CGFloat tint = pressed ? 0.8 : amount > 0.01f ? 0.2 + 0.5 * amount : 0;
    if (tint > 0) { set_fill(ctx, 0.97, 0.79, 0.28, tint); CGContextFillRect(ctx, canvas); }
    if (s.selected == part) { set_fill(ctx, 1, 1, 1, 0.25 + 0.45 * s.pulse); CGContextFillRect(ctx, canvas); }
    CGContextEndTransparencyLayer(ctx);
    CGContextRestoreGState(ctx);
  };
  shoulder(art.l, DP_L, s.trig_l, kL);
  shoulder(art.r, DP_R, s.trig_r, kR);
  shoulder(art.z, DP_Z, s.pressed[DP_Z] ? 1.0f : 0.0f, {(kZ.x0 + kZ.x1) / 2, (kZ.y0 + kZ.y1) / 2, 150});

  // 2. The body, then the live sticks on top of their gates.
  if (art.body) draw_image(ctx, art.body, canvas);
  else { set_fill(ctx, 0.27, 0.29, 0.58, 1); add_box(ctx, f, {60, 200, 3770, 2330}, 700); CGContextFillPath(ctx); }
  deadzone_ring(ctx, f, kStickGate, kStickTravel, s.stick_deadzone);
  deadzone_ring(ctx, f, kCStickGate, kCStickTravel, s.cstick_deadzone);
  const CGFloat sx = std::clamp(s.stick_x, -1.0f, 1.0f) * kStickTravel, sy = -std::clamp(s.stick_y, -1.0f, 1.0f) * kStickTravel;
  const CGFloat cx = std::clamp(s.cstick_x, -1.0f, 1.0f) * kCStickTravel, cy = -std::clamp(s.cstick_y, -1.0f, 1.0f) * kCStickTravel;
  if (art.stick) draw_image(ctx, art.stick, CGRectOffset(canvas, f.d(sx), f.d(sy)));
  else { set_fill(ctx, 0.85, 0.85, 0.88, 1); add_circle(ctx, f, {kStickCap.x + sx, kStickCap.y + sy, kStickCap.r}); CGContextFillPath(ctx); }
  if (art.cstick) draw_image(ctx, art.cstick, CGRectOffset(canvas, f.d(cx), f.d(cy)));
  else { set_fill(ctx, 0.98, 0.88, 0.10, 1); add_circle(ctx, f, {kCStickCap.x + cx, kCStickCap.y + cy, kCStickCap.r}); CGContextFillPath(ctx); }

  // 3. Buttons.
  for (int part : {DP_A, DP_B, DP_X, DP_Y, DP_START, DP_DUP, DP_DDOWN, DP_DLEFT, DP_DRIGHT}) highlight(ctx, f, s, part);

  // 4. Keyboard: direction targets around both sticks.
  if (s.directions) {
    for (int part = DP_STICK_UP; part <= DP_CSTICK_RIGHT; ++part) {
      const CGPoint p = direction_point(part);
      const bool c = part >= DP_CSTICK_UP;
      if (c) set_fill(ctx, 0.98, 0.88, 0.10, 0.95); else set_fill(ctx, 0.93, 0.93, 0.95, 0.95);
      const int dir = (part - DP_STICK_UP) % 4;
      const CGFloat k = 62;
      CGPoint tip, b1, b2;
      if (dir == 0) { tip = {p.x, p.y - k}; b1 = {p.x - k, p.y + k * 0.6}; b2 = {p.x + k, p.y + k * 0.6}; }
      else if (dir == 1) { tip = {p.x, p.y + k}; b1 = {p.x - k, p.y - k * 0.6}; b2 = {p.x + k, p.y - k * 0.6}; }
      else if (dir == 2) { tip = {p.x - k, p.y}; b1 = {p.x + k * 0.6, p.y - k}; b2 = {p.x + k * 0.6, p.y + k}; }
      else { tip = {p.x + k, p.y}; b1 = {p.x - k * 0.6, p.y - k}; b2 = {p.x - k * 0.6, p.y + k}; }
      CGContextMoveToPoint(ctx, f.x(tip.x), f.y(tip.y)); CGContextAddLineToPoint(ctx, f.x(b1.x), f.y(b1.y)); CGContextAddLineToPoint(ctx, f.x(b2.x), f.y(b2.y)); CGContextClosePath(ctx);
      CGContextFillPath(ctx);
      highlight(ctx, f, s, part);
    }
  }
  CGContextRestoreGState(ctx);
}

int gc_diagram_hit(CGRect bounds, CGPoint point, bool directions) {
  const Fit f = fit(bounds);
  const CGPoint p = f.design(point);
  if (directions)
    for (int part = DP_STICK_UP; part <= DP_CSTICK_RIGHT; ++part) {
      const CGPoint d = direction_point(part);
      if (std::hypot(p.x - d.x, p.y - d.y) <= 110) return part;
    }
  if (Circle{kStart.x, kStart.y, kStart.r + 40}.contains(p)) return DP_START;
  if (Circle{kB.x, kB.y, kB.r + 30}.contains(p)) return DP_B;
  if (Box{kX.x0 - 30, kX.y0 - 30, kX.x1 + 30, kX.y1 + 30}.contains(p)) return DP_X;
  if (Box{kY.x0 - 30, kY.y0 - 30, kY.x1 + 30, kY.y1 + 30}.contains(p)) return DP_Y;
  if (Circle{kA.x, kA.y, kA.r + 30}.contains(p)) return DP_A;
  if (kDUp.contains(p)) return DP_DUP;
  if (kDDown.contains(p)) return DP_DDOWN;
  if (kDLeft.contains(p)) return DP_DLEFT;
  if (kDRight.contains(p)) return DP_DRIGHT;
  if (p.y < kShoulderVisibleBottom) {
    if (kZ.contains(p)) return DP_Z;
    if (kL.contains(p)) return DP_L;
    if (kR.contains(p)) return DP_R;
  }
  return DP_NONE;
}

void gc_diagram_from_pad(DiagramState& s, const ControllerMap& m, const ControllerLiveState& live) {
  float lx = live.lx, ly = live.ly, rx = live.rx, ry = live.ry;
  if (m.swap_sticks) { std::swap(lx, rx); std::swap(ly, ry); }
  s.stick_x = lx; s.stick_y = ly; s.cstick_x = rx; s.cstick_y = ry;
  s.stick_deadzone = m.stick_deadzone / 100.0f; s.cstick_deadzone = m.cstick_deadzone / 100.0f;
  s.directions = false;
  auto held = [&](int input) -> bool {
    if (input == kTriggerLeft) return live.lt * 100.0f >= (float)m.trigger_press;
    if (input == kTriggerRight) return live.rt * 100.0f >= (float)m.trigger_press;
    return input >= 0 && input < 32 && live.button[input];
  };
  auto analog = [&](int input) -> float { return input == kTriggerLeft ? live.lt : input == kTriggerRight ? live.rt : 0.0f; };
  for (int i = 0; i < DP_COUNT; ++i) s.pressed[i] = i < GC_CTL_COUNT && held(m.binding[i]);
  s.trig_l = std::max(analog(m.binding[GC_CTL_L]), s.pressed[DP_L] ? 1.0f : 0.0f);
  s.trig_r = std::max(analog(m.binding[GC_CTL_R]), s.pressed[DP_R] ? 1.0f : 0.0f);
}

void gc_diagram_from_keyboard(DiagramState& s, const KeyboardMap& k, const bool* held, int held_count) {
  auto on = [&](int control) { const int code = k.key[control]; return code > 0 && code < held_count && held[code]; };
  for (int i = 0; i < DP_COUNT; ++i) s.pressed[i] = on(i);   // parts 0..19 line up with keyboard controls 0..19
  const float amount = on(KB_MODIFIER) ? k.modifier_percent / 100.0f : 1.0f;
  s.stick_x = ((on(KB_STICK_RIGHT) ? 1.0f : 0.0f) - (on(KB_STICK_LEFT) ? 1.0f : 0.0f)) * amount;
  s.stick_y = ((on(KB_STICK_UP) ? 1.0f : 0.0f) - (on(KB_STICK_DOWN) ? 1.0f : 0.0f)) * amount;
  s.cstick_x = (on(KB_CSTICK_RIGHT) ? 1.0f : 0.0f) - (on(KB_CSTICK_LEFT) ? 1.0f : 0.0f);
  s.cstick_y = (on(KB_CSTICK_UP) ? 1.0f : 0.0f) - (on(KB_CSTICK_DOWN) ? 1.0f : 0.0f);
  s.trig_l = on(GC_CTL_L) ? 1.0f : 0.0f; s.trig_r = on(GC_CTL_R) ? 1.0f : 0.0f;
  s.stick_deadzone = s.cstick_deadzone = 0; s.directions = true;
}
}  // namespace host
