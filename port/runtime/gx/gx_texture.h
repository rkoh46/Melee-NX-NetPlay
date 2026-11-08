// GameCube texture decoding to RGBA8.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <vector>

namespace gx {

// Bytes occupied by one mip level of a texture in guest memory.
uint32_t texture_level_bytes(uint32_t width, uint32_t height, uint32_t format);
// Decodes one level into out (width*height*4 bytes). tlut points at the TMEM palette (big-endian u16s).
void decode_texture(const uint8_t* src, uint32_t width, uint32_t height, uint32_t format,
                    const uint8_t* tlut, uint32_t tlut_format, std::vector<uint8_t>& out);
uint64_t hash_bytes(const void* data, size_t len);

}  // namespace gx
