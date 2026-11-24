// The Practice menu draws two kinds of pictures of its own (the game's font from the disc, stock icons) through ImGui
// 1.92's ImTextureData. This app's ImGui (Aurora's, 1.91.9) has no such type: this is the small part of it the menu
// uses, uploaded once through Aurora (aurora_imgui_add_texture) the first time the menu draws with it on the render
// thread. AddImage/AddImageQuad then take the texture's id, which 1.91's draw list accepts as is.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#include <aurora/imgui.h>

#include <cstdint>
#include <vector>

enum ImTextureFormat { ImTextureFormat_RGBA32 = 0 };
struct ImTextureRect { unsigned short x, y, w, h; };
using ImTextureRef = ImTextureID;
struct ImTextureData {
  int Width = 0, Height = 0;
  std::vector<uint8_t> Pixels;
  ImTextureRect UsedRect{0, 0, 0, 0};
  ImTextureID id = ImTextureID(0);
  void Create(ImTextureFormat, int w, int h) { Width = w; Height = h; Pixels.assign(size_t(w) * size_t(h) * 4, 0); id = ImTextureID(0); }
  uint8_t* GetPixels() { return Pixels.data(); }
  ImTextureRef GetTexRef() const { return id; }
};

namespace gx {
// Once a frame before drawing `tex` (render thread): uploads it the first time.
inline void keep_user_texture(ImTextureData* tex, ImGuiContext*& registered_in) {
  registered_in = ImGui::GetCurrentContext();
  if (tex && !tex->id && tex->Width > 0) tex->id = aurora_imgui_add_texture(uint32_t(tex->Width), uint32_t(tex->Height), tex->Pixels.data());
}
}  // namespace gx
