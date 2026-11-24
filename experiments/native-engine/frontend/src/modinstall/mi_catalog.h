// melee-unlocked's verified mod catalog (mods-catalog-2.json in the fetched tree): what each mod is, where its
// official download is, and the SHA-256 of the download, of the patch inside it and of the finished disc.
// The rules are melee-unlocked's (port/app/launcher_mod_catalog.cpp `parse`): every field is checked, links must
// be https, hashes must be well formed, ids unique, and a mod gets automatic download ("one click") only when the
// catalog records a licence or a permission for it (policy "licensed" or "permission_granted"); any other entry
// is listed with its page, and the player fetches the file and puts it where the installer looks.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace modinstall {

struct CatalogMod {
  std::string id, name, credits, license, page, repo, tag, asset_pattern;
  std::string kind;                 // "gci", "xdelta_zip" or "xdelta_7z" (a bare .xdelta download is "xdelta_zip" too)
  bool one_click = false;           // automatic download allowed (after the policy rule)
  std::string sha256;               // the download
  std::string output_md5;
  std::string version, url, tool_sha256, patch_sha256, policy;
  std::string output_sha256, note;  // the finished disc; a one-line note
  // Known sizes (this program's table, keyed by the download's SHA-256 so they apply to that exact file): the
  // download and the finished disc, 0 when not known.
  uint64_t download_bytes = 0, disc_bytes = 0;
  bool is_disc() const { return kind != "gci"; }
  bool direct_file() const;         // the url names the file itself (not a page such as MediaFire's)
};

// Parses and checks a catalog; false with *error set when the text breaks a rule (nothing is accepted then).
bool parse_catalog(const std::string& text, std::vector<CatalogMod>* mods, std::string* error);
bool valid_sha256(const std::string& value);
bool valid_id(const std::string& value);

}  // namespace modinstall
