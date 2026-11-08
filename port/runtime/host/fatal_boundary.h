// Headless failure ownership: workers relay failures; the simulation thread
// unwinds its locks, joins all producers, then publishes fatal evidence.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace host {
class HostFatal : public std::runtime_error { public: using std::runtime_error::runtime_error; };
inline thread_local unsigned fatal_boundary_depth = 0;
inline std::atomic<bool> headless_fatal_mode{false};
class ScopedFatalBoundary {
public:
  explicit ScopedFatalBoundary(bool enabled = true) : enabled_(enabled) { if (enabled_) ++fatal_boundary_depth; }
  ~ScopedFatalBoundary() { if (enabled_) --fatal_boundary_depth; }
  ScopedFatalBoundary(const ScopedFatalBoundary&) = delete;
  ScopedFatalBoundary& operator=(const ScopedFatalBoundary&) = delete;
private:
  bool enabled_;
};
inline bool fatal_boundary_active() { return fatal_boundary_depth != 0; }
inline void enable_headless_fatal_mode(bool enabled) { headless_fatal_mode.store(enabled); }

class BackgroundFailureRelay {
public:
  void report(std::string reason) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      if (pending_) return;
      reason_ = std::move(reason);
      pending_ = true;
    } catch (...) { std::terminate(); }
  }
  bool pending() const { std::lock_guard<std::mutex> lock(mutex_); return pending_; }
  bool take(std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_) return false;
    reason = std::move(reason_); pending_ = false;
    return true;
  }
private:
  mutable std::mutex mutex_;
  bool pending_ = false;
  std::string reason_;
};
inline BackgroundFailureRelay background_failure_relay;
inline bool background_failed() { return background_failure_relay.pending(); }
inline bool take_background_failure(std::string& reason) { return background_failure_relay.take(reason); }
template <typename Function> void run_background_task(Function&& task) noexcept {
  const bool enabled = headless_fatal_mode.load();
  ScopedFatalBoundary boundary(enabled);
  try { std::forward<Function>(task)(); }
  catch (const std::exception& e) {
    if (!enabled) std::terminate();
    background_failure_relay.report(e.what());
  } catch (...) {
    if (!enabled) std::terminate();
    background_failure_relay.report("unknown background task exception");
  }
}
}  // namespace host
