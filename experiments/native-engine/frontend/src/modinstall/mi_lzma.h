// LZMA and LZMA2 decoding through the LZMA SDK's decoders (C/LzmaDec.c, C/Lzma2Dec.c, public domain; compiled
// from the copy melee-unlocked vendors in port/third_party/lzma of the fetched tree), and the xz stream xdelta3's
// "lzma" secondary compressor writes.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace modinstall {

class LzmaStream {
 public:
  enum Kind { Lzma1, Lzma2 };
  LzmaStream(Kind kind, const uint8_t* props, size_t props_size);
  ~LzmaStream();
  bool ok() const { return ok_; }
  // Decodes from in[0..*in_size) into out[0..*out_size); both are updated to what was used and made.
  // Returns 0 to go on, 1 at the stream's end mark, -1 on damaged data.
  int decode(const uint8_t* in, size_t* in_size, uint8_t* out, size_t* out_size);
 private:
  struct State;
  std::unique_ptr<State> s_;
  Kind kind_;
  bool ok_ = false;
};

// xdelta3's secondary "lzma" (id 2): one xz stream per section kind (data, instructions, addresses) that runs on
// from window to window (liblzma's stream encoder flushed with LZMA_SYNC_FLUSH after each section, never finished).
// Each section hands its bytes here and asks for exactly the decoded size the section names.
class XzRunOn {
 public:
  XzRunOn();
  ~XzRunOn();
  // Appends the decoding of `in` to `out` until `out_size` bytes were made. False on damaged or short data.
  bool decode(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size);
 private:
  bool parse_headers(const uint8_t*& in, size_t& in_size);
  std::vector<uint8_t> head_;      // the stream and block headers, gathered until complete
  bool headers_done_ = false;
  std::unique_ptr<LzmaStream> lzma2_;
};

}  // namespace modinstall
