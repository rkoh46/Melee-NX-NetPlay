// Play mode's log: no line is written from the thread that produced it while the game runs. The
// simulation thread printed about 1,400 lines in an online match (the game's own, the netplay host's,
// the netplay client's), each one a write to the nxlink socket and to a file on the SD card. Here a
// line is copied into a buffer under a short lock and a writer thread does the writing: to the console
// (nxlink) ten times a second, to the file only between matches, so the SD card is not touched while
// fighters are simulated. Lines printed with fprintf(stderr, ...) by code that cannot be changed (the
// netplay object) arrive through the linker's --wrap=fprintf.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <pthread.h>
#include <unistd.h>

namespace fe {
namespace {
std::mutex mutex;
std::string pending;                       // whole lines, each ending in a newline
std::atomic<bool> running{false}, quit{false}, in_match{false};
std::atomic<uint64_t> lines{0}, dropped{0}, file_bytes_in_match{0};
pthread_t thread; bool thread_started = false;
constexpr size_t kMostPending = 4u << 20, kMostHeldForFile = 1u << 20;

void* writer(void*) {
  std::string batch, for_file;
  batch.reserve(1u << 16);
  for (;;) {
    const bool last = quit.load();
    { std::lock_guard<std::mutex> lock(mutex); batch.swap(pending); }
    if (!batch.empty()) {
      log_write(batch.data(), batch.size(), true, false);
      for_file += batch;
      batch.clear();
    }
    const bool fighting = in_match.load(std::memory_order_relaxed);
    if (!for_file.empty() && (last || !fighting || for_file.size() > kMostHeldForFile)) {
      if (fighting && !last) file_bytes_in_match += for_file.size();
      log_write(for_file.data(), for_file.size(), false, true);
      for_file.clear();
    }
    if (last) break;
    usleep(100000);
  }
  return nullptr;
}
}  // namespace

void log_queue_start() {
  if (thread_started) return;
  { std::lock_guard<std::mutex> lock(mutex); pending.reserve(1u << 16); }
  quit.store(false); lines.store(0); dropped.store(0); file_bytes_in_match.store(0);
  if (pthread_create(&thread, nullptr, writer, nullptr) != 0) return;     // lines are then written where they are made, as before
  thread_started = true;
  running.store(true);
}

void log_queue_stop() {
  if (!thread_started) return;
  running.store(false);                    // from here on a line is written by its own thread again
  quit.store(true);
  pthread_join(thread, nullptr);
  thread_started = false;
  say("log: %llu lines went through the log thread while the game ran (%llu dropped with the buffer full); %llu bytes had to go to the file during a match",
      (unsigned long long)lines.load(), (unsigned long long)dropped.load(), (unsigned long long)file_bytes_in_match.load());
}

void log_queue_match(bool fighting) { in_match.store(fighting, std::memory_order_relaxed); }
bool log_queue_running() { return running.load(std::memory_order_relaxed); }
uint64_t log_queue_lines() { return lines.load(std::memory_order_relaxed); }

bool log_queue_push(const char* line) {
  if (!running.load(std::memory_order_relaxed)) return false;
  const size_t length = std::strlen(line);
  std::lock_guard<std::mutex> lock(mutex);
  if (pending.size() + length + 1 > kMostPending) { ++dropped; return true; }
  pending.append(line, length);
  if (!length || line[length - 1] != '\n') pending.push_back('\n');
  ++lines;
  return true;
}
}  // namespace fe

// Every fprintf of the program (-Wl,--wrap=fprintf). Only lines for stderr while the queue runs are taken.
extern "C" int __wrap_fprintf(FILE* file, const char* format, ...) {
  va_list args; va_start(args, format);
  int n;
  if (file == stderr && fe::log_queue_running()) {
    char line[2048];
    n = std::vsnprintf(line, sizeof line, format, args);
    fe::log_queue_push(line);
  } else {
    n = std::vfprintf(file, format, args);
  }
  va_end(args);
  return n;
}
