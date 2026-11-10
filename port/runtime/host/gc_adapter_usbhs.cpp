// Official / Mayflash "GameCube Controller Adapter for Wii U" (WUP-028) on the Switch, read directly
// over the USB host service (usb:hs) instead of through the system's HID, at 1000 Hz.
//
// The adapter's interrupt IN endpoint asks the host to poll it every 8 ms (125 Hz). On a PC players
// "overclock" it by making the driver poll every 1 ms, and the adapter then reports 1000 times a
// second. usb:hs opens an endpoint from a descriptor the caller passes in, so the same thing is done
// here by opening endpoint 0x81 with bInterval = 1. A dedicated thread posts back-to-back 37-byte
// reads; the frame loop reads the newest report (gcadapter_poll), and the measured report rate is in
// the log and in gcadapter_status. Protocol as in Dolphin's GCAdapter: 0x13 on endpoint 0x02 starts
// the stream, reports are 0x21 + 9 bytes per port, 0x11 + 4 bytes sets rumble.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "host.h"
#include "window.h"

#include <switch.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace host {
namespace {

enum : uint16_t {
  PAD_LEFT = 0x0001, PAD_RIGHT = 0x0002, PAD_DOWN = 0x0004, PAD_UP = 0x0008, PAD_Z = 0x0010, PAD_R = 0x0020, PAD_L = 0x0040,
  PAD_A = 0x0100, PAD_B = 0x0200, PAD_X = 0x0400, PAD_Y = 0x0800, PAD_START = 0x1000,
};
constexpr uint16_t ADAPTER_VID = 0x057E, ADAPTER_PID = 0x0337;

bool g_service = false;
UsbHsClientIfSession g_if{};
UsbHsClientEpSession g_in{}, g_out{};
bool g_open = false;
Thread g_thread{};
std::atomic<bool> g_running{false};
std::mutex g_mutex;
uint8_t g_report[37] = {};
bool g_have_report = false;
uint64_t g_next_scan = 0;
bool g_logged_missing = false;
uint8_t* g_in_buf = nullptr;    // usb:hs transfer buffers must be page-aligned
uint8_t* g_out_buf = nullptr;
struct Origin { bool set = false; uint8_t sx = 128, sy = 128, cx = 128, cy = 128, tl = 0, tr = 0; } g_origin[4];
std::atomic<uint8_t> g_rumble[4]{};
std::atomic<bool> g_rumble_dirty{false};
std::atomic<uint32_t> g_ports{0};
std::atomic<uint32_t> g_rate_hz{0};
int g_interval_ms = 0;

uint64_t now_ms() { return armTicksToNs(armGetSystemTick()) / 1000000ull; }

bool post(UsbHsClientEpSession* ep, uint8_t* buf, uint32_t size, uint32_t* got) {
  return R_SUCCEEDED(usbHsEpPostBuffer(ep, buf, size, got));
}

void reader_main(void*) {
  g_out_buf[0] = 0x13;
  uint32_t n = 0;
  if (!post(&g_out, g_out_buf, 1, &n)) log("gc adapter: start command failed");
  uint64_t window_start = now_ms(); uint32_t reports = 0;
  int failures = 0;
  while (g_running.load(std::memory_order_relaxed)) {
    uint32_t got = 0;
    if (post(&g_in, g_in_buf, 37, &got)) {
      failures = 0;
      if (got == 37 && g_in_buf[0] == 0x21) {
        std::lock_guard<std::mutex> lk(g_mutex);
        std::memcpy(g_report, g_in_buf, 37);
        g_have_report = true;
        ++reports;
      }
    } else if (++failures > 50) {
      log("gc adapter: reads failing, adapter disconnected");
      break;
    }
    if (g_rumble_dirty.exchange(false)) {
      g_out_buf[0] = 0x11;
      for (int i = 0; i < 4; ++i) g_out_buf[1 + i] = g_rumble[i];
      post(&g_out, g_out_buf, 5, &n);
    }
    const uint64_t t = now_ms();
    if (t - window_start >= 1000) {
      const uint32_t hz = uint32_t(reports * 1000ull / (t - window_start));
      const uint32_t prev = g_rate_hz.exchange(hz);
      if (prev == 0 || (hz < prev * 9 / 10) || (hz > prev * 11 / 10)) log("gc adapter: %u reports/s (endpoint polled every %d ms)", hz, g_interval_ms);
      window_start = t; reports = 0;
    }
  }
  g_running.store(false);
}

void close_adapter() {
  g_running.store(false);
  if (g_open) {
    threadWaitForExit(&g_thread); threadClose(&g_thread);
    usbHsEpClose(&g_in); usbHsEpClose(&g_out);
    usbHsIfClose(&g_if);
    g_open = false;
  }
  std::lock_guard<std::mutex> lk(g_mutex);
  g_have_report = false;
  for (auto& o : g_origin) o.set = false;
  g_rate_hz.store(0);
}

// What the system's HID reports: every controller slot with its styles and device type, then how
// often HID updates each connected one (its sampling number counts updates, so two reads a second
// apart give the rate). Once, 15 s after launch (the controller list is set up by then).
void measure_system_rate() {
  static bool done = false;
  if (done) return;
  done = true;
  static Thread t;
  auto body = [](void*) {
    svcSleepThread(15000000000ll);
    const HidNpadIdType ids[] = {HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4,
                                 HidNpadIdType_No5, HidNpadIdType_No6, HidNpadIdType_No7, HidNpadIdType_No8, HidNpadIdType_Handheld};
    for (HidNpadIdType id : ids) {
      const uint32_t styles = hidGetNpadStyleSet(id);
      if (!styles) continue;
      const uint32_t type = hidGetNpadDeviceType(id);
      uint64_t a = 0, b = 0;
      const bool gc = styles & HidNpadStyleTag_NpadGc;
      auto sample = [&]() -> uint64_t {
        if (gc) { HidNpadGcState st{}; hidGetNpadStatesGc(id, &st, 1); return st.sampling_number; }
        HidNpadFullKeyState st{}; hidGetNpadStatesFullKey(id, &st, 1); return st.sampling_number;
      };
      a = sample();
      const uint64_t t0 = armGetSystemTick();
      svcSleepThread(1000000000ll);
      b = sample();
      const double secs = armTicksToNs(armGetSystemTick() - t0) / 1e9;
      log("controllers: slot %d styles %08X device type %08X%s: the system updates it %.0f times/s", int(id) + 1, styles, type,
          gc ? " (GameCube)" : "", double(b - a) / secs);
    }
    log("controllers: survey done");
  };
  if (R_SUCCEEDED(threadCreate(&t, body, nullptr, nullptr, 0x4000, 0x2C, -2))) threadStart(&t);
}

bool open_adapter() {
  if (!g_service) {
    const Result r = usbHsInitialize();
    if (R_FAILED(r)) { if (!g_logged_missing) { log("gc adapter: usb:hs unavailable (0x%x); controllers go through the system", r); g_logged_missing = true; } return false; }
    g_service = true;
    g_in_buf = static_cast<uint8_t*>(aligned_alloc(0x1000, 0x1000));
    g_out_buf = static_cast<uint8_t*>(aligned_alloc(0x1000, 0x1000));
  }
  UsbHsInterfaceFilter filter{};
  filter.Flags = UsbHsInterfaceFilterFlags_idVendor | UsbHsInterfaceFilterFlags_idProduct;
  filter.idVendor = ADAPTER_VID; filter.idProduct = ADAPTER_PID;
  UsbHsInterface ifs[4]; s32 total = 0;
  if (R_FAILED(usbHsQueryAvailableInterfaces(&filter, ifs, sizeof ifs, &total)) || total <= 0) {
    if (!g_logged_missing) {
      s32 owned = 0; UsbHsInterface any[4];
      usbHsQueryAllInterfaces(&filter, any, sizeof any, &owned);
      if (owned > 0) {
        log("gc adapter: plugged in but held by the system (its own GameCube adapter support); controllers go through the system");
        UsbHsClientIfSession probe{};
        const Result r = usbHsAcquireUsbIf(&probe, &any[0]);
        log("gc adapter: taking it from the system: %s (0x%x)", R_SUCCEEDED(r) ? "acquired" : "refused", r);
        if (R_SUCCEEDED(r)) usbHsIfClose(&probe);
        measure_system_rate();
      }
      else log("gc adapter: no WUP-028 adapter (VID 057E PID 0337) on the dock's USB ports");
      g_logged_missing = true;
    }
    return false;
  }
  if (R_FAILED(usbHsAcquireUsbIf(&g_if, &ifs[0]))) { if (!g_logged_missing) { log("gc adapter: could not acquire the adapter interface"); g_logged_missing = true; } return false; }
  // The endpoints from the interface's own descriptors, IN polled every 1 ms instead of its 8.
  const usb_endpoint_descriptor* in_desc = nullptr; const usb_endpoint_descriptor* out_desc = nullptr;
  for (int i = 0; i < 15; ++i) {
    if (!in_desc && g_if.inf.inf.input_endpoint_descs[i].bLength) in_desc = &g_if.inf.inf.input_endpoint_descs[i];
    if (!out_desc && g_if.inf.inf.output_endpoint_descs[i].bLength) out_desc = &g_if.inf.inf.output_endpoint_descs[i];
  }
  if (!in_desc || !out_desc) { log("gc adapter: endpoints not found"); usbHsIfClose(&g_if); return false; }
  usb_endpoint_descriptor in_fast = *in_desc;
  const int native_interval = in_fast.bInterval;
  in_fast.bInterval = 1;
  usb_endpoint_descriptor out_fast = *out_desc;
  out_fast.bInterval = 1;
  if (R_FAILED(usbHsIfOpenUsbEp(&g_if, &g_in, 1, in_fast.wMaxPacketSize, &in_fast)) ||
      R_FAILED(usbHsIfOpenUsbEp(&g_if, &g_out, 1, out_fast.wMaxPacketSize, &out_fast))) {
    log("gc adapter: could not open the endpoints"); usbHsIfClose(&g_if); return false;
  }
  g_interval_ms = 1;
  log("gc adapter: opened WUP-028 over usb:hs, input endpoint polled every 1 ms (adapter asks for %d ms)", native_interval);
  g_logged_missing = false;
  g_open = true;
  g_running.store(true);
  // Above the game and render threads, on the GPU worker's core (light work, never the simulation's).
  if (R_FAILED(threadCreate(&g_thread, reader_main, nullptr, nullptr, 0x4000, 0x24, 0)) || R_FAILED(threadStart(&g_thread))) {
    log("gc adapter: reader thread failed"); g_running.store(false); close_adapter(); return false;
  }
  return true;
}

}  // namespace

uint32_t gcadapter_poll(PadState out[4]) {
  if (!g_open || !g_running.load()) {
    if (g_open && !g_running.load()) close_adapter();
    const uint64_t t = now_ms();
    if (t < g_next_scan) return 0;
    g_next_scan = t + 2000;
    if (!open_adapter()) return 0;
  }
  uint8_t rep[37];
  {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_have_report) return 0;
    std::memcpy(rep, g_report, 37);
  }
  uint32_t mask = 0;
  for (int port = 0; port < 4; ++port) {
    const uint8_t* c = rep + 1 + port * 9;
    if (!(c[0] & 0x30)) { g_origin[port].set = false; continue; }
    Origin& o = g_origin[port];
    if (!o.set) { o.set = true; o.sx = c[3]; o.sy = c[4]; o.cx = c[5]; o.cy = c[6]; o.tl = c[7]; o.tr = c[8]; }
    PadState& p = out[port];
    std::memset(&p, 0, sizeof p);
    uint16_t b = 0;
    if (c[1] & 0x01) b |= PAD_A; if (c[1] & 0x02) b |= PAD_B; if (c[1] & 0x04) b |= PAD_X; if (c[1] & 0x08) b |= PAD_Y;
    if (c[1] & 0x10) b |= PAD_LEFT; if (c[1] & 0x20) b |= PAD_RIGHT; if (c[1] & 0x40) b |= PAD_DOWN; if (c[1] & 0x80) b |= PAD_UP;
    if (c[2] & 0x01) b |= PAD_START; if (c[2] & 0x02) b |= PAD_Z; if (c[2] & 0x04) b |= PAD_R; if (c[2] & 0x08) b |= PAD_L;
    p.button = b;
    auto axis = [](uint8_t v, uint8_t origin) { int a = (int)v - (int)origin; return (int8_t)(a > 127 ? 127 : a < -128 ? -128 : a); };
    p.stick_x = axis(c[3], o.sx); p.stick_y = axis(c[4], o.sy);
    p.sub_x = axis(c[5], o.cx); p.sub_y = axis(c[6], o.cy);
    p.trig_l = (uint8_t)(c[7] > o.tl ? c[7] - o.tl : 0);
    p.trig_r = (uint8_t)(c[8] > o.tr ? c[8] - o.tr : 0);
    mask |= 1u << port;
  }
  g_ports.store(mask);
  return mask;
}

void gcadapter_rumble(int port, bool on) {
  if (port < 0 || port > 3) return;
  window_gamepad_rumble(port, on);
  const uint8_t v = on ? 1 : 0;
  if (g_rumble[port] != v) { g_rumble[port] = v; g_rumble_dirty = true; }
}
bool gcadapter_status(GcAdapterStatus& s) {
  if (!g_open) return false;
  s.ports = g_ports.load(); s.interval_ms = g_interval_ms; s.report_hz = g_rate_hz.load();
  return true;
}
void gcadapter_recalibrate(int port) {
  std::lock_guard<std::mutex> lk(g_mutex);
  if (port < 0 || port > 3) { for (auto& o : g_origin) o.set = false; }
  else g_origin[port].set = false;
}
void gcadapter_shutdown() {
  close_adapter();
  if (g_service) { usbHsExit(); g_service = false; }
}

}  // namespace host
