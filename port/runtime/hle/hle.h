// Helpers for host implementations of guest (SDK) functions.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ppc.h"
#include "host.h"
#include "hle_decls.h"
#include "guest_symbols.h"

// Declares and defines hle::name; names absent from the symbol map are harmless dead code.
#define HLE(name) \
  namespace hle { void name(ppc::Context& c, uint8_t* m); } \
  void hle::name(ppc::Context& c, uint8_t* m)
#define ARG0 (c.r[3])
#define ARG1 (c.r[4])
#define ARG2 (c.r[5])
#define ARG3 (c.r[6])
#define ARG4 (c.r[7])
#define ARG5 (c.r[8])
#define ARG6 (c.r[9])
#define ARG7 (c.r[10])
#define RET(v) (c.r[3] = (uint32_t)(v))
#define TRACE(...) do { if (host::options.trace_calls) host::log(__VA_ARGS__); } while (0)
