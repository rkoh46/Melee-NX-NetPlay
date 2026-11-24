// Reading one file out of a mod's zip or 7z download as a stream, so a patch inside an archive is applied without
// being unpacked to the SD card first (ACE's patch alone is 1.3 GB). zip: stored or deflated entries (zlib's
// inflate). 7z: folders of one LZMA or LZMA2 coder, or stored (LZMA SDK's decoders, public domain), with an
// encoded header. The entry's CRC-32 is checked when its last byte has been read.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "mi_io.h"

#include <memory>
#include <string>
#include <vector>

namespace modinstall {

struct ArchiveEntry {
  std::string name;          // as stored, '/' separated
  uint64_t size = 0;         // uncompressed
  bool directory = false;
};

class Archive {
 public:
  virtual ~Archive() = default;
  virtual const std::vector<ArchiveEntry>& entries() const = 0;
  // A stream of entry `index`; null with *error set when it cannot be read.
  virtual std::unique_ptr<ByteSource> open(size_t index, std::string* error) = 0;
};

// By content: "PK\3\4" is a zip, "7z\xBC\xAF\x27\x1C" a 7z. Null with *error set otherwise.
std::unique_ptr<Archive> open_archive(const std::string& path, std::string* error);
// The first entry whose name ends with `suffix` (case-insensitive), or -1.
long find_entry(const Archive& archive, const std::string& suffix);

}  // namespace modinstall
