// See snapshot_store.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "snapshot_store.h"
#include "../net_host.h"
#include "../rollback.h"

#include <algorithm>
#include <chrono>
#include <map>

namespace snapshot_store {
namespace {
Log say = nullptr;
void (*sync_fn)() = nullptr;
bool is_running = false, failed = false;
std::map<int32_t, bool> keys;       // what the store holds, as the store keeps it (restore drops later keys)
Stats st;
double ms_since(std::chrono::steady_clock::time_point t) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(); }
}  // namespace

void configure(Log log, void (*sync)()) { say = log; sync_fn = sync; }

bool running() { return is_running; }

bool keep(int32_t key, int slots, const char* who) {
  if (sync_fn) sync_fn();
  const auto t = std::chrono::steady_clock::now();
  if (!is_running) {
    if (mu_rollback::active()) { if (say) say("%s: the snapshot store is in use by the netplay host; not kept", who); return false; }
    std::string error;
    if (!mu_rollback::begin(net_host::snapshot_spans(true), slots, false, &error)) {
      if (say && !failed) say("%s: cannot start the snapshot store: %s", who, error.c_str());
      failed = true;
      return false;
    }
    is_running = true; failed = false; keys.clear();
    if (say) say("%s: snapshot store tracking %zu pages (%.1f MiB), %d states", who, mu_rollback::tracked_pages(), mu_rollback::tracked_pages() / 256.0, slots);
  }
  mu_rollback::save(key);
  if (mu_rollback::overflowed()) { if (say) say("%s: more pages written than the store can keep; states are not valid", who); release("overflow"); return false; }
  // The store reuses the oldest slot when full: forget keys it no longer has.
  keys[key] = true;
  for (auto it = keys.begin(); it != keys.end();) it = mu_rollback::has(it->first) ? std::next(it) : keys.erase(it);
  const double ms = ms_since(t);
  ++st.keeps; st.keep_ms_sum += ms; st.keep_ms_max = std::max(st.keep_ms_max, ms);
  return true;
}

bool load(int32_t key, const char* who) {
  if (!is_running || !mu_rollback::has(key)) return false;
  if (sync_fn) sync_fn();
  const auto t = std::chrono::steady_clock::now();
  if (!mu_rollback::restore(key)) return false;
  keys.erase(keys.upper_bound(key), keys.end());
  const double ms = ms_since(t);
  ++st.loads; st.load_ms_sum += ms; st.load_ms_max = std::max(st.load_ms_max, ms);
  (void)who;
  return true;
}

bool has(int32_t key) { return is_running && mu_rollback::has(key); }

bool newest_at_or_before(int32_t target, int32_t* key) {
  if (!is_running) return false;
  auto it = keys.upper_bound(target);
  if (it == keys.begin()) return false;
  *key = std::prev(it)->first;
  return true;
}

void release(const char* why) {
  if (!is_running) return;
  if (sync_fn) sync_fn();
  mu_rollback::end();
  is_running = false; keys.clear();
  if (say) say("snapshot store released (%s): %llu keeps (avg %.2f ms, worst %.2f), %llu loads (avg %.2f ms, worst %.2f)", why,
               static_cast<unsigned long long>(st.keeps), st.keeps ? st.keep_ms_sum / double(st.keeps) : 0.0, st.keep_ms_max,
               static_cast<unsigned long long>(st.loads), st.loads ? st.load_ms_sum / double(st.loads) : 0.0, st.load_ms_max);
}

Stats stats() { return st; }
}  // namespace snapshot_store
