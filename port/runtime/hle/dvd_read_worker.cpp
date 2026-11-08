// Portable asynchronous disc-read worker used by DVD HLE.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "dvd_read_worker.h"

#include <utility>

namespace hle::dvd_detail {

ReadWorker::ReadWorker(Read read) : read_(std::move(read)) {}

ReadWorker::~ReadWorker() { shutdown(); }

bool ReadWorker::finished(ReadStatus status) {
  return status == ReadStatus::complete || status == ReadStatus::failed ||
         status == ReadStatus::cancelled;
}

void ReadWorker::initialize() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  std::lock_guard<std::mutex> lock(mutex_);
  if (accepting_) return;

  // shutdown() holds lifecycle_mutex_ until joining, so a non-accepting worker cannot still
  // be winding down here. A stopped worker is intentionally reusable for another guest run.
  stop_requested_ = false;
  thread_ = std::thread(&ReadWorker::run, this);
  accepting_ = true;
}

ReadWorker::Job ReadWorker::submit(uint32_t offset, uint32_t size) {
  auto job = std::make_shared<ReadJob>(offset, size);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_) return {};
    queue_.push_back(job);
  }
  work_cv_.notify_one();
  return job;
}

void ReadWorker::wait(const Job& job) {
  if (!job) return;
  std::unique_lock<std::mutex> lock(mutex_);
  done_cv_.wait(lock, [&] {
    return finished(job->status.load(std::memory_order_acquire));
  });
}

void ReadWorker::shutdown() noexcept {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_ = false;
    stop_requested_ = true;
    for (const Job& job : queue_)
      job->status.store(ReadStatus::cancelled, std::memory_order_release);
    queue_.clear();
  }
  work_cv_.notify_all();
  done_cv_.notify_all();

  // A read already in progress is allowed to finish into its private buffer. Joining here is
  // the teardown barrier; queued reads never start, and no worker ever receives guest memory.
  if (thread_.joinable()) thread_.join();
}

bool ReadWorker::accepting() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return accepting_;
}

void ReadWorker::run() noexcept {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      work_cv_.wait(lock, [&] { return stop_requested_ || !queue_.empty(); });
      if (stop_requested_ && queue_.empty()) return;
      job = queue_.front();
      queue_.pop_front();
      job->status.store(ReadStatus::reading, std::memory_order_release);
    }

    bool ok = false;
    try {
      job->data.resize(job->length);
      uint8_t empty_read = 0;
      void* destination = job->length ? static_cast<void*>(job->data.data()) : &empty_read;
      ok = read_(job->disc_offset, destination, job->length);
    } catch (...) {
      ok = false;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      job->status.store(ok ? ReadStatus::complete : ReadStatus::failed,
                        std::memory_order_release);
    }
    done_cv_.notify_all();
  }
}

}  // namespace hle::dvd_detail
