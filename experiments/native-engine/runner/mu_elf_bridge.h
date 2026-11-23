// Loads the game image (an AArch64 Linux ELF shared object linked at 0x82800000) without a dynamic
// linker, on Horizon or on Linux. The namespace and the interface are the ones melee-unlocked's
// headless macOS bridge uses (port/mac/elf_loader.h), so its host code (port/linux/host_services.cpp
// with patches/0002) drives this loader through the code paths it already has for that bridge.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "mu_host.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <sys/types.h>
#include <time.h>

#ifndef MAP_FAILED
#define MAP_FAILED ((void*)-1)
#endif
#ifdef __SWITCH__
// newlib has neither; the host code uses pread for disc reads and timegm for the clock.
extern "C" ssize_t pread(int fd, void* buffer, size_t bytes, off_t offset);
extern "C" time_t timegm(struct tm* tm);
#endif

// Called by the host before it lets the kernel write into game memory (disc reads): pages that
// the rollback snapshot is write-tracking are made writable first (rollback.cpp).
extern "C" void mu_host_prewrite(void* address, size_t bytes);

namespace linux_host { void rewind_frames(uint32_t frames); void skip_draws(uint32_t frames); void set_tables_hook(void (*hook)(MuHostApi*, MuGameApi*)); void set_poll_ticks(uint64_t ticks); }   // added to the host by patches/0002 (rollback test)

namespace mac_elf {
bool reserve_window(std::string* error);
bool activate_mem1(std::string* error);          // maps the 40 MiB main memory at 0x80000000
void deactivate_mem1();
void* map_fixed(uintptr_t address, size_t bytes); // zeroed read/write pages; MAP_FAILED on failure
void unmap_fixed(uintptr_t address, size_t bytes);
// Use this image instead of reading the path given to Image::load (the image embedded in the NRO).
void set_embedded_image(const uint8_t* data, size_t size);

class Image {
 public:
  bool load(const std::string& path, std::string* error);
  void unload();
  void* symbol(const char* name) const;
  const std::vector<MuStateRegion>& mappings() const { return mappings_; }
  const std::vector<MuStateRegion>& writable() const { return writable_; }
  uint32_t size() const { return size_; }
 private:
  std::vector<MuStateRegion> mappings_, writable_;
  std::vector<uintptr_t> fini_array_;
  uintptr_t fini_ = 0, symtab_ = 0, strtab_ = 0;
  size_t symbol_count_ = 0, string_bytes_ = 0;
  uint32_t size_ = 0;
  bool initialized_ = false, mapped_ = false;
};
} // namespace mac_elf
