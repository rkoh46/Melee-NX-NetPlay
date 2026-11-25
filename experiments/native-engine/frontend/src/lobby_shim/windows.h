// The few Win32 names melee-unlocked's peer lobby (port/app/launcher_lobby_p2p.cpp, compiled from the fetched tree by
// build.sh sync -> gen/lobby) uses, on POSIX sockets, Linux and the Switch's libnx. Nothing else of Win32 is here: a name
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
