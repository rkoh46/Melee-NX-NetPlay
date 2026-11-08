#pragma once
#include "gx_d3d12.h"
namespace gx {
// Window and D3D12 resources belong to the worker; simulation supplies owned frames.
std::unique_ptr<Backend> create_threaded_backend(const D3D12Options& options, bool visible);
}
