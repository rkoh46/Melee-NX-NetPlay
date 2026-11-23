// What port/runtime/gx needs from a host, on melee-unlocked's native host services. See host.h here.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "host.h"
#include "render_observer.h"
#include "host_services.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <stdexcept>

namespace ppc {
uint32_t RAM_SIZE = linux_host::mem1_bytes;
}

namespace host {
uint8_t* ram = reinterpret_cast<uint8_t*>(linux_host::mem1_base);

void die(const char* fmt, ...) {
  char message[1024];
  va_list args; va_start(args, fmt); std::vsnprintf(message, sizeof message, fmt, args); va_end(args);
  throw std::runtime_error(message);
}
uint8_t* ptr(uint32_t addr, uint32_t bytes) {
  uint8_t* p = linux_host::memory_pointer(addr, bytes);
  if (!p) die("GX address outside game memory: %08X+%X", addr, bytes);
  return p;
}
uint8_t rd8(uint32_t addr) { return *ptr(addr, 1); }
void native_scene(uint8_t* major, uint8_t* minor) {
  MuStatePod state{};
  if (!linux_host::state_snapshot(&state)) { *major = 0; *minor = 0; return; }
  *major = uint8_t(state.scene_major); *minor = uint8_t(state.scene);
}
double now_seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void sim_cost_add(int, double) {}
}  // namespace host

// Draw identities, authored poses and owner tints belong to the recompiled runtime's other renderers.
namespace gx {
void set_render_observe(bool) {}
uint64_t observed_draw_identity(uint64_t fallback, uint64_t& generation) { generation = 0; return fallback; }
void finish_observed_frame() {}
void set_authored_capture(bool) {}
std::shared_ptr<const AuthoredPose> capture_authored_pose() { return {}; }
void set_owner_tracking(bool) {}
uint8_t observed_owner() { return 0xFF; }
bool observed_skinned() { return false; }
}  // namespace gx
