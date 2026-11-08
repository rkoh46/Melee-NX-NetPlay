// SHA-1 identifies game assets only; SHA-256 binds build and executable artifacts.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "file_identity.h"
#include <array>
#include <cstdio>
#include <filesystem>
#include <vector>
#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#include <mach-o/dyld.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace host {
namespace {
bool file_digest(const std::string& path, bool sha256, std::string& digest, std::string& error) {
#if defined(__APPLE__)
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) { error = "cannot open identity input: " + path; return false; }
  CC_SHA1_CTX sha1_state;
  CC_SHA256_CTX sha256_state;
  if (sha256) CC_SHA256_Init(&sha256_state); else CC_SHA1_Init(&sha1_state);
  std::array<uint8_t, 64 * 1024> bytes;
  size_t count;
  while ((count = std::fread(bytes.data(), 1, bytes.size(), file)) != 0) {
    if (sha256) CC_SHA256_Update(&sha256_state, bytes.data(), static_cast<CC_LONG>(count));
    else CC_SHA1_Update(&sha1_state, bytes.data(), static_cast<CC_LONG>(count));
  }
  bool ok = std::ferror(file) == 0;
  std::fclose(file);
  if (!ok) { error = "cannot read identity input: " + path; return false; }
  std::array<uint8_t, CC_SHA256_DIGEST_LENGTH> hash{};
  if (sha256) CC_SHA256_Final(hash.data(), &sha256_state); else CC_SHA1_Final(hash.data(), &sha1_state);
  digest.clear();
  for (size_t i = 0; i < (sha256 ? CC_SHA256_DIGEST_LENGTH : CC_SHA1_DIGEST_LENGTH); ++i) {
    digest += "0123456789abcdef"[hash[i] >> 4]; digest += "0123456789abcdef"[hash[i] & 15];
  }
  return true;
#else
  (void)path; (void)sha256; (void)digest;
  error = "this platform has no configured streaming file identity provider";
  return false;
#endif
}
}
bool file_sha1(const std::string& path, std::string& digest, std::string& error) { return file_digest(path, false, digest, error); }
bool file_sha256(const std::string& path, std::string& digest, std::string& error) { return file_digest(path, true, digest, error); }
bool closed_file_artifact(const std::string& directory, const std::string& path, bool closed,
                          FileArtifact& artifact, std::string& error) {
  artifact = {};
  if (!closed || path.empty()) return true;
  namespace fs = std::filesystem;
  std::error_code ec;
  auto expected_parent = fs::canonical(directory, ec);
  if (ec) { error = "cannot resolve artifact directory"; return false; }
  if (fs::is_symlink(fs::symlink_status(path, ec)) || ec) { error = "artifact is missing or is a symlink"; return false; }
  auto actual = fs::canonical(path, ec);
  if (ec || actual.parent_path() != expected_parent || !fs::is_regular_file(actual, ec) || ec) {
    error = "artifact is not a regular file inside its isolated output directory"; return false;
  }
  artifact.path = actual.string();
  artifact.bytes = fs::file_size(actual, ec);
  if (ec || !file_sha256(artifact.path, artifact.sha256, error)) return false;
  if (fs::file_size(actual, ec) != artifact.bytes || ec) { error = "artifact changed while hashing"; return false; }
  artifact.present = true;
  return true;
}
bool executable_path(std::string& path, std::string& error) {
#if defined(__APPLE__)
  uint32_t size = 4096;
  std::vector<char> bytes(size);
  if (_NSGetExecutablePath(bytes.data(), &size) != 0) {
    bytes.resize(size);
    if (_NSGetExecutablePath(bytes.data(), &size) != 0) { error = "cannot resolve executable path"; return false; }
  }
  std::error_code ec;
  auto resolved = std::filesystem::canonical(bytes.data(), ec);
  if (ec) { error = "cannot canonicalize executable path"; return false; }
  path = resolved.string();
  return true;
#else
  (void)path;
  error = "this platform has no configured executable identity provider";
  return false;
#endif
}
}  // namespace host
#if defined(__APPLE__)
#pragma clang diagnostic pop
#endif
