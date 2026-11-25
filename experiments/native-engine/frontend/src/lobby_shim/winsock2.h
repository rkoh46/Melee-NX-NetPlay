// Winsock names for melee-unlocked's peer lobby on POSIX sockets (see windows.h beside this file).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "windows.h"

typedef int SOCKET;
#define INVALID_SOCKET (-1)
inline int closesocket(SOCKET s) { return ::close(s); }
// The lobby's one use: FIONBIO on its UDP socket. fcntl works on Linux and on libnx's BSD sockets alike.
#ifndef FIONBIO
#define FIONBIO 0x5421
#endif
inline int ioctlsocket(SOCKET s, long command, u_long* value) {
  if (command != FIONBIO || !value) return -1;
  const int flags = ::fcntl(s, F_GETFL, 0);
  if (flags < 0) return -1;
  return ::fcntl(s, F_SETFL, *value ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
