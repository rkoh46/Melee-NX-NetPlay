// Rollback snapshots for the native engine that cost what a frame writes, not what the state holds.
//
// The state a rollback must save is 44.8 MiB (all of main memory plus the image's writable data),
// but a frame of a match writes 130 to 260 of its 11,500 pages and a whole match touches about 2%.
// So: every page of the state starts read-only. The first write to a page faults; the handler keeps
// the page's bytes as they were (its "base") and adds the page to the hot list, and from then on
// the page stays writable. A save copies the hot pages, nothing else. A restore copies them back:
// from the slot for pages that were hot when it was saved, from their base for pages that became
// hot later (they had not changed up to then). The hot list only grows, so slot N holds the first
// count(N) pages of the list and no bookkeeping per page is needed.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_savestate.h"
#include <cstdint>
#include <string>
#include <vector>

namespace mu_rollback {
// Starts tracking the spans (what native_savestate::subtract gives for the game's snapshot ranges
// and exclusions) with room for `slots` saved frames.
bool begin(const std::vector<native_savestate::Span>& spans, int slots, bool keep_reference, std::string* error);
void end();
bool active();
// Keeps the current state under `frame` (reusing that frame's slot, else a free one, else the oldest).
void save(int32_t frame);
// Brings back the state kept under `frame` and forgets the saves of later frames. False when not kept.
bool restore(int32_t frame);
bool has(int32_t frame);

size_t tracked_pages();
size_t hot_pages();
bool overflowed();              // more pages became hot than there is room for: the saves are not valid
// With keep_reference: how many tracked pages that are not hot differ from their bytes at begin().
// Anything but 0 means a write went unnoticed.
size_t missed_writes();
}
