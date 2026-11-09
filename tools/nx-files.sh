#!/usr/bin/env bash
# Move files to and from the Switch's SD card over FTP, without taking the card out.
# Needs an FTP server on the Switch: sys-ftpd-light (background, port 5000, no login) or
# the ftpd homebrew (port 5000) running in the foreground.
#
#   tools/nx-files.sh <switch-ip> pull            # Logs/, launcher.ini, crash and fatal reports -> build/switch-pull/<time>/
#   tools/nx-files.sh <switch-ip> push [aurora]   # NRO + launcher.ini -> sdmc:/switch/melee-nx-netplay/
#   tools/nx-files.sh <switch-ip> iso <GALE01.iso> # the disc (about 1.4 GB; ten minutes or more over Wi-Fi)
#   tools/nx-files.sh <switch-ip> ls [path]        # list a directory
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/.." && pwd -P)"
ip="${1:?usage: tools/nx-files.sh <switch-ip> pull|push|iso|ls ...}"; cmd="${2:?pull|push|iso|ls}"; shift 2
port="${NX_FTP_PORT:-5000}"
base="ftp://$ip:$port"
app="switch/melee-nx-netplay"
ftp() { curl -sS --connect-timeout 10 --ftp-method nocwd "$@"; }
mkd() { curl -sS --connect-timeout 10 -Q "MKD /$1" "$base/" -o /dev/null 2>/dev/null || true; }  # ftpd answers 553 instead of creating parents

case "$cmd" in
  ls)
    ftp "$base/${1:-}/" ;;
  pull)
    out="$repo/build/switch-pull/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$out"
    get_dir() {  # $1 remote dir, $2 local dir: every plain file one level deep
      mkdir -p "$2"
      ftp "$base/$1/" 2>/dev/null | awk '$1 !~ /^d/ {print $NF}' | while read -r f; do
        [[ -n "$f" ]] && ftp -o "$2/$f" "$base/$1/$f" && echo "  $1/$f"
      done || true
    }
    echo "pulling to $out"
    get_dir "$app/Logs" "$out/Logs"
    ftp -o "$out/launcher.ini" "$base/$app/launcher.ini" 2>/dev/null && echo "  $app/launcher.ini" || true
    get_dir "atmosphere/crash_reports" "$out/crash_reports"
    get_dir "atmosphere/crash_reports/dumps" "$out/crash_reports/dumps"
    get_dir "atmosphere/fatal_errors" "$out/fatal_errors"
    find "$out" -type d -empty -delete
    echo "done: $(find "$out" -type f | wc -l | tr -d ' ') files" ;;
  push)
    frontend="${1:-aurora}"
    nro="$repo/build/switch-$frontend/port/melee-nx-netplay.nro"
    [[ -f "$nro" ]] || { echo "no $nro" >&2; exit 66; }
    ini="$repo/dist/sd/$app/launcher.ini"
    mkd "switch"; mkd "$app"; mkd "$app/Logs"
    ftp --ftp-create-dirs -T "$nro" "$base/$app/melee-nx-netplay.nro" && echo "  $app/melee-nx-netplay.nro ($(du -h "$nro" | cut -f1))"
    [[ -f "$ini" ]] && ftp --ftp-create-dirs -T "$ini" "$base/$app/launcher.ini" && echo "  $app/launcher.ini"
    for s in "$repo"/port/scripts/*.txt; do   # the input scripts launcher.ini can name
      ftp -T "$s" "$base/$app/$(basename "$s")" && echo "  $app/$(basename "$s")"
    done ;;
  iso)
    iso="${1:?path to GALE01.iso}"
    mkd "switch"; mkd "$app"
    ftp --ftp-create-dirs --progress-bar -T "$iso" "$base/$app/GALE01.iso" && echo "  $app/GALE01.iso" ;;
  *) echo "unknown command $cmd" >&2; exit 64 ;;
esac
