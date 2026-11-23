// Stand-in for port/runtime/ppc/ppc.h when the GX decoder is built for the native engine: there is no
// translated PowerPC code, only the size of the memory GX addresses may point into.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace ppc {
// Bytes from 0x80000000 that GX data may live in: main memory (40 MiB here) and, directly after it, the
// game image at 0x82800000 with its static data. Set when the frontend attaches (shim.cpp).
extern uint32_t RAM_SIZE;
}  // namespace ppc
