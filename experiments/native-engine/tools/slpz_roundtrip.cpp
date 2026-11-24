// .slpz round trip: every .slp given is compressed with the fetched tree's port/app/replay_slpz.cpp (the code play mode
// writes .slpz with, compressreplays 1) and unpacked again; the result must be byte-identical to the .slp.
//   build (tools/slpz_roundtrip.sh does it in a container):
//     g++ -std=gnu++17 -O2 -I<shim with zstd/zstd.h -> <zstd.h>> slpz_roundtrip.cpp -lzstd
//   slpz_roundtrip [--keep DIR] FILE.slp...
// Prints one line per file and "slpz round trip: N of N identical".
// SPDX-License-Identifier: GPL-3.0-or-later
#include "replay_slpz.h"
#include "replay_slpz.cpp"
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
  std::string keep;
  int files = 0, same = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--keep" && i + 1 < argc) { keep = argv[++i]; continue; }
    ++files;
    std::vector<uint8_t> slp, packed, back;
    FILE* f = std::fopen(arg.c_str(), "rb");
    if (!f) { std::printf("%s: cannot read\n", arg.c_str()); continue; }
    uint8_t block[65536]; size_t n;
    while ((n = std::fread(block, 1, sizeof block, f)) > 0) slp.insert(slp.end(), block, block + n);
    std::fclose(f);
    if (!slpz::compress(slp, packed)) { std::printf("%s: not a complete .slp (%zu bytes)\n", arg.c_str(), slp.size()); continue; }
    const bool header = slpz::is_slpz(packed.data(), packed.size());
    if (!slpz::decompress(packed, back)) { std::printf("%s: compressed to %zu bytes, DOES NOT UNPACK\n", arg.c_str(), packed.size()); continue; }
    const bool ok = header && back == slp;
    same += ok;
    std::printf("%s: %zu bytes -> .slpz %zu bytes (%.1fx) -> %zu bytes: %s\n", arg.c_str(), slp.size(), packed.size(),
                double(slp.size()) / double(packed.size()), back.size(), ok ? "IDENTICAL" : "DIFFERENT");
    if (!keep.empty()) {
      std::string name = arg.substr(arg.find_last_of('/') + 1);
      name = keep + "/" + name.substr(0, name.size() - 4) + ".slpz";
      if (FILE* out = std::fopen(name.c_str(), "wb")) { std::fwrite(packed.data(), 1, packed.size(), out); std::fclose(out); }
    }
  }
  std::printf("slpz round trip: %d of %d identical\n", same, files);
  return same == files && files > 0 ? 0 : 1;
}
