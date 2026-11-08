// SPDX-License-Identifier: GPL-2.0-or-later
#include "gx_aurora.h"
#include "gx_aurora_bridge.h"

#include <aurora/aurora.h>
#include <aurora/gfx.hpp>
#include <dolphin/gx/GXExtra.h>
#include <dolphin/gx/GXFrameBuffer.h>
#include <dolphin/gx/GXTexture.h>
// Verified pinned Aurora writer, not a locally mirrored ABI. Normalized GX
// commands enter its real decoder, which generates WGSL and Metal pipelines.
#include <gx/fifo.hpp>
#include <gx/gx.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <thread>
#include <tuple>

namespace gx {
namespace {
namespace fifo = aurora::gx::fifo;
using aurora_bridge::CoverageError;
GXColor clear_color(uint32_t argb) {
  return GXColor{uint8_t(argb >> 16), uint8_t(argb >> 8), uint8_t(argb), uint8_t(argb >> 24)};
}
void bp(uint8_t reg, uint32_t value) {
  fifo::write_u8(0x61); fifo::write_u32(uint32_t(reg) << 24 | (value & 0xffffff));
}
// EFB copy format (tp_realFormat with the intensity flag) -> the GX texture format the
// game samples it as. Aurora converts the RGBA8 framebuffer into that format itself.
GXTexFmt copy_format(const EfbCopy& c) {
  if (c.intensity) {
    switch (c.format) { case 0: return GX_TF_I4; case 2: return GX_TF_IA4; case 3: return GX_TF_IA8; case 8: return GX_TF_I8; default: return GX_TF_I8; }
  }
  switch (c.format) {
    case 0: return GX_CTF_R4; case 2: return GX_CTF_RA4; case 3: return GX_CTF_RA8;
    case 4: return GX_TF_RGB565; case 5: return GX_TF_RGB5A3; case 6: return GX_TF_RGBA8;
    case 7: return GX_CTF_A8; case 8: return GX_CTF_R8; case 9: return GX_CTF_G8; case 10: return GX_CTF_B8;
    case 11: return GX_CTF_RG8; case 12: return GX_CTF_GB8;
    default: return GX_TF_RGBA8;
  }
}
} // namespace

struct AuroraBackend::Impl {
  AuroraOptions options;
  AuroraStats counters;
  const std::thread::id owner = std::this_thread::get_id();
  std::optional<uint64_t> last_sequence;
  aurora_bridge::CopyHistory copy_history;
  struct CopyToken { uint8_t byte = 0; };
  std::map<uint32_t, std::unique_ptr<CopyToken>> copy_tokens;
  struct TextureKey {
    uintptr_t source;
    uint32_t width, height, format, mode0, mode1, tlut_format, levels, unit;
    auto tuple() const { return std::tie(source, width, height, format, mode0, mode1, tlut_format, levels, unit); }
    bool operator<(const TextureKey& b) const { return tuple() < b.tuple(); }
  };
  struct TextureObject {
    GXTexObj texture{};
    GXTlutObj palette{};
    bool has_palette = false;
    uint64_t last_frame = 0;
    std::shared_ptr<const TextureSnapshot> source;
  };
  std::map<TextureKey, TextureObject> textures;

  explicit Impl(AuroraOptions o) : options(o) {
    require_owner();
    if (!options.logical_width || options.logical_width > EFB_WIDTH || !options.logical_height || options.logical_height > EFB_HEIGHT)
      throw CoverageError("Aurora coverage: invalid backend logical extent");
    const auto [width, height] = aurora::gx::logical_fb_size();
    if (width != options.logical_width || height != options.logical_height)
      throw CoverageError("Aurora coverage: owner must configure VI to the backend logical extent");
    initial_clear();
  }
  void require_owner() const {
    if (std::this_thread::get_id() != owner) throw std::logic_error("AuroraBackend must be used on its owning thread");
  }
  void initial_clear() {
    GXSetCopyClear(clear_color(options.reset_clear_color), options.reset_clear_z & 0xffffff);
    fifo::drain();
  }
  void release_objects() {
    fifo::drain();
    aurora::gfx::synchronize();
    for (auto& [key, object] : textures) {
      GXDestroyTexObj(&object.texture);
      if (object.has_palette) GXDestroyTlutObj(&object.palette);
    }
    for (auto& [address, token] : copy_tokens) GXDestroyCopyTex(token.get());
    fifo::drain();
    aurora::gfx::synchronize();
    textures.clear(); copy_tokens.clear(); copy_history.clear();
  }
  TextureObject& texture(const TextureRef& t, unsigned unit, const void* copy_source) {
    const void* source = copy_source ? copy_source : t.data->image.data();
    const TextureKey key{reinterpret_cast<uintptr_t>(source), t.width, t.height, t.format, t.mode0, t.mode1,
                         t.tlut_format, t.mip_levels, unit};
    auto [it, inserted] = textures.try_emplace(key);
    auto& obj = it->second;
    obj.last_frame = counters.frames;
    if (!inserted) return obj;
    obj.source = t.data; // Keeps the immutable CPU image and palette alive until FIFO drain.
    const auto wrap_s = static_cast<GXTexWrapMode>(bits(t.mode0, 0, 2));
    const auto wrap_t = static_cast<GXTexWrapMode>(bits(t.mode0, 2, 2));
    const auto mips = t.mip_levels > 1 ? GX_TRUE : GX_FALSE;
    obj.has_palette = t.format == 8 || t.format == 9 || t.format == 10;
    if (obj.has_palette) {
      GXInitTlutObj(&obj.palette, t.data->palette.data(), static_cast<GXTlutFmt>(t.tlut_format),
                    uint16_t(t.format == 8 ? 16 : t.format == 9 ? 256 : 16384));
      GXInitTexObjCI(&obj.texture, source, uint16_t(t.width), uint16_t(t.height), static_cast<GXCITexFmt>(t.format),
                     wrap_s, wrap_t, mips, unit);
    } else {
      GXInitTexObj(&obj.texture, source, uint16_t(t.width), uint16_t(t.height), static_cast<GXTexFmt>(t.format),
                   wrap_s, wrap_t, mips);
    }
    constexpr GXTexFilter hw_filter[8]{GX_NEAR, GX_NEAR_MIP_NEAR, GX_LIN_MIP_NEAR, GX_NEAR,
                                     GX_LINEAR, GX_NEAR_MIP_LIN, GX_LIN_MIP_LIN, GX_NEAR};
    GXInitTexObjLOD(&obj.texture, hw_filter[bits(t.mode0, 5, 3)], (t.mode0 & 16) ? GX_LINEAR : GX_NEAR,
                    bits(t.mode1, 0, 8) / 16.f, bits(t.mode1, 8, 8) / 16.f,
                    int8_t(bits(t.mode0, 9, 8)) / 32.f, bits(t.mode0, 21, 1) ? GX_TRUE : GX_FALSE,
                    bits(t.mode0, 8, 1) ? GX_FALSE : GX_TRUE, static_cast<GXAnisotropy>(bits(t.mode0, 19, 2)));
    return obj;
  }
  void submit(const Frame& input) {
    require_owner();
    if (last_sequence && input.sequence <= *last_sequence)
      throw CoverageError("Aurora coverage: repeated/reversed source frame; reset_epoch is required after rollback");
    const auto frame = aurora_bridge::prepare_frame(input, {options.logical_width, options.logical_height});
    auto next_history = aurora_bridge::advance_copy_history(frame, copy_history); // Atomic validation before GPU recording.
    if (!aurora_begin_frame()) throw std::runtime_error("Aurora frame unavailable; owner must service the window before submission");
    for (const auto& cmd : frame.commands) {
      if (cmd.kind == FrameCommand::Draw) {
        const auto& d = frame.draws[cmd.index];
        if (d.vertex_count == 0) continue;   // skipped by the bridge
        for (unsigned unit = 0; unit < 8; ++unit) {
          const auto& t = d.textures[unit];
          if (!t.used) continue;
          const auto found = copy_tokens.find(t.addr);
          auto& obj = texture(t, unit, found == copy_tokens.end() ? nullptr : found->second.get());
          if (obj.has_palette) GXLoadTlut(&obj.palette, unit);
          GXLoadTexObj(&obj.texture, static_cast<GXTexMapID>(unit));
        }
        // GX SDK texture loads may emit cached SDK state. Captured draw state is
        // therefore restored last, before canonical direct vertex bytes.
        fifo::write_data(d.state.data(), uint32_t(d.state.size()));
        fifo::write_u8(d.primitive); fifo::write_u16(d.vertex_count);
        fifo::write_data(d.vertices.data(), uint32_t(d.vertices.size()));
        fifo::finish_draw();
        ++counters.draws;
      } else {
        if (std::find(frame.skipped_copies.begin(), frame.skipped_copies.end(), cmd.index) != frame.skipped_copies.end()) continue;
        const auto& c = frame.copies[cmd.index];
        bp(BP_ZMODE, c.zmode); bp(BP_BLENDMODE, c.blendmode);
        bp(BP_CONSTANTALPHA, c.dstalpha); bp(BP_ZCOMPARE, c.zcontrol);
        GXSetCopyClear(clear_color(c.clear_color), c.clear_z);
        if (!c.to_xfb) {
          auto& token = copy_tokens[c.dest_addr];
          if (!token) token = std::make_unique<CopyToken>();
          const bool full_rect = c.src_x == 0 && c.src_y == 0 && c.src_w == options.logical_width && c.src_h == options.logical_height;
          GXSetTexCopySrc(uint16_t(c.src_x), uint16_t(c.src_y), uint16_t(c.src_w), uint16_t(c.src_h));
          GXSetTexCopyDst(uint16_t(c.src_w), uint16_t(c.src_h), copy_format(c), GX_FALSE);
          GXCopyTex(token.get(), c.clear && full_rect ? GX_TRUE : GX_FALSE);
          ++counters.texture_copies;
        }
        // GXCopyDisp is a stub. The validated final full-size XFB is presented
        // by aurora_end_frame; the next aurora_begin_frame performs its full
        // clear using the copy-time values just loaded. Never clear before blit.
      }
    }
    aurora_end_frame(); // Drains FIFO; uploaded texture source bytes are now safe.
    copy_history = std::move(next_history);
    last_sequence = input.sequence;
    ++counters.frames;
    for (auto it = textures.begin(); it != textures.end();) {
      if (it->second.last_frame + 3 < counters.frames) {
        GXDestroyTexObj(&it->second.texture);
        if (it->second.has_palette) GXDestroyTlutObj(&it->second.palette);
        it = textures.erase(it);
      } else ++it;
    }
    fifo::drain();
  }
};

AuroraBackend::AuroraBackend(AuroraOptions options) : impl_(std::make_unique<Impl>(options)) {}
AuroraBackend::~AuroraBackend() { impl_->release_objects(); }
void AuroraBackend::submit_frame(const Frame& frame) { impl_->submit(frame); }
void AuroraBackend::submit_frame(const Frame& frame, const DrawMatrices* overrides) {
  if (overrides) throw CoverageError("Aurora coverage: unlocked interpolation is disabled pending locked-state fidelity gates");
  submit_frame(frame);
}
void AuroraBackend::set_skip_present(bool skip) {
  if (skip) throw CoverageError("Aurora coverage: drain-without-present is not implemented; source copies may not be dropped");
}
void AuroraBackend::reset_epoch(uint64_t epoch) {
  impl_->require_owner();
  if (epoch <= impl_->counters.epoch) throw std::invalid_argument("Aurora epoch must increase");
  impl_->release_objects();
  impl_->last_sequence.reset();
  impl_->counters.epoch = epoch;
  impl_->initial_clear();
}
AuroraStats AuroraBackend::stats() const { impl_->require_owner(); return impl_->counters; }
} // namespace gx
