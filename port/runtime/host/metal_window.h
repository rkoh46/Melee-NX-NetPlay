// Host window bridge installed by the opt-in Metal executable.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace host {
class MetalFrontend;

// Non-owning attachment. The caller must detach before destroying frontend.
void metal_window_attach(MetalFrontend& frontend);
void metal_window_detach(MetalFrontend& frontend) noexcept;
} // namespace host
