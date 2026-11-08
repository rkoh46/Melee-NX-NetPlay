// The GameCube controller in the controller editors on macOS and iOS: artwork (port/app/art/controller) with
// every button lighting up, both sticks moving with deadzone rings, analog triggers filling, and a pulsing ring
// on the control being assigned. Shared by the AppKit and UIKit views; coordinates are
// top-down (flipped NSView, UIView).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <CoreGraphics/CoreGraphics.h>
#include "input_config.h"

namespace host {
// Parts 0..11 match GcControl; 12..19 are stick directions (keyboard bindings only).
enum DiagramPart : int {
  DP_NONE = -1,
  DP_A = 0, DP_B, DP_X, DP_Y, DP_Z, DP_L, DP_R, DP_START, DP_DUP, DP_DDOWN, DP_DLEFT, DP_DRIGHT,
  DP_STICK_UP, DP_STICK_DOWN, DP_STICK_LEFT, DP_STICK_RIGHT, DP_CSTICK_UP, DP_CSTICK_DOWN, DP_CSTICK_LEFT, DP_CSTICK_RIGHT,
  DP_COUNT
};
struct DiagramState {
  bool pressed[DP_COUNT] = {};
  float stick_x = 0, stick_y = 0, cstick_x = 0, cstick_y = 0;   // -1..1, y up
  float trig_l = 0, trig_r = 0;                                  // 0..1
  float stick_deadzone = 0, cstick_deadzone = 0;                 // 0..1 of full deflection, 0 hides the ring
  int selected = DP_NONE;                                        // being assigned: pulses
  float pulse = 0;                                               // 0..1 animation phase
  bool directions = false;                                       // draw stick direction targets (keyboard)
};
void gc_diagram_draw(CGContextRef ctx, CGRect bounds, const DiagramState& state);
int gc_diagram_hit(CGRect bounds, CGPoint point, bool directions);
constexpr double kDiagramAspect = 3828.0 / 2689.0;   // the artwork's canvas
// Fill the live parts of a state from a controller (its mapping decides what counts as pressed) or the keyboard.
void gc_diagram_from_pad(DiagramState& state, const ControllerMap& map, const ControllerLiveState& live);
void gc_diagram_from_keyboard(DiagramState& state, const KeyboardMap& map, const bool* held, int held_count);
}  // namespace host
