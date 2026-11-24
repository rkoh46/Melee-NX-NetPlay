#!/usr/bin/env bash
# The netplay object (../build.sh net_build) at this program's link. Runs inside the build containers.
#   netplay_link.sh weaken TOOLPREFIX netplay.o netplay-weak.o
#       Makes the object's DEFINED global symbols weak, so the inline variables and template instances it shares with
#       this program (standard library, the app's headers: plain definitions after its relocatable link) join ours
#       instead of clashing. Its UNDEFINED symbols stay strong: `objcopy --weaken` would make them weak too, and a weak
#       reference does not pull a member out of an archive, so on the console curl_global_init, filesystem::remove,
#       select, shutdown and other functions only the netplay object uses were never linked and resolved to address 0
#       (the services NRO died on its first HTTPS call, executing a zero word after a branch the linker had dropped).
#   netplay_link.sh check TOOLPREFIX netplay.o program.elf
#       Fails when the weakened copy has weak references the object did not have, or (services object) when the
#       linked program does not define libcurl's entry points.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
mode=$1 prefix=$2 object=$3 target=$4
nm="${prefix}nm"; objcopy="${prefix}objcopy"
case "$mode" in
  weaken)
    list="$target.symbols"
    "$nm" -g --defined-only "$object" | awk 'NF >= 3 { print $3 }' | sort -u > "$list"
    "$objcopy" --weaken-symbols="$list" "$object" "$target"
    rm -f "$list"
    ;;
  check)
    # 1. The weakened copy must not have turned any reference into a weak one (weak references pull nothing in).
    weak="$(dirname "$object")/netplay-weak.o"
    turned=$(comm -13 <("$nm" "$object" | awk '$1 == "w" || $1 == "v" { print $2 }' | sort -u) <("$nm" "$weak" | awk 'NF == 2 && ($1 == "w" || $1 == "v") { print $2 }' | sort -u) || true)
    if [ -n "$turned" ]; then echo "netplay_link: $(basename "$weak") has weak references the object did not have: $(echo $turned)" >&2; exit 1; fi
    # 2. Then the linker has resolved every reference that kept code makes (an unresolved strong one fails the link);
    #    the object's other references sit in sections --gc-sections dropped. Spot-check the services' entry points.
    # (Lists are read whole first: grep -q on a pipe would end nm early, and pipefail would call that a failure.)
    needs=$("$nm" -u "$object"); have=$("$nm" --defined-only "$target")
    curl=""
    if grep -qw curl_global_init <<<"$needs"; then
      for f in curl_global_init curl_easy_init curl_easy_perform curl_easy_setopt; do
        grep -qw "$f" <<<"$have" || { echo "netplay_link: $(basename "$target") does not define $f" >&2; exit 1; }
      done
      curl="; libcurl's entry points are linked"
    fi
    echo "netplay_link: $(basename "$target"): no reference of the netplay object was made weak$curl"
    ;;
  check-dynamic)
    # Linux: shared libraries supply what is missing at run time; a strong reference that nothing supplies fails the
    # link already, so only references that are still weak and unresolved are a problem. The input-suggestion hooks
    # (../patches/net-0001: mu_net_suggest_read, mu_net_suggest_setting) are optional on purpose: a program without
    # controllers or feature modules leaves them out, and the netplay object then sends no suggestions.
    missing=$(comm -12 <("$nm" -u "$object" | awk '{ print $NF }' | sort -u) <("$nm" "$target" | awk '$1 == "w" || $1 == "v" { print $2 }' | sort -u) | grep -Ev '^(__gmon_start__|_ITM_.*|__cxa_finalize|mu_net_suggest_.*)$' || true)
    if [ -n "$missing" ]; then echo "netplay_link: $(basename "$target") has weak unresolved references of the netplay object: $(echo $missing)" >&2; exit 1; fi
    echo "netplay_link: no weak unresolved reference of the netplay object in $(basename "$target")"
    ;;
  *) echo "usage: netplay_link.sh weaken|check|check-dynamic TOOLPREFIX OBJECT TARGET" >&2; exit 2 ;;
esac
