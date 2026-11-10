#!/usr/bin/env bash
# Native decomp C via guest-memory lowering (docs/NATIVE_C.md). Runs inside the toolchain image:
#   bash builder/docker.sh shell tools/gml/build.sh <out_dir> <decomp file.c>...
# Writes <out_dir>/gml_native.o and gml_native.ll (the lowered, optimised module) for the build.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; repo="$(cd "$here/../.." && pwd)"
out="$1"; shift; mkdir -p "$out"
decomp="$repo/ref/melee"
plugin="$out/GuestMemLower.so"
# Rebuilt when the source changes (by content: the container's clock may differ from the host's).
sum="$(sha1sum "$here/GuestMemLower.cpp" | cut -c1-16)"
if [[ ! -f "$plugin" || "$(cat "$plugin.sum" 2>/dev/null)" != "$sum" ]]; then
  clang++-19 -shared -fPIC -O2 $(llvm-config-19 --cxxflags) "$here/GuestMemLower.cpp" -o "$plugin"
  echo "$sum" > "$plugin.sum"
fi
awk '/= \.[a-z]+:0x/ { n=$1; sub(/.*:0x/, "", $3); sub(/;.*/, "", $3); print n, $3 }' "$decomp/config/GALE01/symbols.txt" > "$out/symbols.map"
ppc=(-target powerpc-unknown-eabi -O2 -ffp-contract=on -fno-builtin -fno-strict-aliasing -fno-jump-tables -std=c99 -w -nostdinc
     -I"$decomp/src" -I"$decomp/include" -I"$decomp/extern/dolphin/include" -I"$decomp/src/MSL" -I"$decomp/include/libc"
     -I"$decomp/extern/dolphin/include/libc" -I"$decomp/src/sysdolphin")
# Each file is optimised and lowered on its own, so its statics resolve inside its own address ranges
# (splits.txt); calls between the files stay direct native calls through the list of what is native.
for f in "$@"; do
  b="$out/$(basename "${f%.c}").ppc.bc"
  clang-19 "${ppc[@]}" -Xclang -disable-llvm-passes -c -emit-llvm "$decomp/$f" -o "$b"   # optimised after gml-externalize
done
: > "$out/natives.txt"
for f in "$@"; do llvm-nm-19 --defined-only "$out/$(basename "${f%.c}").ppc.bc" | awk '$2=="T" {print $3}' >> "$out/natives.txt"; done
lower() {   # $1 = file, $2 = output
  local n="$out/$(basename "${1%.c}")"
  opt-19 -load-pass-plugin="$plugin" -passes=gml-lower -gml-symbols="$out/symbols.map" -gml-dol="$repo/build/disc/main.dol" \
    -gml-splits="$decomp/config/GALE01/splits.txt" -gml-tu="${1#src/}" -gml-native="$out/natives.txt" \
    -gml-report-guest="$out/guest.txt" "$n.opt.bc" -o "$2"
}
for f in "$@"; do
  n="$out/$(basename "${f%.c}")"
  opt-19 -load-pass-plugin="$plugin" -passes='gml-externalize,default<O2>' "$n.ppc.bc" -o "$n.opt.bc"
done
# First pass: which functions stay translated (they must not be called directly from other files).
: > "$out/guest.txt"
for f in "$@"; do lower "$f" /dev/null 2>/dev/null; done
if [[ -s "$out/guest.txt" ]]; then grep -vxF -f "$out/guest.txt" "$out/natives.txt" > "$out/natives.tmp"; mv "$out/natives.tmp" "$out/natives.txt"; fi
: > "$out/guest.txt"
lows=()
for f in "$@"; do
  n="$out/$(basename "${f%.c}")"
  lower "$f" "$n.low.bc"
  lows+=("$n.low.bc")
done
llvm-link-19 "${lows[@]}" -o "$out/all.low.bc"
clang-19 -target aarch64-unknown-none-elf -mcpu=cortex-a57 -O2 -c -emit-llvm "$here/gml_rt.c" -o "$out/gml_rt.bc"
llvm-link-19 "$out/all.low.bc" "$out/gml_rt.bc" -o "$out/linked.bc"
opt-19 -O2 "$out/linked.bc" -o "$out/opt.bc"
llvm-dis-19 "$out/opt.bc" -o "$out/gml_native.ll"
llc-19 -O2 -mtriple=aarch64-unknown-none-elf -mcpu=cortex-a57 -relocation-model=pic -filetype=obj "$out/opt.bc" -o "$out/gml_native.o"
echo "gml: $(grep -c '^define' "$out/gml_native.ll") functions -> $out/gml_native.o"
