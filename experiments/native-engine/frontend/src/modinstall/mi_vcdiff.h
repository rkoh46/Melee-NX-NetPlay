// A VCDIFF (RFC 3284) decoder for the patches xdelta3 writes, so the mods' .xdelta files are applied by this
// program instead of by the xdelta3 executables they ship. It reads the patch as a stream and the source disc
// through a few cached blocks, holds one target window at a time (xdelta3 writes windows of at most 16 MiB) and
// writes the result as it goes: the 1.4 GB disc is never in memory.
//   - the default code table and address cache of RFC 3284; source windows (VCD_SOURCE);
//   - xdelta3's extensions: the application header, the per-window Adler-32 (checked), and secondary compression
//     of the data, instruction and address sections with DJW (static Huffman, id 1) or LZMA (id 2); FGK (id 16),
//     custom code tables and target-window copies (VCD_TARGET, which xdelta3 does not write either) are refused.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "mi_io.h"

#include <functional>
#include <string>

namespace modinstall {

class OutputSink {
 public:
  virtual ~OutputSink() = default;
  virtual bool write(const uint8_t* data, size_t size) = 0;
};

struct VcdiffInfo {
  int secondary = 0;          // 0 none, 1 DJW, 2 LZMA
  uint64_t windows = 0, target_bytes = 0;
};

// Applies `patch` to `source`, writing to `out`. `progress(patch bytes read, patch total)` returns false to cancel.
bool vcdiff_decode(ByteSource& patch, RandomFile& source, OutputSink& out,
                   const std::function<bool(uint64_t, uint64_t)>& progress, std::string* error, VcdiffInfo* info = nullptr);

}  // namespace modinstall
