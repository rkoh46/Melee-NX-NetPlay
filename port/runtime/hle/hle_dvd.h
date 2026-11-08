// DVD HLE lifecycle shared by the simulation host and the guest-call implementations.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace hle {

// Repeated calls are safe. A new init after shutdown starts a fresh callback generation.
void dvd_init();
void dvd_poll();
// Reject/cancel queued reads, let an in-flight host read finish only into private storage, then
// join it. No pending guest write or callback is delivered by or after this shutdown generation.
void dvd_shutdown() noexcept;

}  // namespace hle
