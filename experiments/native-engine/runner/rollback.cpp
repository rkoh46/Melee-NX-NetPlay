// See rollback.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "rollback.h"
#include "dirtytrack.h"
#include "mu_elf_bridge.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

extern "C" void mu_host_prewrite(void* address, size_t bytes) { dirtytrack_prewrite(address, bytes); }

namespace mu_rollback {
namespace {
constexpr size_t kPage = DIRTYTRACK_PAGE;
constexpr size_t kWindowPages = (DIRTYTRACK_END - DIRTYTRACK_BASE) / kPage;
constexpr size_t kMaxHot = 4096;                 // 16 MiB of hot pages; a match needs 250 to 450

struct Segment { uint16_t offset, size; };       // the part of a page that belongs to the state
struct PageInfo { uint32_t first_segment; uint16_t segments; bool tracked; };
std::vector<PageInfo> page_info;                 // per page of the window
std::vector<Segment> segment_list;
std::vector<std::pair<uintptr_t, size_t>> runs;  // contiguous tracked page runs, as protected
size_t tracked_count = 0;

// Written by the fault handler: plain arrays, allocated before tracking starts.
uintptr_t* hot_page = nullptr;
uint8_t* base_bytes = nullptr;
volatile size_t hot_count = 0;
volatile bool overflow = false;

struct Slot { int32_t frame = 0; bool used = false; size_t count = 0; uint64_t order = 0; std::vector<uint8_t> bytes; };
std::vector<Slot> slot_list;
uint64_t next_order = 1;
std::vector<uint8_t> reference;                  // optional: every tracked page as it was at begin()
bool is_active = false;

size_t index_of(uintptr_t page) { return (page - DIRTYTRACK_BASE) / kPage; }

// In the fault handler: no allocation, no locks.
void on_first_write(uintptr_t page) {
  const size_t n = hot_count;
  if (n >= kMaxHot) { overflow = true; return; }
  std::memcpy(base_bytes + n * kPage, reinterpret_cast<void*>(page), kPage);
  hot_page[n] = page;
  hot_count = n + 1;
}

void copy_back(uintptr_t page, const uint8_t* from) {
  const PageInfo& info = page_info[index_of(page)];
  for (uint16_t i = 0; i < info.segments; ++i) {
    const Segment s = segment_list[info.first_segment + i];
    std::memcpy(reinterpret_cast<uint8_t*>(page) + s.offset, from + s.offset, s.size);
  }
}
} // namespace

bool active() { return is_active; }
size_t tracked_pages() { return tracked_count; }
size_t hot_pages() { return hot_count; }
bool overflowed() { return overflow; }

bool begin(const std::vector<native_savestate::Span>& spans, int slots, bool keep_reference, std::string* error) {
  end();
  page_info.assign(kWindowPages, PageInfo{0, 0, false});
  segment_list.clear(); runs.clear(); tracked_count = 0;
  // Spans are sorted by address and do not overlap, so the segments of a page come out in order.
  for (const auto& span : spans) {
    const uintptr_t begin_address = reinterpret_cast<uintptr_t>(span.address), end_address = begin_address + span.size;
    if (begin_address < DIRTYTRACK_BASE || end_address > DIRTYTRACK_END) { if (error) *error = "a snapshot range lies outside 0x80000000..0x84000000"; return false; }
    for (uintptr_t at = begin_address; at < end_address;) {
      const uintptr_t page = at & ~uintptr_t(kPage - 1), next = std::min(end_address, page + kPage);
      PageInfo& info = page_info[index_of(page)];
      if (!info.tracked) { info.tracked = true; info.first_segment = uint32_t(segment_list.size()); ++tracked_count; }
      segment_list.push_back({uint16_t(at - page), uint16_t(next - at)});
      ++info.segments;
      at = next;
    }
  }
  hot_page = static_cast<uintptr_t*>(std::malloc(kMaxHot * sizeof(uintptr_t)));
  base_bytes = static_cast<uint8_t*>(std::malloc(kMaxHot * kPage));
  if (!hot_page || !base_bytes) { if (error) *error = "no memory for the hot page store"; end(); return false; }
  std::memset(base_bytes, 0, kMaxHot * kPage);          // touch it now, not inside a fault
  hot_count = 0; overflow = false;
  slot_list.assign(size_t(slots), Slot{});
  for (auto& slot : slot_list) slot.bytes.resize(512 * kPage);
  if (keep_reference) {
    reference.resize(kWindowPages * kPage);             // sparse use: only tracked pages are filled
    for (size_t i = 0; i < kWindowPages; ++i)
      if (page_info[i].tracked) std::memcpy(&reference[i * kPage], reinterpret_cast<void*>(DIRTYTRACK_BASE + i * kPage), kPage);
  }
  if (dirtytrack_install(on_first_write)) { if (error) *error = dirtytrack_error(); end(); return false; }
  for (size_t i = 0; i < kWindowPages;) {
    if (!page_info[i].tracked) { ++i; continue; }
    size_t j = i;
    while (j < kWindowPages && page_info[j].tracked) ++j;
    const uintptr_t address = DIRTYTRACK_BASE + i * kPage; const size_t size = (j - i) * kPage;
    if (dirtytrack_protect(address, size)) { if (error) *error = dirtytrack_error(); end(); return false; }
    runs.push_back({address, size});
    i = j;
  }
  is_active = true;
  return true;
}

void end() {
  for (const auto& run : runs) dirtytrack_release(run.first, run.second);
  runs.clear();
  std::free(hot_page); std::free(base_bytes); hot_page = nullptr; base_bytes = nullptr;
  slot_list.clear(); reference.clear(); reference.shrink_to_fit();
  is_active = false;
}

bool has(int32_t frame) {
  for (const auto& slot : slot_list) if (slot.used && slot.frame == frame) return true;
  return false;
}

void save(int32_t frame) {
  if (!is_active) return;
  Slot* target = nullptr;
  for (auto& slot : slot_list) if (slot.used && slot.frame == frame) target = &slot;
  if (!target) for (auto& slot : slot_list) if (!slot.used) { target = &slot; break; }
  if (!target) for (auto& slot : slot_list) if (!target || slot.order < target->order) target = &slot;
  const size_t count = hot_count;
  if (target->bytes.size() < count * kPage) target->bytes.resize((count + 256) * kPage);
  uint8_t* out = target->bytes.data();
  for (size_t i = 0; i < count; ++i) std::memcpy(out + i * kPage, reinterpret_cast<void*>(hot_page[i]), kPage);
  target->frame = frame; target->used = true; target->count = count; target->order = next_order++;
}

bool restore(int32_t frame) {
  Slot* source = nullptr;
  for (auto& slot : slot_list) if (slot.used && slot.frame == frame) source = &slot;
  if (!source) return false;
  const size_t count = hot_count;
  for (size_t i = 0; i < source->count; ++i) copy_back(hot_page[i], source->bytes.data() + i * kPage);
  for (size_t i = source->count; i < count; ++i) copy_back(hot_page[i], base_bytes + i * kPage);   // not written yet at that frame
  for (auto& slot : slot_list) if (slot.used && slot.frame > frame) slot.used = false;
  return true;
}

namespace { std::vector<uint8_t> remembered; std::vector<uintptr_t> remembered_pages; }
void remember() {
  const size_t count = hot_count;
  remembered.resize(count * kPage);
  remembered_pages.assign(hot_page, hot_page + count);
  for (size_t i = 0; i < count; ++i) std::memcpy(&remembered[i * kPage], reinterpret_cast<void*>(hot_page[i]), kPage);
}
std::vector<std::pair<uintptr_t, uint32_t>> differences_from_remembered(size_t limit) {
  std::vector<std::pair<uintptr_t, uint32_t>> out;
  // Pages that became hot after remember() were unchanged then, so their base bytes are what they held.
  const size_t count = hot_count;
  for (size_t i = 0; i < count && out.size() < limit; ++i) {
    const uint8_t* then = i < remembered_pages.size() ? &remembered[i * kPage] : base_bytes + i * kPage;
    const uint8_t* live = reinterpret_cast<const uint8_t*>(hot_page[i]);
    const PageInfo& info = page_info[index_of(hot_page[i])];
    for (uint16_t k = 0; k < info.segments && out.size() < limit; ++k) {
      const Segment seg = segment_list[info.first_segment + k];
      for (uint32_t at = seg.offset; at < uint32_t(seg.offset) + seg.size;) {
        if (live[at] == then[at]) { ++at; continue; }
        uint32_t end = at + 1, gap = 0;
        while (end < uint32_t(seg.offset) + seg.size && gap < 8) { if (live[end] != then[end]) gap = 0; else ++gap; ++end; }
        out.push_back({hot_page[i] + at, end - gap - at});
        at = end;
        if (out.size() >= limit) break;
      }
    }
  }
  return out;
}

size_t missed_writes() {
  if (reference.empty()) return 0;
  std::vector<bool> hot(kWindowPages, false);
  const size_t count = hot_count;
  for (size_t i = 0; i < count; ++i) hot[index_of(hot_page[i])] = true;
  size_t missed = 0;
  for (size_t i = 0; i < kWindowPages; ++i) {
    if (!page_info[i].tracked || hot[i]) continue;
    const uint8_t* live = reinterpret_cast<const uint8_t*>(DIRTYTRACK_BASE + i * kPage);
    for (uint16_t k = 0; k < page_info[i].segments; ++k) {
      const Segment s = segment_list[page_info[i].first_segment + k];
      if (std::memcmp(live + s.offset, &reference[i * kPage + s.offset], s.size)) { ++missed; break; }
    }
  }
  return missed;
}
} // namespace mu_rollback
