// Resolves the `slp:<disc file>:<anchor>...` asset links of Slippi's game files against the player's disc.
// C++ port of the `hsd` crate of project-slippi/slippi-rust-extensions (GPL-2.0, commit 934f373).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// What is ported: archive.rs (parse, serialize, reachable), hash.rs (structural hash), link.rs
// (Locator::parse, find_asset, resolve) and the entry sequence of ffi/src/hsd.rs. The authoring
// side (link, locate, autolink, schema, difference, remove_extents) is not on the runtime path.
// An archive is modelled as extents: the byte ranges between consecutive pointer targets, with
// pointer and chain fields zeroed and kept as (field, target extent) pairs.
//
// Kept standalone (C++ standard library only) so the host test in tools/test_hsd_link.cpp builds it.
#include "slippi_hsd_link.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>

namespace slippi::hsd {
namespace {
constexpr uint32_t kHeaderSize = 0x20;       // fixed header before the data section
constexpr uint32_t kChainEnd = 0xFFFFFFFFu;  // terminates an external reference chain
constexpr uint32_t kAlign = 32;              // archives load 32-byte aligned; offset % 32 is a struct's alignment
constexpr size_t kNone = static_cast<size_t>(-1);
// Rust expands `4*n` hops into a Vec and aborts when that cannot be allocated; here a count this
// large is a malformed link instead.
constexpr size_t kMaxHops = 1u << 20;
const char kLinkPrefix[] = "slp:";

using Hash = std::array<uint8_t, 20>;

std::string hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "%x", v);
  return b;
}

uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void put32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

void push32(std::vector<uint8_t>& out, uint32_t v) {
  uint8_t b[4];
  put32(b, v);
  out.insert(out.end(), b, b + 4);
}

// ---- SHA-1 (the crate uses the sha1 crate) ----
struct Sha1 {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  uint8_t buf[64];
  size_t fill = 0;
  uint64_t total = 0;

  static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

  void block(const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = be32(p + 4 * i);
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
      uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }

  void update(const uint8_t* p, size_t n) {
    total += n;
    if (fill) {
      size_t take = std::min(n, sizeof buf - fill);
      std::memcpy(buf + fill, p, take);
      fill += take; p += take; n -= take;
      if (fill < sizeof buf) return;
      block(buf);
      fill = 0;
    }
    for (; n >= 64; p += 64, n -= 64) block(p);
    if (n) { std::memcpy(buf, p, n); fill = n; }
  }

  void update32(uint32_t v) {
    uint8_t b[4];
    put32(b, v);
    update(b, 4);
  }

  Hash finish() {
    uint64_t bits = total * 8;
    uint8_t pad[72] = {0x80};
    size_t padlen = (fill < 56 ? 56 : 120) - fill;
    for (int i = 0; i < 8; ++i) pad[padlen + i] = uint8_t(bits >> (56 - 8 * i));
    update(pad, padlen + 8);
    Hash out;
    for (int i = 0; i < 5; ++i) put32(out.data() + 4 * i, h[i]);
    return out;
  }
};

std::string hash_hex(const Hash& h) {
  static const char digits[] = "0123456789abcdef";
  std::string s;
  for (uint8_t b : h) { s.push_back(digits[b >> 4]); s.push_back(digits[b & 15]); }
  return s;
}

// ---- archive.rs ----
struct Pointer {
  uint32_t field;  // offset of the 4-byte field within the extent
  size_t target;   // index of the target extent
};

struct Extent {
  std::vector<uint8_t> data;
  uint32_t phase = 0;  // data offset modulo 32 this extent must be placed at
  std::vector<Pointer> pointers;
  uint32_t origin = 0;  // data offset in the archive it was parsed from
};

struct Root {
  std::string name;
  size_t extent;
};

struct ExternRef {
  std::string name;
  std::vector<std::pair<size_t, uint32_t>> locations;  // (extent, field), in chain order
};

struct Archive {
  uint8_t version[4] = {};
  uint8_t header_tail[8] = {};  // bytes 0x18..0x20, preserved verbatim
  std::vector<Extent> extents;
  std::vector<Root> roots;
  std::vector<ExternRef> refs;
  bool has_strings = false;  // the original string table, reused while the symbols are unchanged
  std::vector<uint8_t> strings;
  std::vector<uint32_t> root_str, ref_str;
};

std::string truncated(uint64_t needed, uint64_t have) {
  return "archive is truncated: needed " + std::to_string(needed) + " bytes, have " + std::to_string(have);
}

std::string pointer_out_of_range(uint32_t field, uint32_t target) {
  return "pointer at data offset 0x" + hex(field) + " targets 0x" + hex(target) + ", outside the data section";
}

// String::from_utf8_lossy: every maximal invalid sequence becomes U+FFFD.
std::string utf8_lossy(const uint8_t* p, size_t n) {
  std::string out;
  out.reserve(n);
  auto cont = [&](size_t i) { return i < n && (p[i] & 0xC0) == 0x80; };
  size_t i = 0;
  while (i < n) {
    uint8_t b = p[i];
    size_t len = 0, bad = 1;
    if (b < 0x80) {
      len = 1;
    } else if (b >= 0xC2 && b <= 0xDF) {
      if (cont(i + 1)) len = 2;
    } else if (b >= 0xE0 && b <= 0xEF) {
      uint8_t lo = b == 0xE0 ? 0xA0 : 0x80, hi = b == 0xED ? 0x9F : 0xBF;
      if (i + 1 < n && p[i + 1] >= lo && p[i + 1] <= hi) {
        if (cont(i + 2)) len = 3; else bad = 2;
      }
    } else if (b >= 0xF0 && b <= 0xF4) {
      uint8_t lo = b == 0xF0 ? 0x90 : 0x80, hi = b == 0xF4 ? 0x8F : 0xBF;
      if (i + 1 < n && p[i + 1] >= lo && p[i + 1] <= hi) {
        if (!cont(i + 2)) bad = 2;
        else if (!cont(i + 3)) bad = 3;
        else len = 4;
      }
    }
    if (len) {
      out.append(reinterpret_cast<const char*>(p + i), len);
      i += len;
    } else {
      out += "\xEF\xBF\xBD";
      i += bad;
    }
  }
  return out;
}

bool read_cstr(const uint8_t* table, size_t n, uint32_t at, std::string& out, std::string& err) {
  if (n < at) { err = truncated(at, n); return false; }
  size_t end = at;
  while (end < n && table[end] != 0) ++end;
  out = utf8_lossy(table + at, end - at);
  return true;
}

bool parse_archive(const uint8_t* bytes, size_t len, Archive& a, std::string& err) {
  if (len < kHeaderSize) { err = truncated(kHeaderSize, len); return false; }
  const uint32_t file_size = be32(bytes), data_size = be32(bytes + 4);
  const uint64_t reloc_count = be32(bytes + 8), root_count = be32(bytes + 12), ref_count = be32(bytes + 16);
  std::memcpy(a.version, bytes + 0x14, 4);
  std::memcpy(a.header_tail, bytes + 0x18, 8);
  if (file_size != len) {
    err = "header says the file is " + std::to_string(file_size) + " bytes but it is " + std::to_string(len);
    return false;
  }

  const uint64_t reloc_off = uint64_t(kHeaderSize) + data_size;
  const uint64_t roots_off = reloc_off + reloc_count * 4;
  const uint64_t refs_off = roots_off + root_count * 8;
  const uint64_t strings_off = refs_off + ref_count * 8;
  if (len < strings_off) { err = truncated(strings_off, len); return false; }

  const uint8_t* data = bytes + kHeaderSize;
  const uint8_t* strings = bytes + strings_off;
  const size_t strings_len = len - size_t(strings_off);

  // Relocation table: every pointer field, and the extent starts they imply.
  std::vector<std::pair<uint32_t, uint32_t>> fields;  // (field, target)
  fields.reserve(size_t(reloc_count));
  std::vector<uint32_t> boundaries;
  boundaries.reserve(size_t(reloc_count + root_count) + 2);
  boundaries.push_back(0);
  boundaries.push_back(data_size);
  for (uint64_t i = 0; i < reloc_count; ++i) {
    uint32_t field = be32(bytes + reloc_off + i * 4);
    if (uint64_t(field) + 4 > data_size) { err = pointer_out_of_range(field, 0); return false; }
    uint32_t target = be32(data + field);
    if (target > data_size) { err = pointer_out_of_range(field, target); return false; }
    boundaries.push_back(target);
    fields.push_back({field, target});
  }

  // Roots.
  std::vector<std::string> root_names;
  std::vector<uint32_t> root_targets;
  for (uint64_t i = 0; i < root_count; ++i) {
    uint32_t target = be32(bytes + roots_off + i * 8), str_off = be32(bytes + roots_off + i * 8 + 4);
    std::string name;
    if (!read_cstr(strings, strings_len, str_off, name, err)) return false;
    if (target > data_size) { err = "root " + name + " targets 0x" + hex(target) + ", outside the data section"; return false; }
    boundaries.push_back(target);
    a.root_str.push_back(str_off);
    root_names.push_back(std::move(name));
    root_targets.push_back(target);
  }

  // External references and their chains.
  std::vector<std::string> ref_names;
  std::vector<std::vector<uint32_t>> ref_chains;
  for (uint64_t i = 0; i < ref_count; ++i) {
    uint32_t head = be32(bytes + refs_off + i * 8), str_off = be32(bytes + refs_off + i * 8 + 4);
    std::string name;
    if (!read_cstr(strings, strings_len, str_off, name, err)) return false;
    std::vector<uint32_t> chain;
    uint32_t at = head;
    for (;;) {
      if (uint64_t(at) + 4 > data_size || chain.size() > data_size / 4) {
        err = "external reference " + name + " has a malformed chain at data offset 0x" + hex(at);
        return false;
      }
      chain.push_back(at);
      uint32_t next = be32(data + at);
      if (next == kChainEnd) break;
      at = next;
    }
    a.ref_str.push_back(str_off);
    ref_names.push_back(std::move(name));
    ref_chains.push_back(std::move(chain));
  }

  // Cut the data section into extents. A pointer or root may target the very end of the data
  // section: an empty struct, kept as a zero-length extent so the pointer survives.
  bool end_targeted = false;
  for (const auto& f : fields) end_targeted = end_targeted || f.second == data_size;
  for (uint32_t t : root_targets) end_targeted = end_targeted || t == data_size;
  std::sort(boundaries.begin(), boundaries.end());
  boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
  std::vector<uint32_t> starts;
  starts.reserve(boundaries.size());
  for (uint32_t b : boundaries)
    if (b < data_size || (end_targeted && b == data_size)) starts.push_back(b);
  a.extents.resize(starts.size());
  for (size_t i = 0; i < starts.size(); ++i) {
    uint32_t start = starts[i], end = i + 1 < starts.size() ? starts[i + 1] : data_size;
    Extent& e = a.extents[i];
    e.data.assign(data + start, data + end);
    e.phase = start % kAlign;
    e.origin = start;
  }
  auto index_of = [&](uint32_t start) -> size_t {
    auto it = std::lower_bound(starts.begin(), starts.end(), start);
    return it != starts.end() && *it == start ? size_t(it - starts.begin()) : kNone;
  };
  // Locate the extent containing a field and check the field fits inside it.
  auto locate = [&](uint32_t field, size_t& idx, uint32_t& within) -> bool {
    auto it = std::upper_bound(starts.begin(), starts.end(), field);
    if (it == starts.begin()) { err = "field at data offset 0x" + hex(field) + " straddles a struct boundary"; return false; }
    idx = size_t(it - starts.begin()) - 1;
    within = field - starts[idx];
    if (uint64_t(within) + 4 > a.extents[idx].data.size()) {
      err = "field at data offset 0x" + hex(field) + " straddles a struct boundary";
      return false;
    }
    return true;
  };

  for (const auto& f : fields) {
    size_t idx, target_idx = index_of(f.second);
    uint32_t within;
    if (!locate(f.first, idx, within)) return false;
    if (target_idx == kNone) { err = pointer_out_of_range(f.first, f.second); return false; }
    std::memset(a.extents[idx].data.data() + within, 0, 4);
    a.extents[idx].pointers.push_back({within, target_idx});
  }
  for (size_t i = 0; i < root_names.size(); ++i) {
    size_t idx = index_of(root_targets[i]);
    if (idx == kNone) { err = "root " + root_names[i] + " targets 0x" + hex(root_targets[i]) + ", outside the data section"; return false; }
    a.roots.push_back({std::move(root_names[i]), idx});
  }
  for (size_t i = 0; i < ref_names.size(); ++i) {
    ExternRef r;
    r.name = std::move(ref_names[i]);
    r.locations.reserve(ref_chains[i].size());
    for (uint32_t at : ref_chains[i]) {
      size_t idx;
      uint32_t within;
      if (!locate(at, idx, within)) return false;
      std::memset(a.extents[idx].data.data() + within, 0, 4);
      r.locations.push_back({idx, within});
    }
    a.refs.push_back(std::move(r));
  }
  a.strings.assign(strings, strings + strings_len);
  a.has_strings = true;
  return true;
}

bool strings_reusable(const Archive& a) {
  if (!a.has_strings || a.root_str.size() != a.roots.size() || a.ref_str.size() != a.refs.size()) return false;
  std::string s, err;
  for (size_t i = 0; i < a.roots.size(); ++i)
    if (!read_cstr(a.strings.data(), a.strings.size(), a.root_str[i], s, err) || s != a.roots[i].name) return false;
  for (size_t i = 0; i < a.refs.size(); ++i)
    if (!read_cstr(a.strings.data(), a.strings.size(), a.ref_str[i], s, err) || s != a.refs[i].name) return false;
  return true;
}

// Layout is deterministic: extents in order, each placed at the next offset with its recorded
// alignment phase. Offsets are 32-bit and wrap like the reference's release build; where the
// reference would index out of bounds (and abort) this returns an error.
bool serialize_archive(const Archive& a, std::vector<uint8_t>& out, std::string& err) {
  const char* internal = "resolved archive is inconsistent";
  std::vector<uint32_t> offsets;
  offsets.reserve(a.extents.size());
  uint32_t cursor = 0;
  uint64_t wide = 0;  // the same sum without wrapping, to notice an archive past 4 GiB
  for (const Extent& e : a.extents) {
    uint32_t pad = (e.phase + kAlign - cursor % kAlign) % kAlign;
    cursor += pad;
    offsets.push_back(cursor);
    cursor += uint32_t(e.data.size());
    wide += pad + e.data.size();
  }
  if (wide != cursor) { err = internal; return false; }
  const uint32_t data_size = cursor;

  // Data section with pointers and chains filled in.
  std::vector<uint8_t> data(data_size, 0);
  std::vector<uint32_t> relocs;
  for (size_t i = 0; i < a.extents.size(); ++i) {
    const Extent& e = a.extents[i];
    const size_t base = offsets[i];
    if (!e.data.empty()) std::memcpy(data.data() + base, e.data.data(), e.data.size());
    for (const Pointer& p : e.pointers) {
      size_t at = base + p.field;
      if (at + 4 > data.size() || p.target >= offsets.size()) { err = internal; return false; }
      put32(data.data() + at, offsets[p.target]);
      relocs.push_back(uint32_t(at));
    }
  }
  std::sort(relocs.begin(), relocs.end());

  std::vector<uint32_t> ref_heads;
  for (const ExternRef& r : a.refs) {
    if (r.locations.empty()) { err = "external reference " + r.name + " has no locations"; return false; }
    for (const auto& loc : r.locations)
      if (loc.first >= offsets.size()) { err = internal; return false; }
    auto place = [&](const std::pair<size_t, uint32_t>& loc) { return offsets[loc.first] + loc.second; };
    for (size_t n = 0; n < r.locations.size(); ++n) {
      uint32_t value = n + 1 < r.locations.size() ? place(r.locations[n + 1]) : kChainEnd;
      size_t at = place(r.locations[n]);
      if (at + 4 > data.size()) { err = internal; return false; }
      put32(data.data() + at, value);
    }
    ref_heads.push_back(place(r.locations[0]));
  }

  // String table: reuse the original when the symbols are unchanged.
  std::vector<uint8_t> strings;
  std::vector<uint32_t> root_str, ref_str;
  if (strings_reusable(a)) {
    strings = a.strings; root_str = a.root_str; ref_str = a.ref_str;
  } else {
    for (const Root& r : a.roots) {
      root_str.push_back(uint32_t(strings.size()));
      strings.insert(strings.end(), r.name.begin(), r.name.end());
      strings.push_back(0);
    }
    for (const ExternRef& r : a.refs) {
      ref_str.push_back(uint32_t(strings.size()));
      strings.insert(strings.end(), r.name.begin(), r.name.end());
      strings.push_back(0);
    }
  }

  for (const Root& r : a.roots)
    if (r.extent >= offsets.size()) { err = internal; return false; }
  const size_t file_size = kHeaderSize + data.size() + relocs.size() * 4 + (a.roots.size() + a.refs.size()) * 8 + strings.size();
  out.clear();
  out.reserve(file_size);
  push32(out, uint32_t(file_size));
  push32(out, data_size);
  push32(out, uint32_t(relocs.size()));
  push32(out, uint32_t(a.roots.size()));
  push32(out, uint32_t(a.refs.size()));
  out.insert(out.end(), a.version, a.version + 4);
  out.insert(out.end(), a.header_tail, a.header_tail + 8);
  out.insert(out.end(), data.begin(), data.end());
  for (uint32_t r : relocs) push32(out, r);
  for (size_t i = 0; i < a.roots.size(); ++i) { push32(out, offsets[a.roots[i].extent]); push32(out, root_str[i]); }
  for (size_t i = 0; i < a.refs.size(); ++i) { push32(out, ref_heads[i]); push32(out, ref_str[i]); }
  out.insert(out.end(), strings.begin(), strings.end());
  return true;
}

size_t extent_at_origin(const Archive& a, uint32_t data_offset) {
  for (size_t i = 0; i < a.extents.size(); ++i)
    if (a.extents[i].origin == data_offset) return i;
  return kNone;
}

bool contains(const std::vector<uint32_t>& v, uint32_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }

// ---- hash.rs ----
// Hash of one struct from its bytes (trailing zeros ignored) and the hashes of the structs its
// pointers target, in field order.
Hash combine(const std::vector<uint8_t>& data, const std::vector<std::pair<uint32_t, Hash>>& children) {
  size_t end = data.size();
  while (end > 0 && data[end - 1] == 0) --end;
  Sha1 sha;
  sha.update(data.data(), end);
  for (const auto& c : children) {
    sha.update32(c.first);
    sha.update(c.second.data(), c.second.size());
  }
  return sha.finish();
}

std::vector<Pointer> sorted_by_field(std::vector<Pointer> p) {
  std::stable_sort(p.begin(), p.end(), [](const Pointer& a, const Pointer& b) { return a.field < b.field; });
  return p;
}

// Hashes of every extent's reachable structure, memoized. `cut_extent` stands for the reference's
// cloned archive with the cut pointers of that one extent removed (asset_sha). The reference
// recurses; this walks the same depth-first order with an explicit stack, since joint chains run
// thousands deep and console thread stacks are small.
struct Hasher {
  const Archive& archive;
  size_t cut_extent = kNone;
  const std::vector<uint32_t>* cut = nullptr;
  std::vector<Hash> memo;
  std::vector<uint8_t> state;  // 0 unseen, 1 in progress, 2 memoized

  explicit Hasher(const Archive& a) : archive(a), memo(a.extents.size()), state(a.extents.size(), 0) {}

  struct Frame {
    size_t idx;
    std::vector<Pointer> pointers;
    size_t next;
    std::vector<std::pair<uint32_t, Hash>> children;
  };

  static Hash cycle() {
    Sha1 sha;
    sha.update(reinterpret_cast<const uint8_t*>("cycle"), 5);
    return sha.finish();
  }

  void open(std::vector<Frame>& stack, size_t idx) {
    state[idx] = 1;
    std::vector<Pointer> pointers = archive.extents[idx].pointers;
    if (idx == cut_extent && cut)
      pointers.erase(std::remove_if(pointers.begin(), pointers.end(), [&](const Pointer& p) { return contains(*cut, p.field); }),
                     pointers.end());
    stack.push_back(Frame{idx, sorted_by_field(std::move(pointers)), 0, {}});
  }

  // Hash of an extent and everything it points to.
  Hash extent(size_t root) {
    if (state[root] == 2) return memo[root];
    if (state[root] == 1) return cycle();
    std::vector<Frame> stack;
    open(stack, root);
    for (;;) {
      Frame& f = stack.back();
      if (f.next < f.pointers.size()) {
        const Pointer p = f.pointers[f.next];
        if (state[p.target] == 2) { f.children.push_back({p.field, memo[p.target]}); ++f.next; }
        else if (state[p.target] == 1) { f.children.push_back({p.field, cycle()}); ++f.next; }
        else open(stack, p.target);
        continue;
      }
      Hash h = combine(archive.extents[f.idx].data, f.children);
      state[f.idx] = 2;
      memo[f.idx] = h;
      stack.pop_back();
      if (stack.empty()) return h;
      Frame& parent = stack.back();
      parent.children.push_back({parent.pointers[parent.next].field, h});
      ++parent.next;
    }
  }
};

// ---- link.rs ----
struct Locator {
  std::string file;
  bool is_path = false;  // anchor: a root symbol and the pointer fields to follow, or a file offset
  uint32_t offset = 0;
  std::string root;
  std::vector<uint32_t> hops;
  bool has_at = false;  // file offset tried when a path anchor does not resolve
  uint32_t at = 0;
  std::vector<uint32_t> cut;
  bool has_sha = false;
  std::string sha;
  bool strict = false;  // a hash mismatch is an error rather than a warning
};

bool is_link(const std::string& symbol) { return symbol.compare(0, 4, kLinkPrefix) == 0; }

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> parts;
  size_t from = 0;
  for (;;) {
    size_t at = s.find(sep, from);
    if (at == std::string::npos) { parts.push_back(s.substr(from)); return parts; }
    parts.push_back(s.substr(from, at - from));
    from = at + 1;
  }
}

std::string bad_locator(const std::string& symbol, const std::string& reason) {
  return "malformed link symbol " + symbol + ": " + reason;
}

// u32::from_str_radix(digits, 16) after an optional 0x/0X: an optional '+', then hex digits.
bool parse_hex(const std::string& symbol, const std::string& text, uint32_t& out, std::string& err) {
  size_t i = text.compare(0, 2, "0x") == 0 || text.compare(0, 2, "0X") == 0 ? 2 : 0;
  if (i < text.size() && text[i] == '+') ++i;
  uint64_t v = 0;
  bool ok = i < text.size();
  for (; ok && i < text.size(); ++i) {
    char c = text[i];
    uint32_t d;
    if (c >= '0' && c <= '9') d = uint32_t(c - '0');
    else if (c >= 'a' && c <= 'f') d = uint32_t(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = uint32_t(c - 'A' + 10);
    else { ok = false; break; }
    v = v * 16 + d;
    if (v > 0xFFFFFFFFull) ok = false;
  }
  if (!ok) { err = bad_locator(symbol, "bad number " + text); return false; }
  out = uint32_t(v);
  return true;
}

// str::parse::<usize>: an optional '+', then decimal digits, no overflow (usize is 64 bits).
bool parse_count(const std::string& text, uint64_t& out) {
  size_t i = !text.empty() && text[0] == '+' ? 1 : 0;
  if (i >= text.size()) return false;
  uint64_t v = 0;
  for (; i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    uint64_t d = uint64_t(text[i] - '0');
    if (v > (UINT64_MAX - d) / 10) return false;
    v = v * 10 + d;
  }
  out = v;
  return true;
}

bool parse_locator(const std::string& symbol, Locator& l, std::string& err) {
  auto bad = [&](const std::string& reason) { err = bad_locator(symbol, reason); return false; };
  if (!is_link(symbol)) return bad("missing slp: prefix");
  std::vector<std::string> parts = split(symbol.substr(4), ':');
  if (parts[0].empty()) return bad("missing file name");
  l.file = parts[0];
  if (parts.size() < 2) return bad("missing anchor");
  const std::string& anchor = parts[1];
  size_t slash = anchor.find('/');
  if (slash != std::string::npos) {
    l.is_path = true;
    l.root = anchor.substr(0, slash);
    if (l.root.empty()) return bad("missing root symbol");
    for (const std::string& hop : split(anchor.substr(slash + 1), '/')) {
      if (hop.empty()) continue;
      std::string field_text = hop;
      uint64_t count = 1;
      size_t star = hop.find('*');
      if (star != std::string::npos) {
        field_text = hop.substr(0, star);
        std::string n = hop.substr(star + 1);
        if (!parse_count(n, count)) return bad("bad repeat count " + n);
      }
      uint32_t field;
      if (!parse_hex(symbol, field_text, field, err)) return false;
      if (count > kMaxHops - l.hops.size()) return bad("path is too long");
      l.hops.insert(l.hops.end(), size_t(count), field);
    }
  } else {
    if (!parse_hex(symbol, anchor, l.offset, err)) return false;
    if (l.offset < kHeaderSize) return bad("offset is inside the header");
  }
  for (size_t i = 2; i < parts.size(); ++i) {
    const std::string& part = parts[i];
    size_t eq = part.find('=');
    std::string key = eq == std::string::npos ? std::string() : part.substr(0, eq);
    std::string value = eq == std::string::npos ? std::string() : part.substr(eq + 1);
    if (eq != std::string::npos && key == "at") {
      uint32_t offset;
      if (!parse_hex(symbol, value, offset, err)) return false;
      if (offset < kHeaderSize) return bad("at offset is inside the header");
      l.has_at = true;
      l.at = offset;
    } else if (eq != std::string::npos && key == "cut") {
      for (const std::string& item : split(value, ',')) {
        uint32_t field;
        if (!parse_hex(symbol, item, field, err)) return false;
        l.cut.push_back(field);
      }
    } else if (eq != std::string::npos && key == "sha" && value.size() == 8 &&
               std::all_of(value.begin(), value.end(), [](char c) {
                 return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
               })) {
      for (char& c : value)
        if (c >= 'A' && c <= 'F') c = char(c - 'A' + 'a');
      l.has_sha = true;
      l.sha = value;
    } else if (eq != std::string::npos && key == "strict" && value == "1") {
      l.strict = true;
    } else {
      return bad("unknown option " + part);
    }
  }
  return true;
}

// Short structural hash of an asset, as recorded in a locator's `sha` option.
std::string asset_sha(const Archive& archive, size_t extent, const std::vector<uint32_t>& cut) {
  Hasher hasher(archive);
  if (!cut.empty()) { hasher.cut_extent = extent; hasher.cut = &cut; }
  return hash_hex(hasher.extent(extent)).substr(0, 8);
}

// The extent an anchor names in `archive`, or why there is none.
size_t at_anchor(const Archive& archive, const Locator& l, std::string& reason) {
  if (!l.is_path) {
    size_t e = extent_at_origin(archive, l.offset - kHeaderSize);
    if (e == kNone) reason = "no struct starts at file offset 0x" + hex(l.offset);
    return e;
  }
  size_t cur = kNone;
  for (const Root& r : archive.roots)
    if (r.name == l.root) { cur = r.extent; break; }
  if (cur == kNone) { reason = "no root symbol " + l.root; return kNone; }
  for (size_t n = 0; n < l.hops.size(); ++n) {
    size_t next = kNone;
    for (const Pointer& p : archive.extents[cur].pointers)
      if (p.field == l.hops[n]) { next = p.target; break; }
    if (next == kNone) {
      reason = "hop " + std::to_string(n) + " (field 0x" + hex(l.hops[n]) + ") is not a pointer";
      return kNone;
    }
    cur = next;
  }
  return cur;
}

// The other region's copy of a disc file: a Japanese `.dat` and an American `.usd`.
bool regional_twin(const std::string& file, std::string& twin) {
  size_t dot = file.rfind('.');
  if (dot == std::string::npos) return false;
  std::string ext = file.substr(dot + 1);
  if (ext == "usd") twin = file.substr(0, dot) + ".dat";
  else if (ext == "dat") twin = file.substr(0, dot) + ".usd";
  else return false;
  return true;
}

// Structural hash of `extent` without the cut pointers of its first struct, from the hashes of
// every extent in the archive.
Hash cut_hash(const Archive& archive, const std::vector<Hash>& hashes, size_t extent, const std::vector<uint32_t>& cut) {
  if (cut.empty()) return hashes[extent];
  const Extent& e = archive.extents[extent];
  std::vector<std::pair<uint32_t, Hash>> children;
  for (const Pointer& p : e.pointers)
    if (!contains(cut, p.field)) children.push_back({p.field, hashes[p.target]});
  std::stable_sort(children.begin(), children.end(),
                   [](const std::pair<uint32_t, Hash>& a, const std::pair<uint32_t, Hash>& b) { return a.first < b.first; });
  return combine(e.data, children);
}

// A disc file loaded for resolving.
struct Source {
  bool ok = false;
  std::string error;  // why it could not be read
  Archive archive;
  bool hashed = false;
  std::vector<Hash> hashes;  // of every extent, computed the first time an asset is searched for

  // Find the asset a locator records by its hash: at the fallback offset, else anywhere.
  size_t find_by_hash(const Locator& l) {
    if (!l.has_sha) return kNone;
    if (l.has_at) {
      size_t e = extent_at_origin(archive, l.at - kHeaderSize);
      if (e != kNone && asset_sha(archive, e, l.cut) == l.sha) return e;
    }
    if (!hashed) {
      Hasher hasher(archive);
      hashes.reserve(archive.extents.size());
      for (size_t i = 0; i < archive.extents.size(); ++i) hashes.push_back(hasher.extent(i));
      hashed = true;
    }
    uint8_t prefix[4];
    auto nibble = [](char c) { return uint8_t(c <= '9' ? c - '0' : c - 'a' + 10); };  // sha is 8 lowercase hex digits
    for (int i = 0; i < 4; ++i) prefix[i] = uint8_t(nibble(l.sha[2 * i]) << 4 | nibble(l.sha[2 * i + 1]));
    // Hashes memoized across the whole file can differ from a fresh one for an asset that loops
    // back into itself, so they only pick candidates.
    for (size_t i = 0; i < archive.extents.size(); ++i) {
      Hash h = cut_hash(archive, hashes, i, l.cut);
      if (std::memcmp(h.data(), prefix, 4) == 0 && asset_sha(archive, i, l.cut) == l.sha) return i;
    }
    return kNone;
  }
};

// The disc files read while resolving, each read at most once.
struct Sources {
  const DiscReader& read_disc;
  std::vector<std::string>& disc_files;
  std::map<std::string, Source> files;

  Source& get(const std::string& file) {
    auto it = files.find(file);
    if (it != files.end()) return it->second;
    Source& src = files[file];
    std::vector<uint8_t> bytes;
    // The loader of ffi/src/hsd.rs.
    if (!read_disc) src.error = "no disc reader was provided";
    else if (file.find('\0') != std::string::npos) src.error = "file name contains a NUL byte";
    else if (!read_disc(file, bytes)) src.error = "not found on the disc";
    else {
      disc_files.push_back(file);
      src.ok = parse_archive(bytes.data(), bytes.size(), src.archive, src.error);
      if (!src.ok) src.archive = Archive();
    }
    return src;
  }
};

struct Found {
  std::string file;
  size_t extent = kNone;
};

// Find the struct a locator names: by its anchor in the named file, which follows a mod that
// replaced the asset in place; else by its recorded hash in that file, then in the other region's
// copy of the file; else the other region's asset at the same path.
bool find_asset(Sources& sources, const Locator& l, const std::string& symbol, std::vector<std::string>& warnings,
                Found& found, std::string& err) {
  const std::string& file = l.file;
  std::vector<std::string> searched;  // files searched by hash, for the error message
  std::string problem, unreadable;
  bool is_unreadable = false;
  {
    Source& src = sources.get(file);
    if (!src.ok) {
      problem = file + " could not be read (" + src.error + ")";
      unreadable = src.error;
      is_unreadable = true;
    } else {
      std::string reason;
      size_t extent = at_anchor(src.archive, l, reason);
      if (extent != kNone) {
        if (l.has_sha) {
          std::string actual = asset_sha(src.archive, extent, l.cut);
          if (actual != l.sha) {
            if (l.strict) {
              err = "linked asset " + symbol + " does not match its recorded hash (expected " + l.sha + ", found " + actual + ")";
              return false;
            }
            warnings.push_back(symbol + ": the asset on the disc differs from the recorded one (" + l.sha + " vs " + actual +
                               "); using the disc's version");
          }
        }
        found = {file, extent};
        return true;
      }
      extent = src.find_by_hash(l);
      if (extent != kNone) {
        uint32_t offset = src.archive.extents[extent].origin + kHeaderSize;
        warnings.push_back(symbol + ": " + reason + "; found the recorded asset at 0x" + hex(offset) + " in " + file);
        found = {file, extent};
        return true;
      }
      if (l.has_sha) {
        searched.push_back(file);
      } else if (l.has_at && (extent = extent_at_origin(src.archive, l.at - kHeaderSize)) != kNone) {
        // Without a hash the fallback offset cannot be checked, only trusted.
        warnings.push_back(symbol + ": " + reason + "; used the fallback offset");
        found = {file, extent};
        return true;
      }
      problem = reason;
    }
  }

  std::string twin;
  if (regional_twin(file, twin)) {
    Source& src = sources.get(twin);
    if (src.ok) {
      // An offset means nothing in another file, but a path names the same asset.
      std::string ignored;
      size_t slot = l.is_path ? at_anchor(src.archive, l, ignored) : kNone;
      bool slot_is_recorded = slot != kNone && l.has_sha && asset_sha(src.archive, slot, l.cut) == l.sha;
      size_t recorded = slot_is_recorded ? slot : src.find_by_hash(l);
      if (recorded != kNone) {
        warnings.push_back(symbol + ": " + problem + "; found the recorded asset in " + twin);
        found = {twin, recorded};
        return true;
      }
      if (slot != kNone && !l.strict) {
        warnings.push_back(symbol + ": " + problem + "; used the same asset of " + twin + ", which differs from the recorded one");
        found = {twin, slot};
        return true;
      }
      if (l.has_sha) searched.push_back(twin);
    }
  }

  if (is_unreadable) {
    err = "could not read disc file " + file + ": " + unreadable;
  } else if (!l.is_path) {
    err = "disc file " + file + " has no struct starting at file offset 0x" + hex(l.offset);
  } else {
    std::string reason = problem;
    if (!searched.empty()) {
      reason += "; the recorded asset is not in ";
      for (size_t i = 0; i < searched.size(); ++i) reason += (i ? " or " : "") + searched[i];
    }
    err = "link " + symbol + " does not resolve in the disc file: " + reason;
  }
  return false;
}

// Extents reachable from `start` through pointers, in breadth-first order, not following the cut
// pointers of `start` itself.
std::vector<size_t> reachable(const Archive& a, size_t start, const std::vector<uint32_t>& cut) {
  std::vector<uint8_t> seen(a.extents.size(), 0);
  std::vector<size_t> order;
  seen[start] = 1;
  order.push_back(start);
  for (size_t head = 0; head < order.size(); ++head) {
    size_t idx = order[head];
    for (const Pointer& p : a.extents[idx].pointers) {
      if ((idx == start && contains(cut, p.field)) || seen[p.target]) continue;
      seen[p.target] = 1;
      order.push_back(p.target);
    }
  }
  return order;
}

// Resolve every `slp:` reference of `out` by copying the linked assets in from disc files. Other
// references are kept.
bool resolve(Archive& out, Sources& sources, std::vector<std::string>& warnings, std::string& err) {
  std::vector<ExternRef> refs = std::move(out.refs);
  out.refs.clear();
  bool resolved_any = false;

  for (ExternRef& r : refs) {
    if (!is_link(r.name)) { out.refs.push_back(r); continue; }
    Locator locator;
    Found found;
    if (!parse_locator(r.name, locator, err)) return false;
    if (!find_asset(sources, locator, r.name, warnings, found, err)) return false;
    const Archive& src = sources.get(found.file).archive;
    const size_t root = found.extent;

    std::vector<size_t> ordered = reachable(src, root, locator.cut);
    std::stable_sort(ordered.begin(), ordered.end(), [&](size_t a, size_t b) { return src.extents[a].origin < src.extents[b].origin; });
    std::map<size_t, size_t> new_index;
    for (size_t i : ordered) {
      new_index[i] = out.extents.size();
      Extent e;
      e.data = src.extents[i].data;
      e.phase = src.extents[i].phase;
      e.origin = src.extents[i].origin;
      out.extents.push_back(std::move(e));
    }
    for (size_t i : ordered) {
      std::vector<Pointer> pointers;
      for (const Pointer& p : src.extents[i].pointers) {
        if (i == root && contains(locator.cut, p.field)) continue;
        auto t = new_index.find(p.target);
        if (t != new_index.end()) pointers.push_back({p.field, t->second});
      }
      out.extents[new_index[i]].pointers = std::move(pointers);
    }
    const size_t target = new_index[root];
    for (const auto& loc : r.locations) out.extents[loc.first].pointers.push_back({loc.second, target});
    resolved_any = true;
  }

  if (resolved_any) out.has_strings = false;
  return true;
}

LinkResult resolve_links_impl(std::vector<uint8_t>& contents, const DiscReader& read_disc) {
  LinkResult result;
  if (contents.size() < kHeaderSize) return result;
  // Cheap exit for archives without an external reference table.
  if (be32(contents.data() + 0x10) == 0) return result;

  Archive archive;
  std::string err;
  if (!parse_archive(contents.data(), contents.size(), archive, err)) {
    result.warnings.push_back("not a parseable archive, serving it as is: " + err);
    return result;
  }
  for (const ExternRef& r : archive.refs)
    if (is_link(r.name)) ++result.links;
  if (result.links == 0) return result;

  Sources sources{read_disc, result.disc_files, {}};
  std::vector<uint8_t> out;
  // The reference logs warnings only once the whole file resolved.
  std::vector<std::string> warnings;
  if (!resolve(archive, sources, warnings, err) || !serialize_archive(archive, out, err)) {
    result.status = LinkStatus::Failed;
    result.error = err;
    return result;
  }
  result.warnings = std::move(warnings);
  result.status = LinkStatus::Resolved;
  contents = std::move(out);
  return result;
}
}  // namespace

LinkResult resolve_links(std::vector<uint8_t>& contents, const DiscReader& read_disc) {
#if defined(__cpp_exceptions)
  // Nothing here throws by design; this keeps an allocation failure from crossing the API.
  try {
    return resolve_links_impl(contents, read_disc);
  } catch (...) {
    LinkResult result;
    result.status = LinkStatus::Failed;
    result.error = "out of memory or internal error while resolving links";
    return result;
  }
#else
  return resolve_links_impl(contents, read_disc);
#endif
}
}  // namespace slippi::hsd
