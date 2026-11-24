#!/usr/bin/env bash
# Builds Melee-NX-NetPlay (the native engine as the app; version in VERSION) where the build computer expects the app's NRO:
# dist/sd/switch/melee-nx-netplay/melee-nx-netplay.nro. Meant as tools/update-host.py's build command
# (MELEE_NX_UPDATE_BUILD, with update-host-native.patch applied); it can also be run by hand:
#   experiments/native-engine/frontend/app-build.sh /path/to/GALE01.iso
# The disc is not read by the build (the engine reads it on the console); the argument is accepted because
# update-host.py passes it.
#
# On an x86 build computer the aarch64 image runs under qemu (apt install qemu-user-static): same GCC 15.2, slower.
# Needs, besides Docker (every container runs with a time limit and no network, except the one git clone below):
#   - the images mu-arm-gcc15 (docker build -t mu-arm-gcc15 -f experiments/native-engine/linux-gcc15.Dockerfile
#     experiments/native-engine) and melee-nx-netplay-build (the app's own builder image, made by install.sh);
#   - build/sdk (the app's graphics SDK, unpacked by install.sh) and build/switch-aurora/_deps (the sources Aurora
#     fetched in one build of the app's aurora frontend): both exist after one install.sh run;
#   - the melee-unlocked tree at its pinned commit in build/native-engine/mu (../fetch.sh clones it once, the only
#     network access; MU_SOURCE=file:///path/to/clone fetches from a local copy instead).
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
cd "$repo"
step() { printf '\n== app-build: %s\n' "$*"; }
for need in build/sdk/SDK_MANIFEST.json build/switch-aurora/_deps/imgui-src; do
  [ -e "$need" ] || { echo "app-build: missing $need: run install.sh once on this computer (it builds the app's SDK and Aurora's sources)" >&2; exit 1; }
done
# The game's Linux pieces are built for arm64; on an x86 build computer they run under qemu, several times slower.
case "$(uname -m)" in aarch64|arm64) ;; *) export MU_TIMEOUT_SCALE="${MU_TIMEOUT_SCALE:-8}" ;; esac
docker image inspect mu-arm-gcc15 >/dev/null 2>&1 || { step "the aarch64 Linux image"; timeout 7200 docker build --platform linux/arm64 -t mu-arm-gcc15 -f experiments/native-engine/linux-gcc15.Dockerfile experiments/native-engine; }

step "melee-unlocked at its pinned commit"; bash experiments/native-engine/fetch.sh
step "the game image";                      bash experiments/native-engine/build.sh game
step "the netplay objects";                 bash experiments/native-engine/build.sh linux
                                            bash experiments/native-engine/build.sh nro
                                            MU_NET_SERVICES=1 bash experiments/native-engine/build.sh nro
step "the app (services NRO)";              bash "$here/build.sh" sync
                                            bash "$here/build.sh" nro
out="dist/sd/switch/melee-nx-netplay"
mkdir -p "$out"
cp build/native-engine-gfx/out/melee-nx-netplay.nro "$out/melee-nx-netplay.nro.tmp"
mv "$out/melee-nx-netplay.nro.tmp" "$out/melee-nx-netplay.nro"
echo "app-build: $out/melee-nx-netplay.nro (Melee-NX-NetPlay $(cat "$here/VERSION"), sha256 $(shasum -a 256 "$out/melee-nx-netplay.nro" | cut -c1-16))"
