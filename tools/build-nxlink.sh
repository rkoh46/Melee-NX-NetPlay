#!/usr/bin/env bash
# Build devkitPro's nxlink natively for this computer (macOS or Linux) into tools/bin.
# nxlink sends an NRO to the Homebrew Menu's netloader (press Y in hbmenu) and, with -s,
# streams the program's stdout back. It is one C file that needs only zlib.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
src="${SWITCH_TOOLS_SRC:-$here/../build/switch-tools}"
if [[ ! -f "$src/src/nxlink.c" ]]; then
  git clone --quiet --depth 1 https://github.com/switchbrew/switch-tools.git "$src"
fi
mkdir -p "$here/bin"
cc -O2 -Wno-pointer-sign -o "$here/bin/nxlink" "$src/src/nxlink.c" -lz
echo "built $here/bin/nxlink"
