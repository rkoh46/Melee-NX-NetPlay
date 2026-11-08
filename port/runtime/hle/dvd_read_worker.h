// Portable asynchronous disc-read worker used by DVD HLE.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace hle::dvd_detail {

enum class ReadStatus : uint8_t { queued, reading, complete, failed, cancelled };

struct ReadJob {
  ReadJob(uint32_t offset, uint32_t size) : disc_offset(offset), length(size) {}

  uint32_t disc_offset;
  uint32_t length;
  std::vector<uint8_t> data;
  std::atomic<ReadStatus> status{ReadStatus::queued};
};

class ReadWorker {
 public:
  using Read = std::function<bool(uint32_t offset, void* dst, uint32_t size)>;
  using Job = std::shared_ptr<ReadJob>;

  explicit ReadWorker(Read read);
  ~ReadWorker();
  ReadWorker(const ReadWorker&) = delete;
  ReadWorker& operator=(const ReadWorker&) = delete;

  void initialize();
  Job submit(uint32_t offset, uint32_t size);
  void wait(const Job& job);
  // Cancels jobs that have not started. A job already inside Read finishes into its own data
  // vector before this call joins; no read receives guest-owned storage.
  void shutdown() noexcept;
  bool accepting() const;

  static bool finished(ReadStatus status);

 private:
  void run() noexcept;

  Read read_;
  mutable std::mutex mutex_;
  std::mutex lifecycle_mutex_;
  std::condition_variable work_cv_;
  std::condition_variable done_cv_;
  std::deque<Job> queue_;
  std::thread thread_;
  bool accepting_ = false;
  bool stop_requested_ = false;
};

}  // namespace hle::dvd_detail
