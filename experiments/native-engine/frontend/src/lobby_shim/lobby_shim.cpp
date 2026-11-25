// The Win32 calls of windows.h and bcrypt.h beside this file, for melee-unlocked's peer lobby.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows.h"
#include "bcrypt.h"

#include <chrono>
#include <cerrno>
#ifdef __SWITCH__
#include <switch.h>
#else
#include <sys/random.h>
#endif

ULONGLONG GetTickCount64() {
  return ULONGLONG(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void GetLocalTime(SYSTEMTIME* out) {
  timeval tv{}; gettimeofday(&tv, nullptr);
  const time_t seconds = tv.tv_sec;
  std::tm local{};
  localtime_r(&seconds, &local);
  out->wYear = WORD(local.tm_year + 1900); out->wMonth = WORD(local.tm_mon + 1); out->wDayOfWeek = WORD(local.tm_wday);
  out->wDay = WORD(local.tm_mday); out->wHour = WORD(local.tm_hour); out->wMinute = WORD(local.tm_min); out->wSecond = WORD(local.tm_sec);
  out->wMilliseconds = WORD(tv.tv_usec / 1000);
}

long BCryptGenRandom(void*, PUCHAR buffer, ULONG size, ULONG) {
#ifdef __SWITCH__
  randomGet(buffer, size);   // the kernel's random source (svcGetInfo entropy, ChaCha-expanded by libnx)
  return 0;
#else
  for (ULONG done = 0; done < size;) {
    const ssize_t got = getrandom(buffer + done, size - done, 0);
    if (got < 0) { if (errno == EINTR) continue; return -1; }
    done += ULONG(got);
  }
  return 0;
#endif
}
