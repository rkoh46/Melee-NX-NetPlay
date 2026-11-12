// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
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
#include <gfx/texture.hpp>
#include <gfx/texture_convert.hpp>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <array>
#include <map>
#include <unordered_map>
#include <memory>
#include <optional>
#include <thread>
#include <tuple>
#include <vector>

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
// Experiment (Switch bring-up): every texture copy is kept as RGBA8 on the GPU and sampled as
// RGBA8. Melee's shadow maps are 256x256 I4 intensity copies and the I4 path rendered noise.
// Live debug bits (set_debug_file): experiments that can be flipped during a match.
constexpr unsigned kDebugSkipCopyDraws = 1;     // drop every draw that samples an EFB copy
constexpr unsigned kDebugCopyFromRam = 2;       // sample the guest RAM snapshot instead of the GPU copy
constexpr unsigned kDebugCopyChecker = 4;       // sample a host checkerboard instead of the GPU copy
constexpr unsigned kDebugCopyNativeFormat = 8;  // copy in the game's format (I4...) instead of RGBA8
constexpr unsigned kDebugCopyClearPartial = 16; // honour the copy's clear flag for partial rects too
constexpr unsigned kDebugDumpNow = 32;          // one-shot: draw dump of the next frame
constexpr unsigned kDebugSkipCopies = 64;       // do not perform texture copies at all
constexpr unsigned kDebugCopyHalfSize = 128;    // copy into a half-size texture (Aurora's blit shader instead of vkCmdCopyImage)
constexpr unsigned kDebugShowCopies = 256;      // draw this frame's texture copies as quads along the top of the screen
constexpr unsigned kDebugPaletteNative = 1024;  // hand Aurora the palette indices and TLUT instead of decoding them here (see texture())
constexpr unsigned kDebugShowTextures = 512;    // draw the textures this frame samples as thumbnails; bits 16-19 pick one GX format (0 = all)
unsigned g_debug = 0;
bool copies_as_rgba8() { return !(g_debug & kDebugCopyNativeFormat); }
GXTexFmt copy_format(const EfbCopy& c) {
  if (copies_as_rgba8()) return GX_TF_RGBA8;
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
  // One token per guest destination of an EFB-to-texture copy: Aurora keys its copy textures on the token's
  // address. A token neither copied into nor sampled for kCopyTokenIdleFrames is destroyed (with its GPU
  // texture): copies land in per-scene heap memory, so every match used to add textures that lived forever.
  struct CopyToken { uint64_t last_used = 0; uint32_t width = 0, height = 0, format = ~0u; };
  static constexpr uint64_t kCopyTokenIdleFrames = 600;   // 10 s
  std::map<uint32_t, std::unique_ptr<CopyToken>> copy_tokens;
  struct TextureKey {
    uintptr_t source;
    uint32_t width, height, format, mode0, mode1, tlut_format, levels, unit;
    auto tuple() const { return std::tie(source, width, height, format, mode0, mode1, tlut_format, levels, unit); }
    bool operator<(const TextureKey& b) const { return tuple() < b.tuple(); }
    bool operator==(const TextureKey& b) const { return tuple() == b.tuple(); }
  };
  struct TextureKeyHash {
    size_t operator()(const TextureKey& k) const {
      uint64_t h = k.source * 0x9E3779B97F4A7C15ull;
      for (uint32_t v : {k.width, k.height, k.format, k.mode0, k.mode1, k.tlut_format, k.levels, k.unit})
        h = (h ^ v) * 0x100000001B3ull;
      return size_t(h ^ (h >> 29));
    }
  };
  struct TextureObject {
    GXTexObj texture{};
    GXTlutObj palette{};
    bool has_palette = false;
    uint64_t last_frame = 0;
    std::shared_ptr<const TextureSnapshot> source;
    std::vector<uint8_t> converted;   // paletted textures: the decoded image Aurora samples instead of the indices
  };
  std::unordered_map<TextureKey, TextureObject, TextureKeyHash> textures;   // looked up for every texture of every draw
  std::function<void()> overlay;
  aurora_bridge::PreparedFrame prepared;   // reused every frame: its draws keep their byte buffers
  std::string dump_path; uint8_t dump_major = 0, dump_minor = 0; unsigned dump_after = 0, dump_seen = 0; bool dump_armed = false;
  std::string debug_path; bool dump_now = false;
  std::vector<uint8_t> checker;   // 256x256 RGBA8 host checkerboard for kDebugCopyChecker (GX tiled RGBA8)
  void poll_debug_file() {
    if (debug_path.empty() || counters.frames % 60) return;
    FILE* f = std::fopen(debug_path.c_str(), "r");
    if (!f) return;
    unsigned v = 0;
    const bool ok = std::fscanf(f, "%u", &v) == 1;
    std::fclose(f);
    if (!ok) return;
    if (v & kDebugDumpNow) {
      dump_now = true;
      v &= ~kDebugDumpNow;
      if (FILE* w = std::fopen(debug_path.c_str(), "w")) { std::fprintf(w, "%u\n", v); std::fclose(w); }
    }
    if (v != g_debug) { std::fprintf(stderr, "aurora debug bits: %u -> %u\n", g_debug, v); g_debug = v; }
  }
  const void* checker_image() {
    if (checker.empty()) {
      // GX RGBA8 tiles: 4x4 texels, 64 bytes: AR pairs for 16 texels then GB pairs.
      checker.resize(256 * 256 * 4);
      for (unsigned ty = 0; ty < 64; ++ty) for (unsigned tx = 0; tx < 64; ++tx) {
        uint8_t* tile = checker.data() + (ty * 64 + tx) * 64;
        for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x) {
          const unsigned px = tx * 4 + x, py = ty * 4 + y;
          const bool on = ((px / 16) + (py / 16)) & 1;
          const uint8_t r = on ? 255 : 32, g = uint8_t(px), b = uint8_t(py);
          tile[(y * 4 + x) * 2] = 255; tile[(y * 4 + x) * 2 + 1] = r;
          tile[32 + (y * 4 + x) * 2] = g; tile[32 + (y * 4 + x) * 2 + 1] = b;
        }
      }
    }
    return checker.data();
  }

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
    const bool checker_mode = copy_source && (g_debug & kDebugCopyChecker) && t.width == 256 && t.height == 256;
    const void* source = checker_mode ? checker_image() : copy_source ? copy_source : t.data->image.data();
    const uint32_t format = (checker_mode || (copy_source && copies_as_rgba8())) ? 6u : t.format;   // 6 = GX_TF_RGBA8
    // Paletted textures (CI4/CI8/CI14) are decoded here with Aurora's own converter and handed over as RGBA8:
    // given the indices and a TLUT, Aurora's draw-time palette path on the console drew nothing for CI8
    // (every character-select icon and portrait, the stage tiles), while the same converter run on the
    // same bytes decodes them correctly. Debug bit 1024 restores the native path for comparison.
    const bool decode_palette = (format == 8 || format == 9 || format == 10) && !(g_debug & kDebugPaletteNative);
    const TextureKey key{reinterpret_cast<uintptr_t>(source), t.width, t.height, format | (decode_palette ? 0x100u : 0u), t.mode0, t.mode1,
                         t.tlut_format, t.mip_levels, unit};
    auto [it, inserted] = textures.try_emplace(key);
    auto& obj = it->second;
    obj.last_frame = counters.frames;
    if (!inserted) return obj;
    obj.source = t.data; // Keeps the immutable CPU image and palette alive until FIFO drain.
    const auto wrap_s = static_cast<GXTexWrapMode>(bits(t.mode0, 0, 2));
    const auto wrap_t = static_cast<GXTexWrapMode>(bits(t.mode0, 2, 2));
    const auto mips = t.mip_levels > 1 ? GX_TRUE : GX_FALSE;
    obj.has_palette = format == 8 || format == 9 || format == 10;
    if (decode_palette) {
      const uint16_t entries = format == 8 ? 16 : format == 9 ? 256 : 16384;
      auto converted = aurora::gfx::convert_texture_palette(format, t.width, t.height, t.mip_levels,
                                                            {t.data->image.data(), texture_chain_bytes(t.width, t.height, format, t.mip_levels)},
                                                            static_cast<GXTlutFmt>(t.tlut_format), entries, {t.data->palette.data(), t.data->palette.size()});
      if (!converted.data.empty()) {
        obj.converted.assign(converted.data.data(), converted.data.data() + converted.data.size());
        obj.has_palette = false;
        GXInitTexObj(&obj.texture, obj.converted.data(), uint16_t(t.width), uint16_t(t.height), GX_TF_RGBA8_PC, wrap_s, wrap_t, mips);
      }
    }
    if (!obj.has_palette && !obj.converted.empty()) {
      // already initialised above
    } else if (obj.has_palette) {
      GXInitTlutObj(&obj.palette, t.data->palette.data(), static_cast<GXTlutFmt>(t.tlut_format),
                    uint16_t(t.format == 8 ? 16 : t.format == 9 ? 256 : 16384));
      GXInitTexObjCI(&obj.texture, source, uint16_t(t.width), uint16_t(t.height), static_cast<GXCITexFmt>(format),
                     wrap_s, wrap_t, mips, unit);
    } else {
      GXInitTexObj(&obj.texture, source, uint16_t(t.width), uint16_t(t.height), static_cast<GXTexFmt>(format),
                   wrap_s, wrap_t, mips);
    }
    // Aurora keys its conversion caches by object identity, which it derives from the data pointer.
    // Snapshot buffers are short-lived and their addresses recycle, so give it the content instead:
    // the snapshot hash (image ^ palette) for the texture, a hash of the palette bytes for the TLUT.
    {
      const uint64_t h = t.data->hash ^ (uint64_t(t.width) << 48) ^ (uint64_t(t.height) << 32) ^ (uint64_t(t.format) << 24);
      __GXSetTexObjIdentity(&obj.texture, uint32_t(h) ^ uint32_t(h >> 32), 1);
      if (obj.has_palette) {
        uint64_t ph = 1469598103934665603ull;
        for (uint8_t b : t.data->palette) ph = (ph ^ b) * 1099511628211ull;
        ph ^= uint64_t(t.tlut_format) << 56;
        __GXSetTlutObjIdentity(&obj.palette, uint32_t(ph) ^ uint32_t(ph >> 32), 1);
      }
    }
    constexpr GXTexFilter hw_filter[8]{GX_NEAR, GX_NEAR_MIP_NEAR, GX_LIN_MIP_NEAR, GX_NEAR,
                                     GX_LINEAR, GX_NEAR_MIP_LIN, GX_LIN_MIP_LIN, GX_NEAR};
    GXInitTexObjLOD(&obj.texture, hw_filter[bits(t.mode0, 5, 3)], (t.mode0 & 16) ? GX_LINEAR : GX_NEAR,
                    bits(t.mode1, 0, 8) / 16.f, bits(t.mode1, 8, 8) / 16.f,
                    int8_t(bits(t.mode0, 9, 8)) / 32.f, bits(t.mode0, 21, 1) ? GX_TRUE : GX_FALSE,
                    bits(t.mode0, 8, 1) ? GX_FALSE : GX_TRUE, static_cast<GXAnisotropy>(bits(t.mode0, 19, 2)));
    return obj;
  }
  // One frame of draw state, one line per draw, for finding what the Aurora path renders wrongly.
  void write_draw_dump(const Frame& input, const aurora_bridge::PreparedFrame& frame) {
    FILE* f = std::fopen(dump_path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "# frame seq=%llu scene %02X/%02X draws=%zu copies=%zu commands=%zu\n", (unsigned long long)input.sequence,
                 input.scene_major, input.scene_minor, input.draws.size(), input.copies.size(), input.commands.size());
    size_t draw_index = 0;
    auto rebuilt = std::make_unique<DrawState>();   // the draws carry state deltas: rebuild it in order
    DrawState& st = *rebuilt;
    for (const auto& cmd : input.commands) {
      if (cmd.kind != FrameCommand::Draw) { std::fprintf(f, "copy %u\n", unsigned(cmd.index)); continue; }
      const auto& d = input.draws[cmd.index];
      apply_state(st, d, input);
      const bool skipped = cmd.index < frame.draws.size() && frame.draws[cmd.index].vertex_count == 0;
      std::fprintf(f, "draw %zu%s prim=%02X verts=%u comps=%08X genmode=%08X texgens=%u tevstages=%u indstages=%u mtxidx=%08X/%08X zmode=%06X blend=%06X owner=%u\n",
                   draw_index++, skipped ? " SKIPPED" : "", d.primitive, d.vertex_count, d.components, st.bp.reg[0], st.bp.numtexgens(),
                   st.bp.numtevstages() + 1, st.bp.numindstages(), st.matrix_index_a, st.matrix_index_b, st.bp.reg[0x40] & 0xffffff,
                   st.bp.reg[0x41] & 0xffffff, d.owner_player);
      std::fprintf(f, "  xf: numtexgens=%u", st.xf_regs[0x3f]);
      for (unsigned i = 0; i < 8; ++i) std::fprintf(f, " tg%u=%08X/post%08X", i, st.xf_regs[0x40 + i], st.xf_regs[0x50 + i]);
      std::fprintf(f, " dualtex=%u colorchans=%u\n", st.xf_regs[0x12] & 1, st.xf_regs[0x09]);
      std::fprintf(f, "  tev:");
      for (unsigned s2 = 0; s2 <= st.bp.numtevstages(); ++s2)
        std::fprintf(f, " [%u map=%d coord=%d en=%d c=%08X a=%08X ind=%08X]", s2, st.bp.order_texmap(int(s2)), st.bp.order_texcoord(int(s2)),
                     st.bp.order_enable(int(s2)), st.bp.reg[0xC0 + s2 * 2] & 0xffffff, st.bp.reg[0xC1 + s2 * 2] & 0xffffff, st.bp.tevind(int(s2)) & 0xffffff);
      std::fprintf(f, " ztex=%06X/%06X alpha=%06X\n", st.bp.reg[0x74] & 0xffffff, st.bp.reg[0x75] & 0xffffff, st.bp.reg[0xF3] & 0xffffff);
      std::fprintf(f, "  regs:");
      for (unsigned i = 0; i < 4; ++i) std::fprintf(f, " c%u=%d,%d,%d,%d", i, st.tev_colors[i][0], st.tev_colors[i][1], st.tev_colors[i][2], st.tev_colors[i][3]);
      for (unsigned i = 0; i < 4; ++i) std::fprintf(f, " k%u=%d,%d,%d,%d", i, st.tev_kcolors[i][0], st.tev_kcolors[i][1], st.tev_kcolors[i][2], st.tev_kcolors[i][3]);
      std::fprintf(f, " ksel=");
      for (unsigned r = 0xF6; r <= 0xFD; ++r) std::fprintf(f, "%06X ", st.bp.reg[r] & 0xffffff);
      std::fprintf(f, "chan=%08X/%08X/%08X/%08X amb=%08X/%08X mat=%08X/%08X\n", st.xf_regs[0x0e], st.xf_regs[0x0f], st.xf_regs[0x10], st.xf_regs[0x11],
                   st.xf_regs[0x0a], st.xf_regs[0x0b], st.xf_regs[0x0c], st.xf_regs[0x0d]);
      if (st.bp.numtexgens()) {
        const unsigned pn = st.matrix_index_a & 63;
        std::fprintf(f, "  pnmtx%u:", pn);
        for (unsigned k = 0; k < 12; ++k) std::fprintf(f, " %.4g", st.posMatrices[(pn / 3) * 12 + k]);
        for (unsigned i = 0; i < st.bp.numtexgens() && i < 8; ++i) {
          const unsigned tm = i < 4 ? (st.matrix_index_a >> (6 + i * 6)) & 63 : (st.matrix_index_b >> ((i - 4) * 6)) & 63;
          if (tm != 60) {   // GX_IDENTITY
            std::fprintf(f, "\n  texmtx%u(%u):", i, tm);
            for (unsigned k = 0; k < 12; ++k) std::fprintf(f, " %.4g", st.posMatrices[(tm / 3) * 12 + k]);
          }
          const unsigned post = (st.xf_regs[0x12] & 1) ? st.xf_regs[0x50 + i] & 63 : 61;
          if (post != 61) {
            std::fprintf(f, "\n  postmtx%u(%u):", i, post);
            for (unsigned k = 0; k < 12; ++k) std::fprintf(f, " %.4g", st.postMatrices[(post / 3) * 12 + k]);
          }
        }
        std::fprintf(f, "\n");
      }
      for (unsigned i = 0; i < 8; ++i) {
        const auto& t = d.textures[i];
        if (!t.used) continue;
        std::fprintf(f, "  tex%u: addr=%08X %ux%u fmt=%u tlut=%08X/%u mode0=%08X mode1=%08X mips=%u image0=%08X\n", i, t.addr, t.width, t.height, t.format,
                     t.tlut_addr, t.tlut_format, t.mode0, t.mode1, t.mip_levels, st.bp.teximage0(int(i)) & 0xffffff);
      }
    }
    for (const auto& c : input.copies)
      std::fprintf(f, "efbcopy: dest=%08X src=%u,%u %ux%u fmt=%08X clear=%d xfb=%d\n", c.dest_addr, c.src_x, c.src_y, c.src_w, c.src_h, c.format, c.clear ? 1 : 0, c.to_xfb ? 1 : 0);
    std::fclose(f);
  }
  // Diagnostic: show every texture copy of this frame as a 320x240 image along the top edge, drawn
  // by ImGui from Aurora's own copy texture views, so the copy textures' contents are seen directly.
  void show_copies(const aurora_bridge::PreparedFrame& frame) {
    auto* list = ImGui::GetForegroundDrawList();
    if (!list) return;
    char label[160];
    std::snprintf(label, sizeof label, "frame copies %zu, tokens %zu, aurora copy textures %zu / cache %zu, texture copies so far %llu",
                  frame.copies.size(), copy_tokens.size(), aurora::gx::g_gxState.copyTextures.size(),
                  aurora::gx::g_gxState.copyTextureCache.size(), (unsigned long long)counters.texture_copies);
    list->AddText(ImVec2(10, 20), IM_COL32(255, 255, 0, 255), label);
    unsigned slot = 0;
    for (const auto& c : frame.copies) {
      if (c.to_xfb || slot >= 3) continue;
      const float x0 = 10.f + slot * 330.f, y0 = 40.f;
      ++slot;
      const auto token = copy_tokens.find(c.dest_addr);
      if (token == copy_tokens.end()) { list->AddText(ImVec2(x0, y0), IM_COL32(255, 80, 80, 255), "no token"); continue; }
      const auto it = aurora::gx::g_gxState.copyTextures.find(token->second.get());
      if (it == aurora::gx::g_gxState.copyTextures.end()) {
        std::snprintf(label, sizeof label, "copy %08X: token %p has no Aurora copy texture", c.dest_addr, (void*)token->second.get());
        list->AddText(ImVec2(x0, y0), IM_COL32(255, 80, 80, 255), label); continue;
      }
      if (!it->second.handle) { list->AddText(ImVec2(x0, y0), IM_COL32(255, 80, 80, 255), "copy texture handle is null"); continue; }
      const auto& tex = *it->second.handle;
      const ImTextureID id = reinterpret_cast<ImTextureID>(tex.sampleTextureView.Get());
      list->AddRectFilled(ImVec2(x0 - 2, y0 - 2), ImVec2(x0 + 322, y0 + 242), IM_COL32(255, 0, 255, 255));
      list->AddImage(id, ImVec2(x0, y0), ImVec2(x0 + 320, y0 + 240));
      std::snprintf(label, sizeof label, "copy %08X %ux%u fmt %u -> %ux%u gx %u rev %u", c.dest_addr, c.src_w, c.src_h, c.format,
                    tex.size.width, tex.size.height, tex.gxFormat, it->second.revision);
      list->AddText(ImVec2(x0, y0 + 244), IM_COL32(255, 255, 0, 255), label);
    }
  }
  // Diagnostic: show the textures this frame's draws sample, decoded by Aurora's own converters from the
  // captured bytes and drawn by ImGui as 128-point thumbnails in rows along the top. The format selector in
  // the debug bits (bits 16-19, 0 = every format) picks one GX texture format, so a blank or garbled
  // texture can be told apart from a draw that goes wrong after sampling. Uploads are cached by content.
  std::map<uint64_t, aurora::gfx::TextureHandle> debug_textures;
  void show_textures(const Frame& frame) {
    auto* list = ImGui::GetForegroundDrawList();
    if (!list) return;
    const unsigned want = (g_debug >> 16) & 0xF;
    std::vector<const TextureRef*> shown;
    for (const auto& cmd : frame.commands) {
      if (cmd.kind != FrameCommand::Draw) continue;
      for (const auto& t : frame.draws[cmd.index].textures) {
        if (!t.used || !t.data || (want && t.format != want)) continue;
        if (copy_tokens.count(t.addr)) continue;   // copies have their own viewer
        bool seen = false;
        for (const auto* s : shown) if (s->data->hash == t.data->hash) { seen = true; break; }
        if (!seen) shown.push_back(&t);
        if (shown.size() >= 18) break;
      }
      if (shown.size() >= 18) break;
    }
    char label[200];
    std::snprintf(label, sizeof label, "textures: %zu distinct%s shown of this frame's draws (format filter %u)", shown.size(),
                  shown.size() >= 18 ? "+" : "", want);
    list->AddText(ImVec2(10, 20), IM_COL32(255, 255, 0, 255), label);
    unsigned slot = 0;
    for (const auto* tp : shown) {
      const auto& t = *tp;
      const float x0 = 10.f + (slot % 9) * 140.f, y0 = 40.f + (slot / 9) * 170.f;
      ++slot;
      const uint64_t key = t.data->hash ^ (uint64_t(t.format) << 60);
      auto it = debug_textures.find(key);
      if (it == debug_textures.end()) {
        aurora::gfx::TextureHandle handle;
        const uint32_t level0 = texture_level_bytes(t.width, t.height, t.format);
        if (t.format == 8 || t.format == 9 || t.format == 10) {
          const uint16_t entries = t.format == 8 ? 16 : t.format == 9 ? 256 : 16384;
          auto converted = aurora::gfx::convert_texture_palette(t.format, t.width, t.height, 1, {t.data->image.data(), level0},
                                                                static_cast<GXTlutFmt>(t.tlut_format), entries,
                                                                {t.data->palette.data(), t.data->palette.size()});
          if (!converted.data.empty())
            handle = aurora::gfx::new_static_texture_2d(t.width, t.height, 1, GX_TF_RGBA8_PC, {converted.data.data(), converted.data.size()},
                                                        false, "debug texture");
        } else if (t.data->image.size() >= level0) {
          handle = aurora::gfx::new_static_texture_2d(t.width, t.height, 1, t.format, {t.data->image.data(), level0}, false, "debug texture");
        }
        it = debug_textures.emplace(key, std::move(handle)).first;
      }
      list->AddRectFilled(ImVec2(x0 - 2, y0 - 2), ImVec2(x0 + 130, y0 + 130), IM_COL32(255, 0, 255, 255));
      list->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + 128, y0 + 128), IM_COL32(40, 40, 40, 255));
      if (it->second) {
        const ImTextureID id = reinterpret_cast<ImTextureID>(it->second->sampleTextureView.Get());
        list->AddImage(id, ImVec2(x0, y0), ImVec2(x0 + 128, y0 + 128));
      } else {
        list->AddText(ImVec2(x0 + 4, y0 + 56), IM_COL32(255, 80, 80, 255), "no upload");
      }
      std::snprintf(label, sizeof label, "%08X %ux%u f%u", t.addr, t.width, t.height, t.format);
      list->AddText(ImVec2(x0, y0 + 132), IM_COL32(255, 255, 0, 255), label);
      std::snprintf(label, sizeof label, "tlut %X/%u m%u", t.tlut_addr, t.tlut_format, t.mip_levels);
      list->AddText(ImVec2(x0, y0 + 146), IM_COL32(255, 255, 0, 255), label);
    }
    if (debug_textures.size() > 256) debug_textures.clear();
  }
  void submit(const Frame& input) {
    require_owner();
    if (last_sequence && input.sequence <= *last_sequence)
      throw CoverageError("Aurora coverage: repeated/reversed source frame; reset_epoch is required after rollback");
    poll_debug_file();
    if (!aurora_begin_frame()) throw std::runtime_error("Aurora frame unavailable; owner must service the window before submission");
    // Each draw goes to Aurora as soon as it is converted: Aurora's FIFO thread decodes the frame
    // while this thread is still converting it (it used to start only after the whole frame was ready).
    const auto& frame = prepared;
    const auto emit = [&](size_t command_index) {
      const auto& cmd = frame.commands[command_index];
      if (cmd.kind == FrameCommand::Draw) {
        const auto& d = frame.draws[cmd.index];
        if (d.vertex_count == 0) return;   // skipped by the bridge
        if (g_debug & kDebugSkipCopyDraws) {
          bool samples_copy = false;
          for (unsigned unit = 0; unit < 8; ++unit)
            if (d.textures[unit]->used && copy_tokens.count(d.textures[unit]->addr)) samples_copy = true;
          if (samples_copy) return;
        }
        for (unsigned unit = 0; unit < 8; ++unit) {
          const auto& t = *d.textures[unit];
          if (!t.used) continue;
          const auto found = (g_debug & kDebugCopyFromRam) ? copy_tokens.end() : copy_tokens.find(t.addr);
          if (found != copy_tokens.end()) found->second->last_used = counters.frames;
          auto& obj = texture(t, unit, found == copy_tokens.end() ? nullptr : found->second.get());
          if (obj.has_palette) GXLoadTlut(&obj.palette, unit);
          GXLoadTexObj(&obj.texture, static_cast<GXTexMapID>(unit));
        }
        // GX SDK texture loads may emit cached SDK state. Captured draw state is
        // therefore restored last, before canonical direct vertex bytes.
        fifo::write_data(d.state.data(), uint32_t(d.state.size()));
        fifo::write_u8(d.primitive); fifo::write_u16(d.vertex_count);
        fifo::write_data(d.vertex_data, d.vertex_bytes);
        fifo::finish_draw();
        ++counters.draws;
      } else {
        if (std::find(frame.skipped_copies.begin(), frame.skipped_copies.end(), cmd.index) != frame.skipped_copies.end()) return;
        const auto& c = frame.copies[cmd.index];
        bp(BP_ZMODE, c.zmode); bp(BP_BLENDMODE, c.blendmode);
        bp(BP_CONSTANTALPHA, c.dstalpha); bp(BP_ZCOMPARE, c.zcontrol);
        GXSetCopyClear(clear_color(c.clear_color), c.clear_z);
        if (!c.to_xfb && !(g_debug & kDebugSkipCopies)) {
          auto& token = copy_tokens[c.dest_addr];
          if (!token) token = std::make_unique<CopyToken>();
          const unsigned shrink = (g_debug & kDebugCopyHalfSize) ? 2 : 1;
          const uint32_t dst_w = c.src_w / shrink, dst_h = c.src_h / shrink, fmt = uint32_t(copy_format(c));
          // Aurora keeps one texture per (destination, size, format): a new size at the same address would add
          // a texture and keep the old one, so the old one goes first.
          if (token->format != ~0u && (token->width != dst_w || token->height != dst_h || token->format != fmt))
            GXDestroyCopyTex(token.get());
          token->width = dst_w; token->height = dst_h; token->format = fmt; token->last_used = counters.frames;
          GXSetTexCopySrc(uint16_t(c.src_x), uint16_t(c.src_y), uint16_t(c.src_w), uint16_t(c.src_h));
          GXSetTexCopyDst(uint16_t(dst_w), uint16_t(dst_h), copy_format(c), GX_FALSE);
          // Partial-rectangle clears are scissored to the copy rect (aurora-switch-partial-clear.patch).
          GXCopyTex(token.get(), c.clear ? GX_TRUE : GX_FALSE);
          ++counters.texture_copies;
        }
        // GXCopyDisp is a stub. The validated final full-size XFB is presented
        // by aurora_end_frame; the next aurora_begin_frame performs its full
        // clear using the copy-time values just loaded. Never clear before blit.
      }
    };
    aurora_bridge::prepare_frame(input, {options.logical_width, options.logical_height}, prepared, emit);
    if (dump_armed && input.scene_major == dump_major && input.scene_minor == dump_minor && ++dump_seen == dump_after) {
      dump_armed = false;
      write_draw_dump(input, frame);
    }
    if (dump_now) {
      dump_now = false;
      const auto slash = debug_path.find_last_of('/');
      const std::string saved = dump_path;
      dump_path = (slash == std::string::npos ? std::string() : debug_path.substr(0, slash + 1)) + "draws-now.txt";
      write_draw_dump(input, frame);
      dump_path = saved;
    }
    // Copy-history coverage warnings (MELEE_COPY_HISTORY=1 / copy_history=1). Diagnostic only: it compares
    // every copy-sampling draw's texture bytes with the copy, ~15% of a 4-player frame on the render thread.
    static const bool copy_history_on = [] { const char* v = std::getenv("MELEE_COPY_HISTORY"); return v && v[0] == '1'; }();
    if (copy_history_on) aurora_bridge::advance_copy_history_in_place(frame, copy_history);
    if (g_debug & kDebugShowCopies) show_copies(frame);
    if (g_debug & kDebugShowTextures) show_textures(input); else if (!debug_textures.empty()) debug_textures.clear();
    if (overlay) overlay();
    // Idle copy tokens: destroy after the frame's own draws are queued (GXDestroyCopyTex goes through the FIFO,
    // after anything that sampled it), once a second.
    if (counters.frames % 60 == 0)
      for (auto it = copy_tokens.begin(); it != copy_tokens.end();) {
        if (counters.frames - it->second->last_used > kCopyTokenIdleFrames) { GXDestroyCopyTex(it->second.get()); it = copy_tokens.erase(it); }
        else ++it;
      }
    aurora_end_frame(); // Drains FIFO; uploaded texture source bytes are now safe.
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
void AuroraBackend::set_overlay(std::function<void()> draw) { impl_->require_owner(); impl_->overlay = std::move(draw); }
void AuroraBackend::set_draw_dump(std::string path, uint8_t scene_major, uint8_t scene_minor, unsigned after) {
  impl_->dump_path = std::move(path); impl_->dump_major = scene_major; impl_->dump_minor = scene_minor;
  impl_->dump_after = after ? after : 1; impl_->dump_seen = 0; impl_->dump_armed = true;
}
void AuroraBackend::set_debug_file(std::string path) { impl_->debug_path = std::move(path); }
} // namespace gx
