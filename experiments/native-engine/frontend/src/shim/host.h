// Stand-in for port/runtime/host/host.h when this repository's GX decoder (port/runtime/gx) is built on
// melee-unlocked's native host services instead of the recompiled runtime. Only what the decoder, the
// frame queue and the app's input and audio sources use. Implemented in shim.cpp, except host::log (melee-unlocked's host_services.cpp).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include "ppc.h"
#include "fatal_boundary.h"

namespace host {
extern uint8_t* ram;                       // 0x80000000
void log(const char* fmt, ...);
[[noreturn]] void die(const char* fmt, ...);   // throws; the decoder thread relays it, the frontend reports it
uint8_t* ptr(uint32_t addr, uint32_t bytes = 1);
uint8_t rd8(uint32_t addr);
// The native game raises its own draw-done and token interrupts after it has flushed the pipe.
inline void set_pe_finish_pending() {}
inline void set_pe_token_pending(uint16_t) {}
// Scene controller state from the game's own state snapshot.
void native_scene(uint8_t* major, uint8_t* minor);

// Monotonic time. On the console this reads the system counter directly: the decoder asks several
// thousand times a frame (SimCostScope around every draw record and texture snapshot).
uint64_t now_ns();
inline double now_seconds() { return double(now_ns()) * 1e-9; }
// The app's cost slots, plus SIM_DECODE: everything the decoder thread does (patches/0102).
enum SimCost { SIM_DVD, SIM_AX, SIM_JUKEBOX, SIM_EXI, SIM_SNAPSHOT, SIM_QUEUE, SIM_OBSERVE, SIM_RECORD, SIM_RENDER, SIM_TEXTURE, SIM_PUMP, SIM_GPUWAIT, SIM_DRAWABLE, SIM_SAVESTATE, SIM_DECODE, SIM_COST_COUNT };
void sim_cost_add(int slot, double seconds);
uint64_t sim_cost_ns(int slot);            // total so far; any thread
struct SimCostScope { int slot; uint64_t t0; explicit SimCostScope(int s) : slot(s), t0(now_ns()) {} ~SimCostScope(); };

// ---- what the app's input and audio sources use (input_sdl.cpp, gc_adapter_*.cpp, audio_sdl.cpp) ----
struct PadState { uint16_t button; int8_t stick_x, stick_y, sub_x, sub_y; uint8_t trig_l, trig_r, analog_a, analog_b; int8_t err; };
void input_poll(PadState out[4]);                // input_sdl.cpp: adapter, SDL gamepads through Aurora's PAD, keyboard, script
uint32_t gcadapter_poll(PadState out[4]);
struct GcAdapterStatus { uint32_t ports = 0; int interval_ms = 0; double report_hz = 0.0; };
bool gcadapter_status(GcAdapterStatus& out);
void gcadapter_rumble(int port, bool on);
void gcadapter_recalibrate(int port);
void gcadapter_shutdown();
void gamepad_rumble(int port, bool on);
uint32_t retrace_count();
void audio_session_report();
}  // namespace host
