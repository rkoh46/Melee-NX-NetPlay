// OS-level HLE: console output, thread waits, contexts, exit.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "hle.h"
#include <cstring>
#include <string>

// int __write_console(u32 handle, u8* buf, u32* count, void (*idle)(void))
HLE(__write_console) {
  uint32_t buf = ARG1, count_ptr = ARG2;
  uint32_t n = host::rd32(count_ptr);
  std::string s;
  for (uint32_t i = 0; i < n; ++i) s += (char)host::rd8(buf + i);
  host::log_guest_text(s.data(), s.size());
  RET(0);
}
HLE(__read_console) { host::wr32(ARG2, 0); RET(0); }
HLE(exit) {
  host::log("guest called exit(%d)", (int)ARG0);
  host::request_exit((int)ARG0);
  throw ExitRequested{(int)ARG0};
}
HLE(OSResetSystem) {
  host::log("OSResetSystem(%u, %u, %u)", ARG0, ARG1, ARG2);
  host::request_exit(0);
  throw ExitRequested{0};
}
HLE(OSPanic) {
  std::string file = host::cstr(ARG0), msg = host::cstr(ARG2);
  host::die("OSPanic at %s:%u: %s", file.c_str(), ARG1, msg.c_str());
}
HLE(__OSUnhandledException) {
  host::die("__OSUnhandledException %u (context %08X dsisr %08X dar %08X)", ARG0, ARG1, ARG2, ARG3);
}
HLE(__OSInitAudioSystem) {}
HLE(__OSStopAudioSystem) {}

// Threads: Melee runs one thread. Sleeping means "wait for the next hardware event".
HLE(OSSleepThread) { host::wait_event(); }
HLE(OSWakeupThread) {}
HLE(OSYieldThread) {}
HLE(__OSReschedule) {}
HLE(OSCreateThread) { host::log("OSCreateThread ignored (entry %08X)", ARG1); RET(0); }
HLE(OSResumeThread) { RET(0); }
HLE(OSSuspendThread) { RET(0); }
HLE(OSJoinThread) { RET(0); }

// OSLoadContext never returns on hardware; unwind to the host interrupt dispatcher.
HLE(OSLoadContext) { throw LoadContextUnwind{ARG0}; }
