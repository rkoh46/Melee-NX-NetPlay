// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "gx_d3d12.h"
#include <memory>
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;
namespace gx {
void load_pc_settings(D3D12Options& options, int& volume);
class PcSettingsUI {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  PcSettingsUI(void* window, ID3D12Device* device, ID3D12CommandQueue* queue, const D3D12Options& options);
  ~PcSettingsUI();
  bool begin(D3D12Options& options); // returns true when render configuration changed
  void draw(ID3D12GraphicsCommandList* list);
};
}
