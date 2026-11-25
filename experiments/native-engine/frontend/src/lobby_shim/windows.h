// The few Win32 names melee-unlocked's peer lobby (port/app/launcher_lobby_p2p.cpp, compiled from the fetched tree by
// build.sh sync -> gen/lobby) and its peer-to-peer match (port/runtime/mu_net, compiled into the netplay object by
// ../build.sh) use, on POSIX sockets, Linux and the Switch's libnx. Nothing else of Win32 is here: a name
// the lobby starts to use upstream fails the build instead of doing something unexpected.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

typedef unsigned long long ULONGLONG;
typedef unsigned long ULONG;
typedef unsigned char UCHAR;
typedef UCHAR* PUCHAR;
typedef unsigned short WORD;
typedef unsigned short u_short;
typedef unsigned long u_long;

// GetTickCount64: milliseconds of a steady clock (lobby_shim.cpp).
ULONGLONG GetTickCount64();

struct SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; };
void GetLocalTime(SYSTEMTIME* out);   // lobby_shim.cpp: the local wall clock

// Text conversion as mu_net's rollback_session.cpp uses it for the names in the game's match state (lobby_shim.cpp): UTF-8 to
// UTF-16, and UTF-16 to Shift-JIS (code page 932) for ASCII and its full-width forms; any other character becomes '?'.
#define CP_UTF8 65001
int MultiByteToWideChar(unsigned code_page, unsigned long flags, const char* in, int in_size, wchar_t* out, int out_size);
int WideCharToMultiByte(unsigned code_page, unsigned long flags, const wchar_t* in, int in_size, char* out, int out_size,
                        const char* default_char, int* used_default);
