// The few host services melee-unlocked's phillip_ai.cpp and practice_menu.cpp call, for the frontend's build of them.
// host::log is the host services' own (melee-unlocked's host_services.cpp, the frontend's log), now_ns the frontend's
// clock (src/shim/shim.cpp; now_seconds as src/shim/host.h has it), dol_data_read features/proai.cpp's.
// Not melee-unlocked's host.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace host {
void log(const char* fmt, ...);
uint64_t now_ns();
inline double now_seconds() { return double(now_ns()) * 1e-9; }
bool dol_data_read(uint32_t address, void* dst, uint32_t size);   // bytes of the disc's main.dol at a console address
}  // namespace host
