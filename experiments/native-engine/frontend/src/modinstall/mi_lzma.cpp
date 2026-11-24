// SPDX-License-Identifier: GPL-3.0-or-later
#include "mi_lzma.h"
#include "mi_hash.h"

extern "C" {
#include "LzmaDec.h"
#include "Lzma2Dec.h"
}

#include <cstdlib>
#include <cstring>

namespace modinstall {
namespace {
void* alloc_fn(ISzAllocPtr, size_t size) { return std::malloc(size); }
void free_fn(ISzAllocPtr, void* p) { std::free(p); }
const ISzAlloc kAlloc = {alloc_fn, free_fn};
}  // namespace

struct LzmaStream::State {
  CLzmaDec lzma;
  CLzma2Dec lzma2;
};

LzmaStream::LzmaStream(Kind kind, const uint8_t* props, size_t props_size) : s_(new State), kind_(kind) {
  if (kind == Lzma1) {
    LzmaDec_Construct(&s_->lzma);
    ok_ = props_size == LZMA_PROPS_SIZE && LzmaDec_Allocate(&s_->lzma, props, LZMA_PROPS_SIZE, &kAlloc) == SZ_OK;
    if (ok_) LzmaDec_Init(&s_->lzma);
  } else {
    Lzma2Dec_Construct(&s_->lzma2);
    ok_ = props_size == 1 && props[0] <= 40 && Lzma2Dec_Allocate(&s_->lzma2, props[0], &kAlloc) == SZ_OK;
    if (ok_) Lzma2Dec_Init(&s_->lzma2);
  }
}
LzmaStream::~LzmaStream() {
  if (kind_ == Lzma1) LzmaDec_Free(&s_->lzma, &kAlloc);
  else Lzma2Dec_Free(&s_->lzma2, &kAlloc);
}
int LzmaStream::decode(const uint8_t* in, size_t* in_size, uint8_t* out, size_t* out_size) {
  SizeT src = *in_size, dst = *out_size;
  ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
  const SRes r = kind_ == Lzma1 ? LzmaDec_DecodeToBuf(&s_->lzma, out, &dst, in, &src, LZMA_FINISH_ANY, &status)
                                : Lzma2Dec_DecodeToBuf(&s_->lzma2, out, &dst, in, &src, LZMA_FINISH_ANY, &status);
  *in_size = src; *out_size = dst;
  if (r != SZ_OK) return -1;
  return status == LZMA_STATUS_FINISHED_WITH_MARK ? 1 : 0;
}

XzRunOn::XzRunOn() = default;
XzRunOn::~XzRunOn() = default;

namespace {
// A variable-length integer of the xz format; false when incomplete or too long.
bool xz_vli(const uint8_t* p, size_t n, size_t* at, uint64_t* value) {
  *value = 0;
  for (int i = 0; i < 9; ++i) {
    if (*at >= n) return false;
    const uint8_t b = p[(*at)++];
    *value |= uint64_t(b & 0x7F) << (7 * i);
    if (!(b & 0x80)) return true;
  }
  return false;
}
}  // namespace

// Stream header (12 bytes: magic, flags, CRC32), then the block header ((first byte + 1) * 4 bytes: flags, the
// optional sizes, the filter flags, padding, CRC32). liblzma's stream encoder with xdelta3's settings writes one
// LZMA2 filter and leaves the block open.
bool XzRunOn::parse_headers(const uint8_t*& in, size_t& in_size) {
  while (in_size) {
    head_.push_back(*in++); --in_size;
    if (head_.size() < 13) continue;
    static const uint8_t kMagic[6] = {0xFD, '7', 'z', 'X', 'Z', 0};
    if (std::memcmp(head_.data(), kMagic, 6) != 0 || crc32_update(0, head_.data() + 6, 2) !=
        (uint32_t(head_[8]) | uint32_t(head_[9]) << 8 | uint32_t(head_[10]) << 16 | uint32_t(head_[11]) << 24)) return false;
    const size_t block_size = (size_t(head_[12]) + 1) * 4;
    if (head_[12] == 0) return false;                       // an index: no block at all
    if (head_.size() < 12 + block_size) continue;
    const uint8_t* b = head_.data() + 12;
    const uint32_t crc = uint32_t(b[block_size - 4]) | uint32_t(b[block_size - 3]) << 8 | uint32_t(b[block_size - 2]) << 16 | uint32_t(b[block_size - 1]) << 24;
    if (crc32_update(0, b, block_size - 4) != crc) return false;
    const uint8_t flags = b[1];
    size_t at = 2;
    uint64_t v = 0;
    if ((flags & 0x40) && !xz_vli(b, block_size - 4, &at, &v)) return false;
    if ((flags & 0x80) && !xz_vli(b, block_size - 4, &at, &v)) return false;
    if ((flags & 0x03) != 0) return false;                 // more than one filter
    uint64_t id = 0, props = 0;
    if (!xz_vli(b, block_size - 4, &at, &id) || !xz_vli(b, block_size - 4, &at, &props) || id != 0x21 || props != 1 || at >= block_size - 4) return false;
    lzma2_.reset(new LzmaStream(LzmaStream::Lzma2, b + at, 1));
    if (!lzma2_->ok()) return false;
    headers_done_ = true;
    return true;
  }
  return true;   // more header bytes are to come
}

bool XzRunOn::decode(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size) {
  if (!headers_done_ && (!parse_headers(in, in_size) || !headers_done_)) return false;
  size_t made = 0;
  while (made < out_size) {
    size_t used = in_size, produced = out_size - made;
    const int r = lzma2_->decode(in, &used, out + made, &produced);
    if (r < 0) return false;
    in += used; in_size -= used; made += produced;
    if (used == 0 && produced == 0) return false;
  }
  return true;
}

}  // namespace modinstall
