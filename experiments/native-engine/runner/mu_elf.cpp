// ELF loader for the game image. See mu_elf_bridge.h.
//
// Everything is prepared in staging pages first (segments copied, relocations applied), because on
// Horizon the pages cannot be written through their final address until they are mapped and given
// a protection, and code pages can never be written there. Relocation targets are all in the
// writable segment; the image is linked at its run address, so the load bias is zero.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mu_elf_bridge.h"
#include "fixedmap.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#ifdef __SWITCH__
#include <unistd.h>
#endif

extern "C" void* mu_import(const char* name);

#ifdef __SWITCH__
extern "C" ssize_t pread(int fd, void* buffer, size_t bytes, off_t offset) {
  if (lseek(fd, offset, SEEK_SET) < 0) return -1;
  return read(fd, buffer, bytes);
}
extern "C" time_t timegm(struct tm* tm) { return mktime(tm); }   // newlib's local time is UTC here
#endif

namespace mac_elf {
namespace {
constexpr uintptr_t mem1 = 0x80000000u, image_base = 0x82800000u, window_end = 0x84000000u;
constexpr size_t mem1_size = 40u << 20, page = FIXEDMAP_PAGE;
const uint8_t* embedded = nullptr;
size_t embedded_size = 0;

struct Ehdr { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Phdr { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; };
struct Dyn { int64_t tag; uint64_t value; };
struct Sym { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; };
struct Rela { uint64_t offset, info; int64_t addend; };
enum { PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7, PF_X = 1, PF_W = 2 };
enum { DT_NULL = 0, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8, DT_RELAENT = 9,
       DT_STRSZ = 10, DT_SYMENT = 11, DT_INIT = 12, DT_FINI = 13, DT_PLTREL = 20, DT_JMPREL = 23, DT_INIT_ARRAY = 25,
       DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28, DT_GNU_HASH = 0x6ffffef5 };
enum { R_AARCH64_ABS64 = 257, R_AARCH64_GLOB_DAT = 1025, R_AARCH64_JUMP_SLOT = 1026, R_AARCH64_RELATIVE = 1027 };

uintptr_t floor_page(uintptr_t p) { return p & ~uintptr_t(page - 1); }
uintptr_t ceil_page(uintptr_t p) { return (p + page - 1) & ~uintptr_t(page - 1); }
struct Failure { std::string why; };
void require(bool ok, const std::string& why) { if (!ok) throw Failure{"game image: " + why}; }
bool set(std::string* error, const std::string& text) { if (error) *error = text; return false; }
} // namespace

void set_embedded_image(const uint8_t* data, size_t size) { embedded = data; embedded_size = size; }
bool reserve_window(std::string*) { return true; }   // nothing to do early: fixedmap checks each range when it maps it

void* map_fixed(uintptr_t address, size_t bytes) {
  void* staged = fixedmap_stage(bytes);
  if (!staged || fixedmap_commit(address, staged, bytes, "fixed range") || fixedmap_protect(address, bytes, FIXEDMAP_RW)) {
    std::fprintf(stderr, "fixedmap: %s\n", fixedmap_error());
    fixedmap_release(address);
    return MAP_FAILED;
  }
  return reinterpret_cast<void*>(address);
}
void unmap_fixed(uintptr_t address, size_t) { fixedmap_release(address); }
bool activate_mem1(std::string* error) {
  if (map_fixed(mem1, mem1_size) == MAP_FAILED) return set(error, std::string("cannot map main memory at 0x80000000: ") + fixedmap_error());
  return true;
}
void deactivate_mem1() { fixedmap_release(mem1); }

bool Image::load(const std::string& path, std::string* error) {
  uint8_t* staged = nullptr;
  size_t span = 0;
  try {
    require(mappings_.empty(), "already loaded");
    std::vector<uint8_t> owned;
    const uint8_t* file = embedded;
    size_t length = embedded_size;
    if (!file) {
      FILE* in = std::fopen(path.c_str(), "rb");
      require(in != nullptr, "cannot open " + path);
      std::fseek(in, 0, SEEK_END); const long n = std::ftell(in); std::fseek(in, 0, SEEK_SET);
      if (n > 0) { owned.resize(size_t(n)); if (std::fread(owned.data(), 1, owned.size(), in) != owned.size()) owned.clear(); }
      std::fclose(in);
      file = owned.data(); length = owned.size();
    }
    require(length >= sizeof(Ehdr) && length < (64u << 20), "invalid file size");
    Ehdr eh; std::memcpy(&eh, file, sizeof eh);
    require(!std::memcmp(eh.ident, "\177ELF\2\1\1", 7) && eh.type == 3 && eh.machine == 183 && eh.version == 1,
            "expected a little-endian AArch64 shared object");
    require(eh.phentsize == sizeof(Phdr) && eh.phnum > 0 && eh.phnum < 64 && eh.phoff <= length &&
            uint64_t(eh.phnum) * sizeof(Phdr) <= length - eh.phoff, "invalid program headers");
    std::vector<Phdr> headers(eh.phnum);
    std::memcpy(headers.data(), file + eh.phoff, headers.size() * sizeof(Phdr));

    const Phdr* dynamic = nullptr;
    std::vector<std::pair<uintptr_t, uintptr_t>> pages;
    uintptr_t image_end = image_base;
    for (const auto& p : headers) {
      require(p.type != PT_TLS, "thread-local storage is not supported");
      if (p.type == PT_DYNAMIC) dynamic = &p;
      if (p.type != PT_LOAD) continue;
      require(p.memsz && p.filesz <= p.memsz && p.offset <= length && p.filesz <= length - p.offset &&
              p.vaddr >= image_base && p.vaddr < window_end && p.memsz <= window_end - p.vaddr &&
              !((p.flags & PF_X) && (p.flags & PF_W)), "segment outside 0x82800000..0x84000000 or writable code");
      const uintptr_t lo = floor_page(p.vaddr), hi = ceil_page(p.vaddr + p.memsz);
      for (const auto& old : pages) require(hi <= old.first || lo >= old.second, "segments share a 4 KiB page");
      pages.push_back({lo, hi});
      image_end = std::max(image_end, hi);
      size_ = std::max(size_, uint32_t(p.vaddr + p.memsz - image_base));
    }
    require(dynamic && !pages.empty() && std::min_element(pages.begin(), pages.end())->first == image_base,
            "not linked at 0x82800000, or no dynamic section");

    span = image_end - image_base;
    staged = static_cast<uint8_t*>(fixedmap_stage(span));
    require(staged != nullptr, fixedmap_error());
    // `at(address)` is where the byte that will live at `address` sits in the staging pages.
    auto at = [&](uint64_t address) { return staged + (address - image_base); };
    auto valid = [&](uint64_t address, uint64_t n, uint32_t flags = 0) {
      for (const auto& p : headers)
        if (p.type == PT_LOAD && address >= p.vaddr && address - p.vaddr <= p.memsz && n <= p.memsz - (address - p.vaddr) &&
            (p.flags & flags) == flags) return true;
      return false;
    };
    for (const auto& p : headers) if (p.type == PT_LOAD) std::memcpy(at(p.vaddr), file + p.offset, p.filesz);

    require(valid(dynamic->vaddr, dynamic->memsz) && dynamic->memsz % sizeof(Dyn) == 0, "invalid dynamic section");
    std::map<int64_t, uint64_t> dt;
    for (size_t i = 0; i < dynamic->memsz / sizeof(Dyn); ++i) {
      Dyn d; std::memcpy(&d, at(dynamic->vaddr) + i * sizeof(Dyn), sizeof d);
      if (d.tag == DT_NULL) break;
      if (!dt.count(d.tag)) dt[d.tag] = d.value;    // DT_NEEDED repeats; the libraries it names are bound in mu_imports.c
    }
    require(dt[DT_SYMENT] == sizeof(Sym) && (!dt[DT_RELASZ] || dt[DT_RELAENT] == sizeof(Rela)) &&
            (!dt[DT_PLTRELSZ] || dt[DT_PLTREL] == DT_RELA), "unsupported dynamic format");
    symtab_ = dt[DT_SYMTAB]; strtab_ = dt[DT_STRTAB]; string_bytes_ = dt[DT_STRSZ];
    require(string_bytes_ && valid(strtab_, string_bytes_), "invalid string table");

    // The number of dynamic symbols is not stored directly: take it from the hash table.
    if (dt.count(DT_HASH)) {
      require(valid(dt[DT_HASH], 8), "invalid hash table");
      uint32_t n; std::memcpy(&n, at(dt[DT_HASH]) + 4, 4); symbol_count_ = n;
    } else {
      const uint64_t hash = dt[DT_GNU_HASH];
      require(hash && valid(hash, 16), "no symbol hash table");
      uint32_t head[4]; std::memcpy(head, at(hash), 16);
      const uint32_t buckets = head[0], first = head[1], bloom = head[2];
      require(buckets && buckets < 100000 && bloom < 100000, "invalid GNU hash table");
      const uint64_t bucket_at = hash + 16 + uint64_t(bloom) * 8, chains = bucket_at + uint64_t(buckets) * 4;
      require(valid(bucket_at, uint64_t(buckets) * 4), "invalid GNU hash buckets");
      symbol_count_ = first;
      for (uint32_t b = 0; b < buckets; ++b) {
        uint32_t s; std::memcpy(&s, at(bucket_at) + b * 4, 4);
        if (!s) continue;
        require(s >= first, "invalid GNU hash index");
        for (;; ++s) {
          require(s < 100000 && valid(chains + uint64_t(s - first) * 4, 4), "unterminated GNU hash chain");
          uint32_t value; std::memcpy(&value, at(chains + uint64_t(s - first) * 4), 4);
          symbol_count_ = std::max(symbol_count_, size_t(s) + 1);
          if (value & 1) break;
        }
      }
    }
    require(symbol_count_ && symbol_count_ < 100000 && valid(symtab_, symbol_count_ * sizeof(Sym)), "invalid symbol table");

    std::vector<uint64_t> resolved(symbol_count_);
    size_t imports = 0, weak = 0;
    for (size_t i = 1; i < symbol_count_; ++i) {
      Sym s; std::memcpy(&s, at(symtab_) + i * sizeof(Sym), sizeof s);
      require(s.name < string_bytes_, "invalid symbol name");
      const char* name = reinterpret_cast<const char*>(at(strtab_)) + s.name;
      require((s.info & 15) != 6 && (s.info & 15) != 10, "TLS or IFUNC symbol");
      if (s.shndx) resolved[i] = s.value;
      else if ((s.info >> 4) == 2) { resolved[i] = 0; ++weak; }     // undefined weak: absent, as when the library lacks it
      else {
        void* target = mu_import(name);
        require(target != nullptr, std::string("the image imports ") + name + ", which mu_imports.c does not provide");
        resolved[i] = reinterpret_cast<uint64_t>(target); ++imports;
      }
    }
    size_t relocations = 0;
    auto relocate = [&](uint64_t table, uint64_t bytes) {
      if (!bytes) return;
      require(bytes % sizeof(Rela) == 0 && valid(table, bytes), "invalid relocation table");
      for (size_t i = 0; i < bytes / sizeof(Rela); ++i) {
        Rela r; std::memcpy(&r, at(table) + i * sizeof(Rela), sizeof r);
        const uint32_t type = uint32_t(r.info); const uint64_t sym = r.info >> 32;
        require(valid(r.offset, 8, PF_W) && sym < symbol_count_, "relocation outside the writable segment");
        uint64_t value = 0;
        switch (type) {
          case R_AARCH64_RELATIVE: value = uint64_t(r.addend); break;
          case R_AARCH64_ABS64: case R_AARCH64_GLOB_DAT: case R_AARCH64_JUMP_SLOT: value = resolved[sym] + uint64_t(r.addend); break;
          default: require(false, "unsupported relocation type " + std::to_string(type));
        }
        std::memcpy(at(r.offset), &value, 8); ++relocations;
      }
    };
    relocate(dt[DT_RELA], dt[DT_RELASZ]);
    relocate(dt[DT_JMPREL], dt[DT_PLTRELSZ]);

    auto array = [&](int64_t address_tag, int64_t size_tag) {
      const uint64_t table = dt[address_tag], bytes = dt[size_tag];
      require(bytes % 8 == 0 && (!bytes || valid(table, bytes)), "invalid init/fini array");
      std::vector<uintptr_t> result;
      for (size_t i = 0; i < bytes / 8; ++i) {
        uint64_t fn; std::memcpy(&fn, at(table) + i * 8, 8);    // read after relocation
        require(valid(fn, 4, PF_X), "init/fini entry outside the code segment");
        result.push_back(uintptr_t(fn));
      }
      return result;
    };
    const auto init_array = array(DT_INIT_ARRAY, DT_INIT_ARRAYSZ);
    fini_array_ = array(DT_FINI_ARRAY, DT_FINI_ARRAYSZ);
    fini_ = dt[DT_FINI];
    require((!fini_ || valid(fini_, 4, PF_X)) && (!dt[DT_INIT] || valid(dt[DT_INIT], 4, PF_X)), "invalid init/fini");

    // From here on the staging pointer is gone and the image lives at its own addresses.
    uint8_t* pages_to_commit = staged; staged = nullptr;
    require(fixedmap_commit(image_base, pages_to_commit, span, "game image") == 0, fixedmap_error());
    mapped_ = true;
    for (const auto& p : headers) if (p.type == PT_LOAD) {
      const uintptr_t lo = floor_page(p.vaddr), hi = ceil_page(p.vaddr + p.memsz);
      const int prot = (p.flags & PF_X) ? FIXEDMAP_RX : (p.flags & PF_W) ? FIXEDMAP_RW : FIXEDMAP_R;
      require(fixedmap_protect(lo, hi - lo, prot) == 0, fixedmap_error());
      mappings_.push_back({reinterpret_cast<void*>(uintptr_t(p.vaddr)), uint32_t(p.memsz)});
      if (p.flags & PF_W) writable_.push_back(mappings_.back());
      std::fprintf(stderr, "game image: %lx..%lx %s\n", (unsigned long)p.vaddr, (unsigned long)(p.vaddr + p.memsz),
                   prot == FIXEDMAP_RX ? "r-x" : prot == FIXEDMAP_RW ? "rw-" : "r--");
    }
    std::fprintf(stderr, "game image: %zu symbols, %zu imports bound, %zu weak absent, %zu relocations\n",
                 symbol_count_, imports, weak, relocations);
    if (dt[DT_INIT]) reinterpret_cast<void (*)()>(uintptr_t(dt[DT_INIT]))();
    for (auto fn : init_array) reinterpret_cast<void (*)()>(fn)();
    initialized_ = true;
    return true;
  } catch (const Failure& failure) {
    // Staging pages that were never committed are only leaked on this error path (the back ends
    // allocate them differently); a failed load ends the run anyway.
    unload();
    return set(error, failure.why);
  }
}

void* Image::symbol(const char* name) const {
  if (!mapped_) return nullptr;
  const auto* symbols = reinterpret_cast<const Sym*>(symtab_);
  for (size_t i = 1; i < symbol_count_; ++i) {
    const auto& s = symbols[i];
    if (s.shndx && s.name < string_bytes_ && !std::strcmp(name, reinterpret_cast<const char*>(strtab_ + s.name)))
      return reinterpret_cast<void*>(uintptr_t(s.value));
  }
  return nullptr;
}

void Image::unload() {
  if (initialized_) {
    for (auto it = fini_array_.rbegin(); it != fini_array_.rend(); ++it) reinterpret_cast<void (*)()>(*it)();
    if (fini_) reinterpret_cast<void (*)()>(fini_)();
  }
  if (mapped_) fixedmap_release(image_base);
  mappings_.clear(); writable_.clear(); fini_array_.clear();
  fini_ = symtab_ = strtab_ = 0; symbol_count_ = string_bytes_ = 0; size_ = 0; initialized_ = false; mapped_ = false;
}
} // namespace mac_elf
