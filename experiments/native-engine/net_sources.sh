#!/usr/bin/env bash
# The netplay object's sources: this repository's port/runtime/hle copied to OUT_DIR/port/runtime/hle with
# patches/net-*.patch applied. port/ itself is never changed. The patches may include headers from the fetched
# melee-unlocked tree (build/native-engine/mu): those are copied in here at build time and never committed.
#   net_sources.sh OUT_DIR      prints the stamp of what it made (a hash of the copy)
# Used by build.sh (net_build) and frontend/build.sh (the Linux services object).
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../.." && pwd -P)"
out="${1:?output folder}"; tree="$repo/build/native-engine/mu"
stage="$out.stage"; rm -rf "$stage"; mkdir -p "$stage/port/runtime"
cp -R "$repo/port/runtime/hle" "$stage/port/runtime/hle"
# Slippi 3.7.0's rules for receiving and using input suggestions, as melee-unlocked has them (header only).
[ -f "$tree/port/runtime/hle/slippi_input_suggestions.h" ] || { echo "net_sources: $tree is missing: run ./fetch.sh first" >&2; exit 1; }
cp "$tree/port/runtime/hle/slippi_input_suggestions.h" "$stage/port/runtime/hle/"
# melee-unlocked's live pick preview bytes (patches/net-0005; header only).
cp "$tree/port/runtime/abi/mu_pick_preview.h" "$stage/port/runtime/hle/"
for p in "$here"/patches/net-*.patch; do [ -f "$p" ] && patch -s -p1 -d "$stage" < "$p"; done
# melee-unlocked's own peer-to-peer match (runner/p2p_bridge.cpp): its mu_net session and the Monocypher it uses, as fetched
# (GPL-2.0-or-later; BSD-2-Clause or CC0-1.0), built over frontend/src/lobby_shim's Win32 names. nat_discovery.cpp is left out,
# as melee-unlocked's own build of the session leaves it out (peer_connector.cpp only uses its inline address classes).
mkdir -p "$stage/port/runtime/mu_net" "$stage/port/third_party/monocypher" "$stage/win32_shim"
for f in "$tree"/port/runtime/mu_net/*.h rollback_session peer_connector peer_identity input_transport session_security result_outbox session_types; do
  case "$f" in *.h) cp "$f" "$stage/port/runtime/mu_net/" ;; *) perl -pe 's/\r$//' "$tree/port/runtime/mu_net/$f.cpp" > "$stage/port/runtime/mu_net/$f.cpp" ;; esac
done
mkdir -p "$stage/port/runtime/host"; cp "$tree/port/runtime/host/early_input_rules.h" "$stage/port/runtime/host/"   # header only (input_transport.h)
cp "$tree"/port/third_party/monocypher/monocypher.[ch] "$tree"/port/third_party/monocypher/monocypher-ed25519.[ch] "$stage/port/third_party/monocypher/"
cp "$here"/frontend/src/lobby_shim/*.h "$here"/frontend/src/lobby_shim/lobby_shim.cpp "$stage/win32_shim/"
mkdir -p "$out"
# By content, so a build only sees files that changed.
rsync -rlpc --delete "$stage/" "$out/"
rm -rf "$stage"
(cd "$out" && find port win32_shim -type f | LC_ALL=C sort | xargs cat) | shasum -a 256 | cut -c1-32
