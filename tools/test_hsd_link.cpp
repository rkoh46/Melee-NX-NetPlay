// Host test for port/runtime/hle/slippi_hsd_link.cpp.
//   test_hsd_link <archive> <dir of extracted disc files> <out>   resolve one file; <out> is written only when Resolved
//   test_hsd_link --selftest                                      the crate's sample archive, linked and resolved
// Build: clang++ -std=c++17 -Wall -Wextra -O2 -Iport/runtime/hle tools/test_hsd_link.cpp port/runtime/hle/slippi_hsd_link.cpp -o test_hsd_link
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_hsd_link.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>

using Bytes = std::vector<uint8_t>;

static bool read_file(const std::string& path, Bytes& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

static void be32(Bytes& b, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
}

// The sample of the crate's tests (archive.rs): root -> A, A -> B (40 bytes at 16) and twice -> C (64 bytes of 7 at
// 64). `linked` is the same file with C replaced by a link; resolving it against the full file must give the full
// file back, with no warning: the recorded hash (sha1 of C, as the crate computes it) matches.
static Bytes sample(bool linked) {
  Bytes data(linked ? 56 : 128, 0);
  for (int i = 0; i < 40; ++i) data[16 + i] = uint8_t(i + 1);
  auto put = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) data[at + i] = uint8_t(v >> (24 - 8 * i)); };
  put(0, 16);
  if (linked) { put(4, 8); put(8, 0xFFFFFFFFu); } else { put(4, 64); put(8, 64); for (int i = 64; i < 128; ++i) data[i] = 7; }
  std::string strings("sample_root\0", 12);
  if (linked) strings += std::string("slp:Sample.dat:sample_root/4:at=0x60:sha=6b4d5b86\0", 50);
  const uint32_t relocs = linked ? 1 : 3;
  Bytes out;
  be32(out, uint32_t(0x20 + data.size() + relocs * 4 + (linked ? 16 : 8) + strings.size()));
  be32(out, uint32_t(data.size())); be32(out, relocs); be32(out, 1); be32(out, linked ? 1 : 0);
  out.insert(out.end(), {'0', '0', '1', 'B', 0, 0, 0, 0, 0, 0, 0, 0});
  out.insert(out.end(), data.begin(), data.end());
  for (uint32_t r = 0; r < relocs; ++r) be32(out, r * 4);
  be32(out, 0); be32(out, 0);
  if (linked) { be32(out, 4); be32(out, 12); }
  out.insert(out.end(), strings.begin(), strings.end());
  return out;
}

static int selftest() {
  using namespace slippi::hsd;
  const Bytes disc = sample(false);
  auto reader = [&](const std::string& name, Bytes& out) { return name == "Sample.dat" ? (out = disc, true) : false; };
  Bytes a = sample(true);
  LinkResult r = resolve_links(a, reader);
  assert(r.status == LinkStatus::Resolved && r.links == 1 && r.warnings.empty());
  assert(r.disc_files == std::vector<std::string>{"Sample.dat"});
  assert(a == disc);
  // No external references: untouched.
  Bytes b = disc;
  assert(resolve_links(b, reader).status == LinkStatus::NoLinks && b == disc);
  // The disc file is missing: Failed, untouched.
  Bytes c = sample(true);
  r = resolve_links(c, [](const std::string&, Bytes&) { return false; });
  assert(r.status == LinkStatus::Failed && c == sample(true));
  assert(r.error == "could not read disc file Sample.dat: not found on the disc");
  // A different asset on the disc: used, with a warning (resolve_links has no strict link here).
  Bytes edited = disc;
  edited[0x20 + 64] = 8;
  Bytes d = sample(true);
  r = resolve_links(d, [&](const std::string&, Bytes& out) { out = edited; return true; });
  assert(r.status == LinkStatus::Resolved && r.warnings.size() == 1 && d == edited);
  std::puts("selftest ok");
  return 0;
}

int main(int argc, char** argv) {
  using namespace slippi::hsd;
  if (argc == 2 && std::string(argv[1]) == "--selftest") return selftest();
  if (argc != 4) { std::fputs("usage: test_hsd_link <archive> <disc dir> <out> | --selftest\n", stderr); return 2; }
  Bytes contents;
  if (!read_file(argv[1], contents)) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
  const size_t in_size = contents.size();
  const std::string dir = argv[2];
  auto reader = [&](const std::string& name, Bytes& out) {
    return name.find('/') == std::string::npos && read_file(dir + "/" + name, out);
  };
  auto t0 = std::chrono::steady_clock::now();
  LinkResult r = resolve_links(contents, reader);
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  static const char* names[] = {"Resolved", "NoLinks", "Failed"};
  std::string files;
  for (const auto& f : r.disc_files) files += (files.empty() ? "" : ",") + f;
  std::printf("status: %s\nlinks: %zu\ndisc_files: %s\n", names[int(r.status)], r.links, files.c_str());
  for (const auto& w : r.warnings) std::printf("warning: %s\n", w.c_str());
  if (r.status == LinkStatus::Failed) std::printf("error: %s\n", r.error.c_str());
  std::fprintf(stderr, "%.1f ms: %zu bytes in, %zu bytes out\n", ms, in_size, contents.size());
  if (r.status == LinkStatus::Resolved) std::ofstream(argv[3], std::ios::binary).write((const char*)contents.data(), (std::streamsize)contents.size());
  return 0;
}
