#!/usr/bin/env bash
# Builds and runs tools/mod_install_test.cpp natively (macOS with the system's clang, libcurl and zlib, or Linux with g++),
# which also shows the installer library builds outside the Switch build. Needs ../fetch.sh first (the catalog and the
# LZMA SDK decoders come from the fetched melee-unlocked tree).
#   tools/mod_install_test.sh /path/to/disc-folder [mod ids...]
# The disc folder holds GALE01.iso (vanilla NTSC 1.02) and, optionally, mods/<id>/ with hand-patched discs to compare with
# (and copies of the large official downloads, used when their SHA-256 matches the catalog instead of fetching them again).
# Downloads come only from the catalog's own URLs and are kept in build/mod-downloads/ (fetched once). Built discs go to
# build/mod-install-test/ and are deleted after each comparison.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
repo="$(cd "$here/../.." && pwd -P)"
disc="${1:?folder with GALE01.iso}"; shift
mu="$repo/build/native-engine/mu"
src="$here/frontend/src/modinstall"
out="$repo/build/mod-install-test"; mkdir -p "$out/obj"
CXX="${CXX:-c++}"; CC="${CC:-cc}"
for c in LzmaDec Lzma2Dec; do "$CC" -O2 -c "$mu/port/third_party/lzma/$c.c" -I"$mu/port/third_party/lzma" -o "$out/obj/$c.o"; done
"$CXX" -std=c++17 -O2 -Wall -Wextra -o "$out/mod_install_test" "$here/tools/mod_install_test.cpp" \
  "$src"/mi_hash.cpp "$src"/mi_io.cpp "$src"/mi_archive.cpp "$src"/mi_lzma.cpp "$src"/mi_vcdiff.cpp "$src"/mi_catalog.cpp \
  "$src"/mod_install.cpp "$src"/mi_fetch_curl.cpp "$out/obj/LzmaDec.o" "$out/obj/Lzma2Dec.o" \
  -I"$src" -I"$mu/port/third_party/lzma" -I"$repo/port/third_party" -lcurl -lz -lpthread
"$out/mod_install_test" "$mu/mods-catalog-2.json" "$disc" "$repo/build/mod-downloads" "$out" "$@"
