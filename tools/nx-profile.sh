#!/usr/bin/env bash
# Name the hot functions in a Logs/profile.txt written by the Switch build (profile=1).
#   tools/nx-profile.sh <switch-ip|profile.txt> [aurora|headless] [top]
# With an IP it fetches switch/melee-nx-netplay/Logs/profile.txt over FTP first. Samples are
# aggregated per function (not per address) for each sampled thread: "pc" is the guest thread,
# "pc1" the render thread when the build sampled both. addr2line runs in the toolchain image.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/.." && pwd -P)"
src="${1:?usage: tools/nx-profile.sh <switch-ip|profile.txt> [aurora|headless] [top]}"
frontend="${2:-aurora}"; top="${3:-30}"
# The ELF of the run that wrote the profile (nx-run.sh archives one per run); NX_ELF overrides.
elf="${NX_ELF:-$repo/build/nxlink-logs/current.elf}"
[[ -f "$elf" ]] || elf="$repo/build/switch-$frontend/port/melee-nx-netplay.elf"
[[ -f "$elf" ]] || { echo "no $elf" >&2; exit 66; }
relelf="${elf#$repo/}"
if [[ -f "$src" ]]; then profile="$src"; else
  profile="$repo/build/switch-pull/profile-$(date +%Y%m%d-%H%M%S).txt"; mkdir -p "$(dirname "$profile")"
  curl -sS --connect-timeout 10 -o "$profile" "ftp://$src:${NX_FTP_PORT:-5000}/switch/melee-nx-netplay/Logs/profile.txt"
fi
profile="$(cd "$(dirname "$profile")" && pwd -P)/$(basename "$profile")"   # docker mounts need absolute paths
head -1 "$profile"
anchor_rt=$(head -1 "$profile" | sed -E 's/.*anchor_runtime=([0-9a-f]+).*/\1/')
image="${MELEE_DOCKER_IMAGE:-melee-nx-netplay-build:latest}"
tc=/opt/devkitpro/devkitA64/bin/aarch64-none-elf
anchor_elf=$(docker run --rm -v "$repo:/project" "$image" $tc-nm "/project/$relelf" </dev/null 2>/dev/null | awk '$3=="_Z20switch_profile_startP6Threadj" || $3=="_Z20switch_profile_startP6ThreadjS0_" || $3=="switch_profile_start" {print $1}' | head -1)
[[ -n "$anchor_elf" ]] || { echo "anchor symbol not in ELF" >&2; exit 65; }
slide=$(( 0x$anchor_rt - 0x$anchor_elf ))
work="$(dirname "$profile")/$(basename "$profile" .txt).work"; mkdir -p "$work"
for tag in pc pc1 svclr svclr1 svcup svcup1 svcup2 svcup21 rangelr rangelr1; do
  awk -v t="$tag" '$1==t' "$profile" > "$work/$tag.rows"
  [[ -s "$work/$tag.rows" ]] || continue
  total=$(awk '{s+=$3} END {print s}' "$work/$tag.rows")
  while read -r _ addr count; do printf '%#x\n' $(( 0x$addr - slide )); done < "$work/$tag.rows" > "$work/$tag.addrs"
  docker run --rm -v "$repo:/project" -v "$work:/w" "$image" sh -c "$tc-addr2line -f -C -e /project/$relelf < /w/$tag.addrs" </dev/null 2>/dev/null |
    paste - - | cut -f1 | sed -E 's/\(anonymous namespace\)/{anon}/g; s/\(.*//' > "$work/$tag.names"
  label="guest thread"; [[ "$tag" == "pc1" ]] && label="render thread"
  [[ "$tag" == "svclr" ]] && label="guest thread: callers of syscalls (lr at the svc)"
  [[ "$tag" == "svclr1" ]] && label="render thread: callers of syscalls (lr at the svc)"
  [[ "$tag" == "svcup" ]] && label="guest thread: syscalls, one frame record up (who called the wrapper)"
  [[ "$tag" == "svcup1" ]] && label="render thread: syscalls, one frame record up"
  [[ "$tag" == "svcup2" ]] && label="guest thread: syscalls, two frame records up"
  [[ "$tag" == "svcup21" ]] && label="render thread: syscalls, two frame records up"
  [[ "$tag" == "rangelr" ]] && label="guest thread: callers of the watched range (profile_range)"
  [[ "$tag" == "rangelr1" ]] && label="render thread: callers of the watched range (profile_range)"
  echo; echo "== $label: $total samples in the table, by function"
  paste <(awk '{print $3}' "$work/$tag.rows") "$work/$tag.names" |
    awk -F'\t' -v tot="$total" '{a[$2]+=$1} END {for (k in a) printf "%5.1f%%  %s\n", 100*a[k]/tot, k}' | sort -rn | head -n "$top"
done
