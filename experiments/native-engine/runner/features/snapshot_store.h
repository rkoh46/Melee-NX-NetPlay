// Kept game states for the offline features of play mode: the Training Lab's save state (commands
// 0xF2 / 0xF3) and the replay viewer's jump-back points. Both use the hot-page store the netplay host
// uses for rollback (../rollback.h): tracking starts at the first keep and ends when the scene ends, so
// the scene's heap ranges are the ones the store watches. Only one user at a time: the netplay host
// never runs in a scene where these do (offline Training, replay playback).
// Simulation thread only. `sync` is called before every copy (play mode: wait for the GX decoder, which
// reads the pages the store copies and, on the console, remaps).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>

namespace snapshot_store {
using Log = void (*)(const char* format, ...);
void configure(Log log, void (*sync)());
// Keeps the state now under `key` (starting the store with room for `slots` states when it is not running).
bool keep(int32_t key, int slots, const char* who);
// Brings back the state kept under `key`; later keys are forgotten. False when none is kept.
bool load(int32_t key, const char* who);
// The newest key at or before `target` (false when none).
bool newest_at_or_before(int32_t target, int32_t* key);
bool has(int32_t key);
bool running();
// The scene ended or the run ends: stop tracking, forget every state.
void release(const char* why);
struct Stats { uint64_t keeps = 0, loads = 0; double keep_ms_max = 0, load_ms_max = 0, keep_ms_sum = 0, load_ms_sum = 0; };
Stats stats();
}  // namespace snapshot_store
