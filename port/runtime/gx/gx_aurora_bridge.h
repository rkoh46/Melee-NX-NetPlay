// Pure, owned conversion at the decoded Frame seam. No guest pointers or GPU calls.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "gx_core.h"
#include <array>
#include <map>
#include <stdexcept>
#include <string>

namespace gx::aurora_bridge {
struct Extent {
  uint32_t width = 640, height = 480;
};

class CoverageError : public std::runtime_error {
public:
  explicit CoverageError(const std::string& message) : std::runtime_error(message) {}
};

struct Draw {
  // Reconstructed semantic GX state and canonical direct-attribute vertex data,
  // in documented GX big-endian wire order. Never the original guest FIFO.
  std::vector<uint8_t> state, vertices;
  std::array<TextureRef, 8> textures;
  uint8_t primitive = 0;
  uint16_t vertex_count = 0;
  uint32_t vertex_stride = 0;
};

struct PreparedFrame {
  std::vector<Draw> draws;
  std::vector<EfbCopy> copies;
  std::vector<FrameCommand> commands;
  std::vector<size_t> skipped_copies;   // copies the bridge could not validate; the renderer ignores them
  uint64_t sequence = 0;
};

// Validates the whole frame before returning any work. Throws CoverageError for
// an invalid or unsupported capture. Texture bytes remain shared immutable data.
// Baseline: one final full-size, full-clear XFB; no unlocked/repeated subframes.
PreparedFrame prepare_frame(const Frame& frame, Extent extent = {});

struct CopyRecord {
  uint32_t width = 0, height = 0, bytes = 0;
  bool xfb = false;
  std::vector<uint8_t> destination_bytes;
};
using CopyHistory = std::map<uint32_t, CopyRecord>;
// Side-effect-free cross-frame alias validation, used by the actual adapter.
CopyHistory advance_copy_history(const PreparedFrame& frame, CopyHistory history);
} // namespace gx::aurora_bridge
