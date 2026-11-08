// Memory card (CARD SDK) implemented on a folder of .gci files, the same layout Dolphin uses for
// its "GCI folder" cards: 64-byte directory entry followed by the file's 8 KiB blocks. Slot A holds
// one 128 Mbit card (2043 blocks); slot B is empty. Saves made in Slippi Dolphin can be dropped in.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "hle.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace {
constexpr int32_t READY = 0, BUSY = -1, NOCARD = -3, NOFILE = -4, BROKEN = -6, EXIST = -7, NOENT = -8,
                  INSSPACE = -9, NOPERM = -10, NAMETOOLONG = -12;
constexpr uint32_t SECTOR = 0x2000, MAX_FILES = 127, TOTAL_BLOCKS = 2043, MEM_SIZE_MBIT = 128;

struct File {
  uint8_t dir[64] = {};        // CARDDir, big-endian as on the card
  std::vector<uint8_t> data;   // blocks * SECTOR
  std::string path;            // .gci on disk
  std::string name() const { return std::string((const char*)dir + 8, strnlen((const char*)dir + 8, 32)); }
  uint16_t blocks() const { return (uint16_t)((dir[0x38] << 8) | dir[0x39]); }
};

std::vector<File*> g_files;   // indexed by fileNo; null = empty slot
bool g_mounted = false;
int32_t g_xferred = 0;
std::filesystem::path g_dir;

uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

const uint8_t* disk_id() { return host::ptr(0x80000000u, 6); }   // gameName[4] company[2]
bool same_game(const File& f) { return std::memcmp(f.dir, disk_id(), 6) == 0; }

uint32_t used_blocks() { uint32_t n = 0; for (File* f : g_files) if (f) n += f->blocks(); return n; }
int32_t find(const std::string& name) {
  for (size_t i = 0; i < g_files.size(); ++i) if (g_files[i] && same_game(*g_files[i]) && g_files[i]->name() == name) return (int32_t)i;
  return -1;
}

std::string safe_name(const File& f) {
  std::string s((const char*)f.dir, 6);
  s += "-";
  for (char ch : f.name()) s += (std::isalnum((unsigned char)ch) || ch == '_' || ch == '-' || ch == '.') ? ch : '_';
  return s + ".gci";
}

void save(File& f) {
  if (f.path.empty()) f.path = (g_dir / safe_name(f)).string();
  std::filesystem::path tmp = f.path + ".tmp";
  FILE* out = std::fopen(tmp.string().c_str(), "wb");
  if (!out) { host::log("card: cannot write %s", f.path.c_str()); return; }
  bool ok = std::fwrite(f.dir, 1, 64, out) == 64 && std::fwrite(f.data.data(), 1, f.data.size(), out) == f.data.size();
  std::fclose(out);
  std::error_code ec;
  if (ok) std::filesystem::rename(tmp, f.path, ec);
  if (!ok || ec) host::log("card: failed to save %s", f.path.c_str());
}

void mount() {
  if (g_mounted) return;
  g_dir = host::options.card_dir;
  std::error_code ec;
  std::filesystem::create_directories(g_dir, ec);
  for (File* f : g_files) delete f;
  g_files.assign(MAX_FILES, nullptr);
  size_t slot = 0;
  for (auto& entry : std::filesystem::directory_iterator(g_dir, ec)) {
    if (entry.path().extension() != ".gci" || slot >= MAX_FILES) continue;
    FILE* in = std::fopen(entry.path().string().c_str(), "rb");
    if (!in) continue;
    auto* f = new File;
    bool ok = std::fread(f->dir, 1, 64, in) == 64;
    if (ok) { f->data.resize((size_t)f->blocks() * SECTOR); ok = std::fread(f->data.data(), 1, f->data.size(), in) == f->data.size(); }
    std::fclose(in);
    if (!ok || f->blocks() == 0 || f->blocks() > TOTAL_BLOCKS) { host::log("card: ignoring %s", entry.path().string().c_str()); delete f; continue; }
    f->path = entry.path().string();
    g_files[slot++] = f;
  }
  g_mounted = true;
  host::log("card: slot A mounted from %s (%zu files, %u of %u blocks used)", g_dir.string().c_str(), slot, used_blocks(), TOTAL_BLOCKS);
}

void complete(uint32_t callback, int32_t chan, int32_t result) {
  if (callback) host::post_completion([callback, chan, result] { host::call_guest(callback, (uint32_t)chan, (uint32_t)result); });
}

File* file_of(uint32_t info, int32_t* chan_out = nullptr) {
  int32_t chan = (int32_t)host::rd32(info), no = (int32_t)host::rd32(info + 4);
  if (chan_out) *chan_out = chan;
  if (chan != 0 || !g_mounted || no < 0 || no >= (int32_t)MAX_FILES) return nullptr;
  return g_files[no];
}

void fill_info(uint32_t info, int32_t no, const File& f) {
  host::wr32(info, 0); host::wr32(info + 4, (uint32_t)no); host::wr32(info + 8, 0);
  host::wr32(info + 12, (uint32_t)f.data.size()); host::wr16(info + 16, be16(f.dir + 0x36));
}

// File timestamps use the guest's OS clock (timebase + the OS adjust at 0x800030D8, seconds since
// 2000-01-01), not the host clock: the value lands in guest RAM and must be deterministic for a
// given timebase (validation runs with a preset one).
uint32_t now_2000() {
  uint64_t adjust = ((uint64_t)host::rd32(0x800030D8u) << 32) | host::rd32(0x800030DCu);
  return (uint32_t)((host::cpu->tb + adjust) / host::TB_HZ);
}

// __CARDUpdateIconOffsets: where the banner, icons and data sit relative to iconAddr.
void icon_offsets(const File& f, uint8_t* stat) {
  uint32_t icon_addr = be32(f.dir + 0x2C), banner_fmt = f.dir[7] & 3, icon_fmt = be16(f.dir + 0x30);
  uint32_t off = icon_addr;
  const uint32_t none = 0xFFFFFFFFu;
  if (icon_addr == none) {
    put32(stat + 0x3C, none); put32(stat + 0x40, none);
    for (int i = 0; i < 8; ++i) put32(stat + 0x44 + 4 * i, none);
    put32(stat + 0x64, none); put32(stat + 0x68, 0);
    return;
  }
  if (banner_fmt == 1) { put32(stat + 0x3C, off); off += 3072; put32(stat + 0x40, off); off += 512; }
  else if (banner_fmt == 2) { put32(stat + 0x3C, off); off += 6144; put32(stat + 0x40, none); }
  else { put32(stat + 0x3C, none); put32(stat + 0x40, none); }
  bool tlut = false;
  for (int i = 0; i < 8; ++i) {
    uint32_t fmt = (icon_fmt >> (2 * i)) & 3;
    if (fmt == 1) { put32(stat + 0x44 + 4 * i, off); off += 1024; tlut = true; }
    else if (fmt == 2) { put32(stat + 0x44 + 4 * i, off); off += 2048; }
    else put32(stat + 0x44 + 4 * i, none);
  }
  if (tlut) { put32(stat + 0x64, off); off += 512; } else put32(stat + 0x64, none);
  put32(stat + 0x68, off);
}
}  // namespace

HLE(CARDInit) { host::pump_completions(); }
HLE(CARDProbe) { host::pump_completions(); RET(ARG0 == 0 ? 1 : 0); }
HLE(CARDProbeEx) {
  host::pump_completions();
  if (ARG0 != 0) { RET(NOCARD); return; }
  if (ARG1) host::wr32(ARG1, MEM_SIZE_MBIT);
  if (ARG2) host::wr32(ARG2, SECTOR);
  RET(READY);
}
HLE(CARDMountAsync) { if (ARG0 != 0) { RET(NOCARD); return; } mount(); complete(ARG3, 0, READY); RET(READY); }
HLE(CARDMount) { if (ARG0 != 0) { RET(NOCARD); return; } mount(); RET(READY); }
HLE(CARDUnmount) { RET(ARG0 == 0 ? READY : NOCARD); }
HLE(CARDCheckAsync) { if (ARG0 != 0 || !g_mounted) { RET(NOCARD); return; } complete(ARG1, 0, READY); RET(READY); }
HLE(CARDCheck) { RET(ARG0 == 0 && g_mounted ? READY : NOCARD); }
HLE(CARDGetResultCode) { RET(ARG0 == 0 ? READY : NOCARD); }
HLE(CARDGetMemSize) { if (ARG0 != 0) { RET(NOCARD); return; } host::wr16(ARG1, MEM_SIZE_MBIT); RET(READY); }
HLE(CARDGetSectorSize) { if (ARG0 != 0) { RET(NOCARD); return; } host::wr32(ARG1, SECTOR); RET(READY); }
HLE(CARDGetEncoding) { if (ARG0 != 0) { RET(NOCARD); return; } host::wr16(ARG1, 0); RET(READY); }
HLE(CARDGetSerialNo) { if (ARG0 != 0) { RET(NOCARD); return; } host::wr32(ARG1, 0x4D454C45u); host::wr32(ARG1 + 4, 0x504F5254u); RET(READY); }
HLE(CARDGetCurrentMode) { if (ARG0 != 0) { RET(NOCARD); return; } host::wr32(ARG1, 0); RET(READY); }
HLE(CARDCancel) { RET(READY); }
HLE(CARDGetXferredBytes) { RET(ARG0 == 0 ? g_xferred : 0); }

HLE(CARDFreeBlocks) {
  if (ARG0 != 0 || !g_mounted) { RET(NOCARD); return; }
  uint32_t files = 0; for (File* f : g_files) if (f) ++files;
  if (ARG1) host::wr32(ARG1, (TOTAL_BLOCKS - std::min(TOTAL_BLOCKS, used_blocks())) * SECTOR);
  if (ARG2) host::wr32(ARG2, MAX_FILES - files);
  RET(READY);
}

HLE(CARDOpen) {
  if (ARG0 != 0 || !g_mounted) { RET(NOCARD); return; }
  std::string name((const char*)host::ptr(ARG1, 32), strnlen((const char*)host::ptr(ARG1, 32), 32));
  int32_t no = find(name);
  if (no < 0) { TRACE("CARDOpen %s: no file", name.c_str()); RET(NOFILE); return; }
  fill_info(ARG2, no, *g_files[no]);
  TRACE("CARDOpen %s -> file %d (%u blocks)", name.c_str(), no, g_files[no]->blocks());
  RET(READY);
}
HLE(CARDFastOpen) {
  if (ARG0 != 0 || !g_mounted) { RET(NOCARD); return; }
  int32_t no = (int32_t)ARG1;
  if (no < 0 || no >= (int32_t)MAX_FILES || !g_files[no]) { RET(NOFILE); return; }
  if (!same_game(*g_files[no])) { RET(NOPERM); return; }
  fill_info(ARG2, no, *g_files[no]);
  RET(READY);
}
HLE(CARDClose) { host::wr32(ARG0 + 4, (uint32_t)-1); RET(READY); }

static int32_t do_read(uint32_t info, uint32_t buf, int32_t length, int32_t offset) {
  int32_t chan; File* f = file_of(info, &chan);
  if (!f) return chan == 0 ? NOFILE : NOCARD;
  if (length < 0 || offset < 0 || (uint64_t)offset + (uint64_t)length > f->data.size()) return -11 /* LIMIT */;
  std::memcpy(host::ptr(buf, (uint32_t)length), f->data.data() + offset, (size_t)length);
  g_xferred = length;
  return READY;
}
static int32_t do_write(uint32_t info, uint32_t buf, int32_t length, int32_t offset) {
  int32_t chan; File* f = file_of(info, &chan);
  if (!f) return chan == 0 ? NOFILE : NOCARD;
  if (length < 0 || offset < 0 || (uint64_t)offset + (uint64_t)length > f->data.size()) return -11;
  std::memcpy(f->data.data() + offset, host::ptr(buf, (uint32_t)length), (size_t)length);
  g_xferred = length;
  save(*f);
  return READY;
}
HLE(CARDReadAsync) { int32_t r = do_read(ARG0, ARG1, (int32_t)ARG2, (int32_t)ARG3); TRACE("CARDReadAsync len=%u off=%u -> %d", ARG2, ARG3, r); if (r == READY) complete(ARG4, 0, READY); RET(r); }
HLE(CARDRead) { RET(do_read(ARG0, ARG1, (int32_t)ARG2, (int32_t)ARG3)); }
HLE(CARDWriteAsync) { int32_t r = do_write(ARG0, ARG1, (int32_t)ARG2, (int32_t)ARG3); TRACE("CARDWriteAsync len=%u off=%u -> %d", ARG2, ARG3, r); if (r == READY) complete(ARG4, 0, READY); RET(r); }
HLE(CARDWrite) { RET(do_write(ARG0, ARG1, (int32_t)ARG2, (int32_t)ARG3)); }

static int32_t do_create(uint32_t chan, uint32_t name_addr, uint32_t size, uint32_t info) {
  if (chan != 0 || !g_mounted) return NOCARD;
  std::string name((const char*)host::ptr(name_addr, 32), strnlen((const char*)host::ptr(name_addr, 32), 32));
  if (name.empty() || name.size() > 32) return NAMETOOLONG;
  if (find(name) >= 0) return EXIST;
  uint32_t blocks = size / SECTOR;
  if (blocks == 0 || size % SECTOR) return -128;   // FATAL_ERROR: the SDK asserts on this
  if (used_blocks() + blocks > TOTAL_BLOCKS) return INSSPACE;
  int32_t no = -1;
  for (size_t i = 0; i < g_files.size(); ++i) if (!g_files[i]) { no = (int32_t)i; break; }
  if (no < 0) return NOENT;
  auto* f = new File;
  std::memcpy(f->dir, disk_id(), 6);
  f->dir[6] = 0xFF; f->dir[7] = 0;                       // padding, bannerFormat none
  std::memset(f->dir + 8, 0, 32); std::memcpy(f->dir + 8, name.data(), name.size());
  put32(f->dir + 0x28, now_2000());
  put32(f->dir + 0x2C, 0xFFFFFFFFu);                     // iconAddr
  put16(f->dir + 0x30, 0); put16(f->dir + 0x32, 0);      // iconFormat, iconSpeed
  f->dir[0x34] = 0x04; f->dir[0x35] = 0;                 // CARD_ATTR_PUBLIC, copyTimes
  put16(f->dir + 0x36, (uint16_t)(5 + used_blocks()));   // startBlock: cosmetic
  put16(f->dir + 0x38, (uint16_t)blocks);
  f->dir[0x3A] = 0xFF; f->dir[0x3B] = 0xFF;
  put32(f->dir + 0x3C, 0xFFFFFFFFu);                     // commentAddr
  f->data.assign((size_t)blocks * SECTOR, 0xFF);
  g_files[no] = f;
  save(*f);
  if (info) fill_info(info, no, *f);
  host::log("card: created %s (%u blocks) as file %d", name.c_str(), blocks, no);
  return READY;
}
HLE(CARDCreateAsync) { int32_t r = do_create(ARG0, ARG1, ARG2, ARG3); if (r == READY) complete(ARG4, 0, READY); RET(r); }
HLE(CARDCreate) { RET(do_create(ARG0, ARG1, ARG2, ARG3)); }

static int32_t do_delete(int32_t no) {
  if (no < 0 || no >= (int32_t)MAX_FILES || !g_files[no]) return NOFILE;
  File* f = g_files[no];
  std::error_code ec;
  if (!f->path.empty()) std::filesystem::remove(f->path, ec);
  host::log("card: deleted %s", f->name().c_str());
  delete f; g_files[no] = nullptr;
  return READY;
}
HLE(CARDDeleteAsync) {
  if (ARG0 != 0 || !g_mounted) { RET(NOCARD); return; }
  std::string name((const char*)host::ptr(ARG1, 32), strnlen((const char*)host::ptr(ARG1, 32), 32));
  int32_t r = do_delete(find(name)); if (r == READY) complete(ARG2, 0, READY); RET(r);
}
HLE(CARDDelete) {
  if (ARG0 != 0 || !g_mounted) { RET(NOCARD); return; }
  std::string name((const char*)host::ptr(ARG1, 32), strnlen((const char*)host::ptr(ARG1, 32), 32));
  RET(do_delete(find(name)));
}
HLE(CARDFastDeleteAsync) { if (ARG0 != 0 || !g_mounted) { RET(NOCARD); return; } int32_t r = do_delete((int32_t)ARG1); if (r == READY) complete(ARG2, 0, READY); RET(r); }
HLE(CARDFastDelete) { if (ARG0 != 0 || !g_mounted) { RET(NOCARD); return; } RET(do_delete((int32_t)ARG1)); }
HLE(CARDFormatAsync) {
  if (ARG0 != 0) { RET(NOCARD); return; }
  mount();
  for (size_t i = 0; i < g_files.size(); ++i) if (g_files[i]) do_delete((int32_t)i);
  host::log("card: formatted");
  complete(ARG1, 0, READY); RET(READY);
}
HLE(CARDFormat) { if (ARG0 != 0) { RET(NOCARD); return; } mount(); for (size_t i = 0; i < g_files.size(); ++i) if (g_files[i]) do_delete((int32_t)i); RET(READY); }

HLE(CARDGetStatus) {
  if (ARG0 != 0 || !g_mounted) { RET(NOCARD); return; }
  int32_t no = (int32_t)ARG1;
  if (no < 0 || no >= (int32_t)MAX_FILES || !g_files[no]) { RET(NOFILE); return; }
  const File& f = *g_files[no];
  uint8_t* st = host::ptr(ARG2, 0x6C);
  std::memset(st, 0, 0x6C);
  std::memcpy(st, f.dir + 8, 32);                       // fileName
  put32(st + 0x20, (uint32_t)f.data.size());            // length in bytes
  put32(st + 0x24, be32(f.dir + 0x28));                 // time
  std::memcpy(st + 0x28, f.dir, 6);                     // gameName, company
  st[0x2E] = f.dir[7];                                  // bannerFormat
  put32(st + 0x30, be32(f.dir + 0x2C));                 // iconAddr
  put16(st + 0x34, be16(f.dir + 0x30)); put16(st + 0x36, be16(f.dir + 0x32));
  put32(st + 0x38, be32(f.dir + 0x3C));                 // commentAddr
  icon_offsets(f, st);
  RET(READY);
}
static int32_t do_set_status(uint32_t chan, int32_t no, uint32_t stat) {
  if (chan != 0 || !g_mounted) return NOCARD;
  if (no < 0 || no >= (int32_t)MAX_FILES || !g_files[no]) return NOFILE;
  File& f = *g_files[no];
  const uint8_t* st = host::ptr(stat, 0x6C);
  f.dir[7] = st[0x2E];
  std::memcpy(f.dir + 0x2C, st + 0x30, 4);
  std::memcpy(f.dir + 0x30, st + 0x34, 4);
  std::memcpy(f.dir + 0x3C, st + 0x38, 4);
  put32(f.dir + 0x28, now_2000());
  save(f);
  return READY;
}
HLE(CARDSetStatusAsync) { int32_t r = do_set_status(ARG0, (int32_t)ARG1, ARG2); if (r == READY) complete(ARG3, 0, READY); RET(r); }
HLE(CARDSetStatus) { RET(do_set_status(ARG0, (int32_t)ARG1, ARG2)); }
static int32_t do_set_attributes(uint32_t chan, int32_t no, uint8_t attr) {
  if (chan != 0 || !g_mounted) return NOCARD;
  if (no < 0 || no >= (int32_t)MAX_FILES || !g_files[no]) return NOFILE;
  g_files[no]->dir[0x34] = attr; save(*g_files[no]); return READY;
}
HLE(CARDSetAttributesAsync) { int32_t r = do_set_attributes(ARG0, (int32_t)ARG1, (uint8_t)ARG2); if (r == READY) complete(ARG3, 0, READY); RET(r); }
HLE(CARDSetAttributes) { RET(do_set_attributes(ARG0, (int32_t)ARG1, (uint8_t)ARG2)); }

static int32_t do_rename(uint32_t chan, uint32_t old_addr, uint32_t new_addr) {
  if (chan != 0 || !g_mounted) return NOCARD;
  std::string old_name((const char*)host::ptr(old_addr, 32), strnlen((const char*)host::ptr(old_addr, 32), 32));
  std::string new_name((const char*)host::ptr(new_addr, 32), strnlen((const char*)host::ptr(new_addr, 32), 32));
  if (new_name.empty() || new_name.size() > 32) return NAMETOOLONG;
  int32_t no = find(old_name);
  if (no < 0) return NOFILE;
  if (find(new_name) >= 0) return EXIST;
  File& f = *g_files[no];
  std::error_code ec;
  if (!f.path.empty()) std::filesystem::remove(f.path, ec);
  f.path.clear();
  std::memset(f.dir + 8, 0, 32); std::memcpy(f.dir + 8, new_name.data(), new_name.size());
  save(f);
  return READY;
}
HLE(CARDRenameAsync) { int32_t r = do_rename(ARG0, ARG1, ARG2); if (r == READY) complete(ARG3, 0, READY); RET(r); }
HLE(CARDRename) { RET(do_rename(ARG0, ARG1, ARG2)); }
