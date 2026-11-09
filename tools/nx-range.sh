#!/usr/bin/env bash
# Point the profiler's caller table at one function of the current Aurora ELF:
#   tools/nx-range.sh <ip> <symbol-regex>      writes profile_range=LO:HI:ANCHOR into launcher.ini and pushes it
# The profile then has "rangelr" tables: who called that function (tools/nx-profile.sh names them).
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"; repo="$(cd "$here/.." && pwd -P)"
ip="${1:?ip}"; sym="${2:?symbol}"
image="${MELEE_DOCKER_IMAGE:-melee-nx-netplay-build:latest}"; tc=/opt/devkitpro/devkitA64/bin/aarch64-none-elf
elf="build/switch-${3:-aurora}/port/melee-nx-netplay.elf"
table=$(docker run --rm -v "$repo:/project" "$image" $tc-nm -S -C "/project/$elf" </dev/null 2>/dev/null)
line=$(echo "$table" | awk -v re="$sym" '$4 ~ re && $3 ~ /[tTW]/ && !found {print; found=1}')   # no early exit: SIGPIPE under pipefail
[[ -n "$line" ]] || { echo "no symbol matching $sym" >&2; exit 65; }
lo=$(echo "$line" | awk '{print $1}'); size=$(echo "$line" | awk '{print $2}')
hi=$(printf '%x' $(( 0x$lo + 0x$size )))
anchor=$(echo "$table" | awk '$4 ~ /^switch_profile_start/ && !found {print $1; found=1}')
echo "watching $(echo "$line" | cut -d' ' -f4-) at $lo..$hi (anchor $anchor)"
ini="$repo/dist/sd/switch/melee-nx-netplay/launcher.ini"
content="$(grep -v '^profile_range=' "$ini" || true)"
printf '%s\nprofile_range=%s:%s:%s\n' "$content" "$lo" "$hi" "$anchor" > "$ini"   # rewrite in place: mv trips on this volume's ownership
curl -sS --connect-timeout 10 -T "$ini" "ftp://$ip:${NX_FTP_PORT:-5000}/switch/melee-nx-netplay/launcher.ini" && echo "launcher.ini pushed"
