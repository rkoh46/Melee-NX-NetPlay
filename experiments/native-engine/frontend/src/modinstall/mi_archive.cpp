// SPDX-License-Identifier: GPL-3.0-or-later
#include "mi_archive.h"
#include "mi_hash.h"
#include "mi_lzma.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace modinstall {
namespace {

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }

bool read_at(FILE* f, uint64_t offset, void* out, size_t size) {
  return fseeko(f, off_t(offset), SEEK_SET) == 0 && std::fread(out, 1, size, f) == size;
}

// A stream that checks the CRC-32 and the length of what it hands on.
class CheckedSource : public ByteSource {
 public:
  CheckedSource(std::unique_ptr<ByteSource> inner, uint64_t size, bool has_crc, uint32_t crc)
      : inner_(std::move(inner)), size_(size), has_crc_(has_crc), crc_(crc) {}
  long read(uint8_t* out, size_t size) override {
    if (pos_ >= size_) {
      if (!checked_) {
        checked_ = true;
        if (has_crc_ && running_ != crc_) { error_ = "the archive entry's CRC-32 does not match"; return -1; }
      }
      return 0;
    }
    if (size > size_ - pos_) size = size_t(size_ - pos_);
    const long got = inner_->read(out, size);
    if (got < 0) { error_ = inner_->error(); return -1; }
    if (got == 0) { error_ = "the archive entry ends early"; return -1; }
    running_ = crc32_update(running_, out, size_t(got));
    pos_ += uint64_t(got);
    return got;
  }
  uint64_t total() const override { return size_; }
 private:
  std::unique_ptr<ByteSource> inner_;
  uint64_t size_;
  bool has_crc_, checked_ = false;
  uint32_t crc_, running_ = 0;
};

// ---- zip ----
class InflateSource : public ByteSource {
 public:
  InflateSource(const std::string& path, uint64_t offset, uint64_t csize) : in_(path, offset, csize) {
    std::memset(&z_, 0, sizeof z_);
    ok_ = in_.ok() && inflateInit2(&z_, -15) == Z_OK;
    if (!ok_) error_ = "cannot start inflating the archive entry";
    buf_.resize(1u << 18);
  }
  ~InflateSource() override { if (ok_) inflateEnd(&z_); }
  long read(uint8_t* out, size_t size) override {
    if (!ok_) return -1;
    if (done_) return 0;
    z_.next_out = out;
    z_.avail_out = uInt(std::min<size_t>(size, 1u << 30));
    while (z_.avail_out == uInt(std::min<size_t>(size, 1u << 30))) {
      if (z_.avail_in == 0) {
        const long got = in_.read(buf_.data(), buf_.size());
        if (got < 0) { error_ = in_.error(); return -1; }
        z_.next_in = buf_.data();
        z_.avail_in = uInt(got);
        if (got == 0) { error_ = "the compressed entry ends early"; return -1; }
      }
      const int r = inflate(&z_, Z_NO_FLUSH);
      if (r == Z_STREAM_END) { done_ = true; break; }
      if (r != Z_OK && r != Z_BUF_ERROR) { error_ = std::string("inflate: ") + (z_.msg ? z_.msg : "data error"); return -1; }
    }
    const long produced = long(std::min<size_t>(size, 1u << 30) - z_.avail_out);
    pos_ += uint64_t(produced);
    return produced;
  }
 private:
  FileSource in_;
  z_stream z_;
  bool ok_ = false, done_ = false;
  std::vector<uint8_t> buf_;
};

class ZipArchive : public Archive {
 public:
  struct Item { uint32_t crc = 0; uint16_t method = 0, flags = 0; uint64_t csize = 0, local = 0; };
  bool load(const std::string& path, std::string* error) {
    path_ = path;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { *error = "cannot open the archive"; return false; }
    const uint64_t size = file_size(path);
    const size_t tail = size_t(std::min<uint64_t>(size, 65536 + 22));
    std::vector<uint8_t> end(tail);
    if (tail < 22 || !read_at(f, size - tail, end.data(), tail)) { std::fclose(f); *error = "the zip is too short"; return false; }
    long eocd = -1;
    for (long i = long(tail) - 22; i >= 0; --i) if (le32(&end[size_t(i)]) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { std::fclose(f); *error = "the zip's directory was not found"; return false; }
    uint64_t count = le16(&end[size_t(eocd) + 10]), cd_size = le32(&end[size_t(eocd) + 12]), cd_offset = le32(&end[size_t(eocd) + 16]);
    if (count == 0xFFFF || cd_offset == 0xFFFFFFFFu) {   // zip64: the locator sits right before the end record
      const uint64_t loc_at = size - tail + uint64_t(eocd) - 20;
      uint8_t loc[20], z64[56];
      if (!read_at(f, loc_at, loc, 20) || le32(loc) != 0x07064b50 || !read_at(f, le64(loc + 8), z64, 56) || le32(z64) != 0x06064b50) {
        std::fclose(f); *error = "the zip64 directory was not found"; return false;
      }
      count = le64(z64 + 32); cd_size = le64(z64 + 40); cd_offset = le64(z64 + 48);
    }
    if (cd_size > (64u << 20) || cd_offset + cd_size > size) { std::fclose(f); *error = "the zip's directory is invalid"; return false; }
    std::vector<uint8_t> cd(static_cast<size_t>(cd_size));
    if (!read_at(f, cd_offset, cd.data(), cd.size())) { std::fclose(f); *error = "cannot read the zip's directory"; return false; }
    std::fclose(f);
    size_t at = 0;
    for (uint64_t i = 0; i < count; ++i) {
      if (at + 46 > cd.size() || le32(&cd[at]) != 0x02014b50) { *error = "the zip's directory is damaged"; return false; }
      const uint8_t* h = &cd[at];
      Item item;
      item.flags = le16(h + 8); item.method = le16(h + 10); item.crc = le32(h + 16);
      item.csize = le32(h + 20);
      uint64_t usize = le32(h + 24);
      item.local = le32(h + 42);
      const size_t name_len = le16(h + 28), extra_len = le16(h + 30), comment_len = le16(h + 32);
      if (at + 46 + name_len + extra_len + comment_len > cd.size()) { *error = "the zip's directory is damaged"; return false; }
      ArchiveEntry e;
      e.name.assign(reinterpret_cast<const char*>(h + 46), name_len);
      std::replace(e.name.begin(), e.name.end(), '\\', '/');
      // zip64 extra field: the sizes and offset that did not fit.
      for (size_t x = 0; x + 4 <= extra_len;) {
        const uint8_t* ex = h + 46 + name_len + x;
        const uint16_t id = le16(ex), len = le16(ex + 2);
        if (x + 4 + len > extra_len) break;
        if (id == 1) {
          size_t k = 4;
          if (usize == 0xFFFFFFFFu && k + 8 <= 4u + len) { usize = le64(ex + k); k += 8; }
          if (item.csize == 0xFFFFFFFFu && k + 8 <= 4u + len) { item.csize = le64(ex + k); k += 8; }
          if (item.local == 0xFFFFFFFFu && k + 8 <= 4u + len) { item.local = le64(ex + k); k += 8; }
        }
        x += 4 + len;
      }
      e.size = usize;
      e.directory = !e.name.empty() && e.name.back() == '/';
      entries_.push_back(e);
      items_.push_back(item);
      at += 46 + name_len + extra_len + comment_len;
    }
    return true;
  }
  const std::vector<ArchiveEntry>& entries() const override { return entries_; }
  std::unique_ptr<ByteSource> open(size_t index, std::string* error) override {
    if (index >= items_.size()) { *error = "no such entry"; return nullptr; }
    const Item& item = items_[index];
    if (item.flags & 1) { *error = "the entry is encrypted"; return nullptr; }
    if (item.method != 0 && item.method != 8) { *error = "the entry uses compression method " + std::to_string(item.method); return nullptr; }
    FILE* f = std::fopen(path_.c_str(), "rb");
    uint8_t local[30];
    const bool ok = f && read_at(f, item.local, local, 30) && le32(local) == 0x04034b50;
    if (f) std::fclose(f);
    if (!ok) { *error = "the entry's local header is damaged"; return nullptr; }
    const uint64_t data = item.local + 30 + le16(local + 26) + le16(local + 28);
    std::unique_ptr<ByteSource> raw;
    if (item.method == 0) raw.reset(new FileSource(path_, data, item.csize));
    else raw.reset(new InflateSource(path_, data, item.csize));
    return std::unique_ptr<ByteSource>(new CheckedSource(std::move(raw), entries_[index].size, true, item.crc));
  }
 private:
  std::string path_;
  std::vector<ArchiveEntry> entries_;
  std::vector<Item> items_;
};

// ---- 7z ----
// Property ids of the 7z header (7zFormat.txt).
enum : uint8_t {
  kEnd = 0x00, kHeader = 0x01, kArchiveProperties = 0x02, kAdditionalStreamsInfo = 0x03, kMainStreamsInfo = 0x04,
  kFilesInfo = 0x05, kPackInfo = 0x06, kUnPackInfo = 0x07, kSubStreamsInfo = 0x08, kSize = 0x09, kCRC = 0x0A,
  kFolder = 0x0B, kCodersUnPackSize = 0x0C, kNumUnPackStream = 0x0D, kEmptyStream = 0x0E, kEmptyFile = 0x0F,
  kName = 0x11, kEncodedHeader = 0x17, kDummy = 0x19,
};

struct Reader {
  const uint8_t* p; size_t n, at = 0; bool bad = false;
  uint8_t byte() { if (at >= n) { bad = true; return 0; } return p[at++]; }
  uint64_t number() {
    const uint8_t first = byte();
    uint8_t mask = 0x80;
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      if ((first & mask) == 0) return value | (uint64_t(first & (mask - 1)) << (8 * i));
      value |= uint64_t(byte()) << (8 * i);
      mask >>= 1;
    }
    return value;
  }
  uint32_t u32() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(byte()) << (8 * i); return v; }
  void skip(uint64_t count) { if (count > n - at) { bad = true; at = n; } else at += size_t(count); }
  std::vector<bool> bits(size_t count) {
    std::vector<bool> out(count);
    uint8_t b = 0, mask = 0;
    for (size_t i = 0; i < count; ++i) { if (!mask) { b = byte(); mask = 0x80; } out[i] = (b & mask) != 0; mask >>= 1; }
    return out;
  }
  std::vector<bool> defined(size_t count) { return byte() ? std::vector<bool>(count, true) : bits(count); }
};

struct Coder { std::vector<uint8_t> id, props; uint64_t in = 1, out = 1; };
struct Folder {
  std::vector<Coder> coders;
  uint64_t pack_index = 0, pack_count = 1;   // its packed streams in the archive's list
  uint64_t unpack_size = 0;
  bool has_crc = false; uint32_t crc = 0;
  uint64_t streams = 1;                      // the files packed in it (substreams)
};
struct StreamsInfo {
  uint64_t pack_pos = 0;
  std::vector<uint64_t> pack_sizes;
  std::vector<Folder> folders;
  std::vector<uint64_t> sub_sizes;           // each substream's size, folder by folder
  std::vector<bool> sub_has_crc;
  std::vector<uint32_t> sub_crc;
};

bool read_streams_info(Reader& r, StreamsInfo* s, std::string* error) {
  uint8_t id = r.byte();
  if (id == kPackInfo) {
    s->pack_pos = r.number();
    const uint64_t count = r.number();
    if (count > 4096) { *error = "too many packed streams"; return false; }
    for (id = r.byte(); id != kEnd && !r.bad; id = r.byte()) {
      if (id == kSize) for (uint64_t i = 0; i < count; ++i) s->pack_sizes.push_back(r.number());
      else if (id == kCRC) { const auto d = r.defined(size_t(count)); for (bool b : d) if (b) r.u32(); }
      else { *error = "unknown pack property"; return false; }
    }
    id = r.byte();
  }
  if (id == kUnPackInfo) {
    if (r.byte() != kFolder) { *error = "the 7z folder list is missing"; return false; }
    const uint64_t count = r.number();
    if (count > 4096 || r.byte() != 0) { *error = "unsupported 7z folder list"; return false; }
    uint64_t pack_index = 0;
    for (uint64_t f = 0; f < count; ++f) {
      Folder folder;
      const uint64_t coders = r.number();
      if (coders == 0 || coders > 8) { *error = "unsupported 7z coder count"; return false; }
      uint64_t total_in = 0, total_out = 0;
      for (uint64_t c = 0; c < coders; ++c) {
        Coder coder;
        const uint8_t flags = r.byte();
        for (int i = 0; i < (flags & 0x0F); ++i) coder.id.push_back(r.byte());
        if (flags & 0x10) { coder.in = r.number(); coder.out = r.number(); }
        if (flags & 0x20) { const uint64_t size = r.number(); if (size > 64) { *error = "invalid coder properties"; return false; } for (uint64_t i = 0; i < size; ++i) coder.props.push_back(r.byte()); }
        if (flags & 0x80) { *error = "unsupported 7z coder flags"; return false; }
        total_in += coder.in; total_out += coder.out;
        folder.coders.push_back(coder);
      }
      if (total_out == 0 || total_out > 16 || total_in > 16) { *error = "unsupported 7z streams"; return false; }
      for (uint64_t b = 0; b + 1 < total_out; ++b) { r.number(); r.number(); }   // bind pairs
      folder.pack_count = total_in - (total_out - 1);
      if (folder.pack_count > 1) for (uint64_t i = 0; i < folder.pack_count; ++i) r.number();
      folder.pack_index = pack_index;
      pack_index += folder.pack_count;
      s->folders.push_back(folder);
    }
    if (r.byte() != kCodersUnPackSize) { *error = "the 7z unpack sizes are missing"; return false; }
    for (auto& folder : s->folders) {
      uint64_t outs = 0;
      for (const auto& c : folder.coders) outs += c.out;
      for (uint64_t i = 0; i < outs; ++i) { const uint64_t v = r.number(); if (i == 0) folder.unpack_size = v; }
    }
    for (id = r.byte(); id != kEnd && !r.bad; id = r.byte()) {
      if (id == kCRC) {
        const auto d = r.defined(s->folders.size());
        for (size_t i = 0; i < d.size(); ++i) if (d[i]) { s->folders[i].has_crc = true; s->folders[i].crc = r.u32(); }
      } else { *error = "unknown folder property"; return false; }
    }
    id = r.byte();
  }
  // Each folder holds one file unless the substreams say otherwise.
  if (id == kSubStreamsInfo) {
    id = r.byte();
    if (id == kNumUnPackStream) { for (auto& folder : s->folders) folder.streams = r.number(); id = r.byte(); }
    for (auto& folder : s->folders) {
      if (folder.streams == 0) continue;
      uint64_t sum = 0;
      if (id == kSize) for (uint64_t i = 1; i < folder.streams; ++i) { const uint64_t v = r.number(); s->sub_sizes.push_back(v); sum += v; }
      else if (folder.streams > 1) { *error = "the 7z substream sizes are missing"; return false; }
      if (sum > folder.unpack_size) { *error = "invalid 7z substream sizes"; return false; }
      s->sub_sizes.push_back(folder.unpack_size - sum);
    }
    if (id == kSize) id = r.byte();
    // CRCs: given here for every stream whose folder has no CRC of its own (or more than one stream).
    size_t unknown = 0;
    for (const auto& folder : s->folders) if (!(folder.streams == 1 && folder.has_crc)) unknown += size_t(folder.streams);
    std::vector<bool> has(unknown);
    std::vector<uint32_t> crc(unknown);
    while (id != kEnd && !r.bad) {
      if (id == kCRC) { has = r.defined(unknown); for (size_t i = 0; i < unknown; ++i) if (has[i]) crc[i] = r.u32(); }
      else { *error = "unknown substream property"; return false; }
      id = r.byte();
    }
    size_t k = 0;
    for (const auto& folder : s->folders) {
      if (folder.streams == 1 && folder.has_crc) { s->sub_has_crc.push_back(true); s->sub_crc.push_back(folder.crc); continue; }
      for (uint64_t i = 0; i < folder.streams; ++i, ++k) { s->sub_has_crc.push_back(has[k]); s->sub_crc.push_back(crc[k]); }
    }
    id = r.byte();
  } else {
    for (const auto& folder : s->folders) { s->sub_sizes.push_back(folder.unpack_size); s->sub_has_crc.push_back(folder.has_crc); s->sub_crc.push_back(folder.crc); }
  }
  if (id != kEnd || r.bad) { *error = "the 7z streams information is damaged"; return false; }
  return true;
}

// Decodes one folder (one LZMA, LZMA2 or Copy coder) from its packed stream.
class FolderSource : public ByteSource {
 public:
  FolderSource(const std::string& path, uint64_t pack_offset, uint64_t pack_size, const Folder& folder)
      : in_(path, pack_offset, pack_size), size_(folder.unpack_size) {
    if (!in_.ok()) { error_ = in_.error(); return; }
    const Coder& c = folder.coders[0];
    if (folder.coders.size() != 1 || folder.pack_count != 1) { error_ = "7z folders with more than one coder (filters) are not supported"; return; }
    if (c.id == std::vector<uint8_t>{0x00}) kind_ = Kind::Copy;
    else if (c.id == std::vector<uint8_t>{0x21} && c.props.size() == 1) { kind_ = Kind::Lzma2; dec_.reset(new LzmaStream(LzmaStream::Lzma2, c.props.data(), 1)); }
    else if (c.id == std::vector<uint8_t>{0x03, 0x01, 0x01} && c.props.size() == 5) { kind_ = Kind::Lzma; dec_.reset(new LzmaStream(LzmaStream::Lzma1, c.props.data(), 5)); }
    else { error_ = "the 7z coder " + to_hex(c.id.data(), c.id.size()) + " is not supported"; return; }
    if (dec_ && !dec_->ok()) { error_ = "not enough memory for the LZMA decoder"; kind_ = Kind::None; }
    buf_.resize(1u << 18);
  }
  long read(uint8_t* out, size_t size) override {
    if (kind_ == Kind::None) return -1;
    if (pos_ >= size_) return 0;
    if (size > size_ - pos_) size = size_t(size_ - pos_);
    if (kind_ == Kind::Copy) {
      const long got = in_.read(out, size);
      if (got < 0) error_ = in_.error();
      if (got > 0) pos_ += uint64_t(got);
      return got;
    }
    size_t produced = 0;
    while (produced < size) {
      if (avail_ == 0 && !in_end_) {
        const long got = in_.read(buf_.data(), buf_.size());
        if (got < 0) { error_ = in_.error(); return -1; }
        if (got == 0) in_end_ = true;
        at_ = 0; avail_ = size_t(got);
      }
      size_t in_used = avail_, out_made = size - produced;
      const int r = dec_->decode(buf_.data() + at_, &in_used, out + produced, &out_made);
      at_ += in_used; avail_ -= in_used; produced += out_made;
      if (r < 0) { error_ = "the 7z data is damaged (LZMA)"; return -1; }
      if (out_made == 0 && in_used == 0) {
        if (in_end_ || r > 0) { error_ = "the 7z data ends early"; return -1; }
      }
    }
    pos_ += produced;
    return long(produced);
  }
  uint64_t total() const override { return size_; }
 private:
  enum class Kind { None, Copy, Lzma, Lzma2 };
  FileSource in_;
  uint64_t size_;
  Kind kind_ = Kind::None;
  std::unique_ptr<LzmaStream> dec_;
  std::vector<uint8_t> buf_;
  size_t at_ = 0, avail_ = 0;
  bool in_end_ = false;
};

// Skips the first `skip` bytes of a folder (the files packed before the one wanted), then hands on `size`.
class SliceSource : public ByteSource {
 public:
  SliceSource(std::unique_ptr<ByteSource> inner, uint64_t skip, uint64_t size) : inner_(std::move(inner)), skip_(skip), size_(size) {}
  long read(uint8_t* out, size_t size) override {
    std::vector<uint8_t> scratch;
    while (skip_) {
      if (scratch.empty()) scratch.resize(1u << 18);
      const long got = inner_->read(scratch.data(), size_t(std::min<uint64_t>(skip_, scratch.size())));
      if (got <= 0) { error_ = got < 0 ? inner_->error() : "the 7z folder ends early"; return -1; }
      skip_ -= uint64_t(got);
    }
    if (pos_ >= size_) return 0;
    if (size > size_ - pos_) size = size_t(size_ - pos_);
    const long got = inner_->read(out, size);
    if (got < 0) error_ = inner_->error();
    if (got > 0) pos_ += uint64_t(got);
    return got;
  }
  uint64_t total() const override { return size_; }
 private:
  std::unique_ptr<ByteSource> inner_;
  uint64_t skip_, size_;
};

class SevenZipArchive : public Archive {
 public:
  bool load(const std::string& path, std::string* error) {
    path_ = path;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { *error = "cannot open the archive"; return false; }
    uint8_t sig[32];
    if (!read_at(f, 0, sig, 32)) { std::fclose(f); *error = "the 7z is too short"; return false; }
    const uint64_t next_offset = le64(sig + 12), next_size = le64(sig + 20);
    const uint32_t next_crc = le32(sig + 28);
    const uint64_t size = file_size(path);
    if (next_size == 0 || next_size > (16u << 20) || 32 + next_offset + next_size > size) { std::fclose(f); *error = "the 7z header is invalid"; return false; }
    std::vector<uint8_t> header(static_cast<size_t>(next_size));
    const bool read = read_at(f, 32 + next_offset, header.data(), header.size());
    std::fclose(f);
    if (!read || crc32_update(0, header.data(), header.size()) != next_crc) { *error = "the 7z header is damaged"; return false; }
    // An encoded header: the real header is itself packed, as a folder described here.
    for (int depth = 0; depth < 4 && !header.empty() && header[0] == kEncodedHeader; ++depth) {
      Reader r{header.data() + 1, header.size() - 1};
      StreamsInfo s;
      if (!read_streams_info(r, &s, error)) return false;
      if (s.folders.size() != 1 || s.pack_sizes.empty() || s.folders[0].unpack_size > (64u << 20)) { *error = "unsupported 7z encoded header"; return false; }
      auto src = folder_stream(s, 0, error);
      if (!src) return false;
      std::vector<uint8_t> plain(static_cast<size_t>(s.folders[0].unpack_size));
      size_t at = 0;
      while (at < plain.size()) {
        const long got = src->read(plain.data() + at, plain.size() - at);
        if (got <= 0) { *error = got < 0 ? src->error() : "the 7z header ends early"; return false; }
        at += size_t(got);
      }
      if (s.folders[0].has_crc && crc32_update(0, plain.data(), plain.size()) != s.folders[0].crc) { *error = "the 7z header's CRC does not match"; return false; }
      header = std::move(plain);
    }
    Reader r{header.data(), header.size()};
    if (r.byte() != kHeader) { *error = "the 7z header is not understood"; return false; }
    uint8_t id = r.byte();
    if (id == kArchiveProperties) {
      for (uint8_t t = r.byte(); t != kEnd && !r.bad; t = r.byte()) r.skip(r.number());
      id = r.byte();
    }
    if (id == kAdditionalStreamsInfo) { *error = "7z additional streams are not supported"; return false; }
    if (id == kMainStreamsInfo) {
      if (!read_streams_info(r, &main_, error)) return false;
      id = r.byte();
    }
    std::vector<bool> empty_stream, empty_file;
    if (id == kFilesInfo) {
      const uint64_t count = r.number();
      if (count > 100000) { *error = "too many files in the 7z"; return false; }
      entries_.resize(size_t(count));
      for (uint8_t t = r.byte(); t != kEnd && !r.bad; t = r.byte()) {
        const uint64_t prop_size = r.number();
        const size_t start = r.at;
        if (prop_size > r.n - r.at) { *error = "the 7z file list is damaged"; return false; }
        if (t == kEmptyStream) empty_stream = r.bits(size_t(count));
        else if (t == kEmptyFile) { size_t empties = 0; for (bool b : empty_stream) empties += b; empty_file = r.bits(empties); }
        else if (t == kName) {
          if (r.byte() != 0) { *error = "external 7z names are not supported"; return false; }
          for (auto& e : entries_) {
            std::string name;
            for (;;) {
              const uint32_t lo = r.byte(), hi = r.byte();
              uint32_t c = lo | hi << 8;
              if (c == 0 || r.bad) break;
              if (c >= 0xD800 && c < 0xDC00) { const uint32_t lo2 = r.byte(), hi2 = r.byte(); c = 0x10000 + ((c - 0xD800) << 10) + (((lo2 | hi2 << 8)) - 0xDC00); }
              if (c < 0x80) name += char(c);
              else if (c < 0x800) { name += char(0xC0 | c >> 6); name += char(0x80 | (c & 63)); }
              else if (c < 0x10000) { name += char(0xE0 | c >> 12); name += char(0x80 | ((c >> 6) & 63)); name += char(0x80 | (c & 63)); }
              else { name += char(0xF0 | c >> 18); name += char(0x80 | ((c >> 12) & 63)); name += char(0x80 | ((c >> 6) & 63)); name += char(0x80 | (c & 63)); }
            }
            std::replace(name.begin(), name.end(), '\\', '/');
            e.name = name;
          }
        }
        r.at = start + size_t(prop_size);
      }
    }
    if (r.bad) { *error = "the 7z header is damaged"; return false; }
    // Files with data take the substreams in order; the substreams fill the folders in order.
    size_t stream = 0, empty_index = 0;
    std::vector<size_t> folder_of;
    for (size_t fo = 0, k = 0; fo < main_.folders.size(); ++fo) for (uint64_t i = 0; i < main_.folders[fo].streams; ++i, ++k) folder_of.push_back(fo);
    for (size_t i = 0; i < entries_.size(); ++i) {
      Place place;
      const bool empty = i < empty_stream.size() && empty_stream[i];
      if (empty) {
        const bool is_file = empty_index < empty_file.size() && empty_file[empty_index];
        entries_[i].directory = !is_file;
        ++empty_index;
      } else {
        if (stream >= main_.sub_sizes.size()) { *error = "the 7z file list and streams disagree"; return false; }
        place.stream = long(stream);
        entries_[i].size = main_.sub_sizes[stream];
        ++stream;
      }
      places_.push_back(place);
    }
    folder_of_ = folder_of;
    return true;
  }
  const std::vector<ArchiveEntry>& entries() const override { return entries_; }
  std::unique_ptr<ByteSource> open(size_t index, std::string* error) override {
    if (index >= places_.size()) { *error = "no such entry"; return nullptr; }
    const long stream = places_[index].stream;
    if (stream < 0) return std::unique_ptr<ByteSource>(new FileSource(path_, 0, 0));
    const size_t folder = folder_of_[size_t(stream)];
    uint64_t skip = 0;
    for (long s = 0; s < stream; ++s) if (folder_of_[size_t(s)] == folder) skip += main_.sub_sizes[size_t(s)];
    auto src = folder_stream(main_, folder, error);
    if (!src) return nullptr;
    const uint64_t size = main_.sub_sizes[size_t(stream)];
    std::unique_ptr<ByteSource> slice(new SliceSource(std::move(src), skip, size));
    return std::unique_ptr<ByteSource>(new CheckedSource(std::move(slice), size, main_.sub_has_crc[size_t(stream)], main_.sub_crc[size_t(stream)]));
  }
 private:
  struct Place { long stream = -1; };
  std::unique_ptr<ByteSource> folder_stream(const StreamsInfo& s, size_t folder, std::string* error) {
    const Folder& f = s.folders[folder];
    if (f.pack_index >= s.pack_sizes.size()) { *error = "the 7z packed streams are missing"; return nullptr; }
    uint64_t offset = 32 + s.pack_pos;
    for (uint64_t i = 0; i < f.pack_index; ++i) offset += s.pack_sizes[size_t(i)];
    std::unique_ptr<FolderSource> src(new FolderSource(path_, offset, s.pack_sizes[size_t(f.pack_index)], f));
    if (!src->error().empty()) { *error = src->error(); return nullptr; }
    return src;
  }
  std::string path_;
  StreamsInfo main_;
  std::vector<ArchiveEntry> entries_;
  std::vector<Place> places_;
  std::vector<size_t> folder_of_;
};

}  // namespace

std::unique_ptr<Archive> open_archive(const std::string& path, std::string* error) {
  uint8_t magic[6] = {};
  if (FILE* f = std::fopen(path.c_str(), "rb")) { const size_t n = std::fread(magic, 1, 6, f); std::fclose(f); if (n != 6) { *error = "the file is too short"; return nullptr; } }
  else { *error = "cannot open " + path; return nullptr; }
  if (le32(magic) == 0x04034b50 || le32(magic) == 0x06054b50) {
    std::unique_ptr<ZipArchive> z(new ZipArchive);
    if (!z->load(path, error)) return nullptr;
    return z;
  }
  static const uint8_t k7z[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
  if (std::memcmp(magic, k7z, 6) == 0) {
    std::unique_ptr<SevenZipArchive> z(new SevenZipArchive);
    if (!z->load(path, error)) return nullptr;
    return z;
  }
  *error = "not a zip or 7z archive";
  return nullptr;
}

long find_entry(const Archive& archive, const std::string& suffix) {
  const std::string want = lower(suffix);
  const auto& list = archive.entries();
  for (size_t i = 0; i < list.size(); ++i) {
    if (list[i].directory) continue;
    const std::string name = lower(list[i].name);
    if (name.size() >= want.size() && name.compare(name.size() - want.size(), want.size(), want) == 0) return long(i);
  }
  return -1;
}

}  // namespace modinstall
