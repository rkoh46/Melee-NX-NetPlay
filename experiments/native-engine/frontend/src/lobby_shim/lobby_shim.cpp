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

int MultiByteToWideChar(unsigned, unsigned long, const char* in, int in_size, wchar_t* out, int out_size) {
  int n = 0;
  for (int i = 0; i < in_size && n < out_size;) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    const int length = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!length || i + length > in_size) return 0;
    unsigned cp = length == 1 ? c : length == 2 ? (c & 0x1Fu) : length == 3 ? (c & 0x0Fu) : (c & 0x07u);
    for (int k = 1; k < length; ++k) cp = (cp << 6) | (static_cast<unsigned char>(in[i + k]) & 0x3Fu);
    i += length;
    if (cp >= 0x10000) {   // UTF-16, as Windows gives it
      if (n + 2 > out_size) break;
      cp -= 0x10000; out[n++] = wchar_t(0xD800 + (cp >> 10)); out[n++] = wchar_t(0xDC00 + (cp & 0x3FF));
    } else out[n++] = wchar_t(cp);
  }
  return n;
}
int WideCharToMultiByte(unsigned, unsigned long, const wchar_t* in, int in_size, char* out, int out_size, const char*, int*) {
  // Shift-JIS row 0x81 for the full-width punctuation U+FF01..U+FF5E (zero: '?'); digits and letters follow their own rows.
  static const unsigned short punct[0x5F] = {
    0, 0x8149, 0x8168, 0x8194, 0x8190, 0x8193, 0x8195, 0x8166, 0x8169, 0x816A, 0x8196, 0x817B, 0x8143, 0x817C, 0x8144, 0x815E,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x8146, 0x8147, 0x8183, 0x8181, 0x8184, 0x8148,
    0x8197, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x816D, 0x818F, 0x816E, 0x814F, 0x8151,
    0x814D, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x816F, 0x8162, 0x8170, 0x8160};
  int n = 0;
  for (int i = 0; i < in_size; ++i) {
    const unsigned w = unsigned(in[i]);
    unsigned code = 0;
    if (w < 0x80) code = w;
    else if (w >= 0xFF10 && w <= 0xFF19) code = 0x824F + (w - 0xFF10);
    else if (w >= 0xFF21 && w <= 0xFF3A) code = 0x8260 + (w - 0xFF21);
    else if (w >= 0xFF41 && w <= 0xFF5A) code = 0x8281 + (w - 0xFF41);
    else if (w >= 0xFF01 && w <= 0xFF5E) code = punct[w - 0xFF00];
    else if (w == 0x3000) code = 0x8140;
    else if (w == 0x201D) code = 0x8168;
    else if (w == 0x2019) code = 0x8166;
    if (!code) code = '?';
    const int bytes = code > 0xFF ? 2 : 1;
    if (n + bytes > out_size) break;
    if (bytes == 2) out[n++] = char(code >> 8);
    out[n++] = char(code & 0xFF);
  }
  return n;
}
