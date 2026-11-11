#!/usr/bin/env bash
# Melee-NX-NetPlay installer: builds the Switch homebrew from YOUR Melee disc.
#
#   curl -fsSL https://raw.githubusercontent.com/rkoh46/Melee-NX-NetPlay/main/install.sh | bash -s -- /path/to/GALE01.iso
#
# What happens, in order (each step says so as it runs):
#   1. checks for Git, Docker and Python 3.11+
#   2. clones the repository into ~/Melee-NX-NetPlay (or updates it)
#   3. downloads the prebuilt SDK (Switch GPU driver, Dawn, SDL3, Aurora: no game content, ~55 MB)
#   4. builds the compiler image (devkitA64 + Clang; the one slow download, ~5 minutes)
#   5. translates your disc's main.dol into C++ (seconds) and compiles it (10-20 minutes)
#   6. leaves a folder ready for your SD card and tells you exactly where to put it
# Your disc stays where it is; nothing from it is uploaded anywhere.
set -euo pipefail

REPO_URL="https://github.com/rkoh46/Melee-NX-NetPlay"
RAW_URL="https://raw.githubusercontent.com/rkoh46/Melee-NX-NetPlay/main"
SDK_RELEASE="sdk-1"
SDK_FILE="melee-nx-netplay-sdk-1.tar.gz"
DEST="${MELEE_NX_HOME:-$HOME/Melee-NX-NetPlay}"
JOBS="${MELEE_BUILD_JOBS:-$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) )}"

bold=$'\033[1m'; dim=$'\033[2m'; yellow=$'\033[33m'; green=$'\033[32m'; red=$'\033[31m'; reset=$'\033[0m'
step() { printf '\n%s%s▶ %s%s\n' "$bold" "$yellow" "$*" "$reset"; }
note() { printf '  %s%s%s\n' "$dim" "$*" "$reset"; }
ok()   { printf '  %s✓ %s%s\n' "$green" "$*" "$reset"; }
fail() { printf '\n%s✗ %s%s\n' "$red" "$*" "$reset" >&2; exit 1; }

# The whole script sits in main() so `curl | bash` parses it completely before any
# command (docker run -i in particular) can read from the same stdin.
main() {
  iso="${1:-}"
  if [[ -z "$iso" ]]; then
    printf '%sMelee-NX-NetPlay%s builds Super Smash Bros. Melee with Slippi online play for a Nintendo Switch\n' "$bold" "$reset"
    printf 'running Atmosphère, from your own disc image. Usage:\n\n'
    printf '  bash install.sh /path/to/GALE01.iso\n\n'
    printf 'The disc must be NTSC-U 1.02 (GALE01). Drag the file into the terminal to get its path.\n'
    exit 64
  fi
  [[ -f "$iso" ]] || fail "No file at $iso"
  iso="$(cd "$(dirname "$iso")" && pwd)/$(basename "$iso")"

  step "1/6  Checking the tools on this computer"
  for tool in git docker python3; do
    command -v "$tool" >/dev/null 2>&1 || fail "$tool is not installed. Git, Docker and Python 3.11+ are needed (Docker Desktop, Colima or OrbStack all work)."
    ok "$tool"
  done
  python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)' || fail "Python 3.11 or newer is needed (found $(python3 --version))."
  docker info >/dev/null 2>&1 || fail "Docker is installed but not running. Start Docker Desktop (or colima start) and run this again."
  ok "Docker is running"
  if docker info 2>/dev/null | grep -qi colima; then
    note "Colima shares only the folders listed in ~/.colima/default/colima.yaml with Docker."
    note "If the build cannot see its files, add your home folder there or set MELEE_NX_HOME to a shared folder."
  fi

  step "2/6  Getting the source into $DEST"
  if [[ -d "$DEST/.git" ]]; then
    git -C "$DEST" pull --ff-only --quiet </dev/null && ok "updated"
  else
    git clone --quiet "$REPO_URL" "$DEST" </dev/null && ok "cloned"
  fi
  cd "$DEST"
  bash builder/fetch-deps.sh melee </dev/null >/dev/null
  ok "pinned doldecomp/melee (symbol names only; no game code comes from it)"

  step "3/6  Downloading the prebuilt SDK (GPU driver, Dawn, SDL3, Aurora: no game content)"
  mkdir -p build/sdk
  if [[ -f build/sdk/SDK_MANIFEST.json ]] && grep -q "\"version\": \"${SDK_RELEASE#sdk-}\"" build/sdk/SDK_MANIFEST.json; then
    ok "already present"
  else
    curl -fL --progress-bar -o "build/$SDK_FILE" "$REPO_URL/releases/download/$SDK_RELEASE/$SDK_FILE"
    curl -fsSL -o "build/$SDK_FILE.sha256" "$REPO_URL/releases/download/$SDK_RELEASE/$SDK_FILE.sha256"
    (cd build && shasum -a 256 -c "$SDK_FILE.sha256" >/dev/null) || fail "The SDK download did not match its checksum; run the installer again."
    rm -rf build/sdk && mkdir -p build/sdk && tar -xzf "build/$SDK_FILE" -C build/sdk
    ok "unpacked to build/sdk"
  fi

  step "4/6  Building the compiler image (devkitA64 + Clang 19; first time only, about 5 minutes)"
  if docker image inspect melee-nx-netplay-build:latest >/dev/null 2>&1; then ok "already built"; else
    docker build -q -t melee-nx-netplay-build:latest switch/docker </dev/null >/dev/null && ok "built"
  fi

  step "5/6  Translating your disc and compiling the game (the long step: 10-20 minutes)"
  note "disc: $iso"
  bash builder/generate-game.sh "$iso" </dev/null 2>&1 | grep -E "^generated |^Generated |rror:|usage:" | sed 's/^/  /'
  ok "translated into C++ in build/game (private to this folder)"
  note "compiling with $JOBS jobs; a progress counter follows"
  MELEE_NX_FRONTEND=aurora MELEE_BUILD_JOBS="$JOBS" bash builder/docker.sh game all </dev/null 2>&1 |
    grep --line-buffered -E '^\[[0-9]+/[0-9]+\]|error:|FAILED|done:' |
    awk '/^\[[0-9]+\/[0-9]+\]/ { split($1, n, /[\[\/\]]/); if (n[2] % 10 == 0 || n[2] == n[3]) print "  " n[2] " of " n[3]; next } { print "  " $0; fflush() }'
  nro="build/switch-aurora/port/melee-nx-netplay.nro"
  [[ -f "$nro" ]] || fail "The build did not produce $nro. The full log is above; open an issue at $REPO_URL/issues with it."
  ok "built $(du -h "$nro" | cut -f1) NRO"

  step "6/6  Your SD card folder"
  out="dist/sd/switch/melee-nx-netplay"
  mkdir -p "$out"
  cp "$nro" "$out/melee-nx-netplay.nro"
  cat > "$out/launcher.ini" <<'INI'
# Melee-NX-NetPlay settings. The in-game menu (hold L+R+Start half a second) changes volume,
# HUD and online delay and saves them here.
# online_delay: auto (2..5 frames from the opponent's ping) or a fixed 1..9 (Slippi's default 2). offline=1 skips the Slippi sign-in.
# overclock=1 asks for the Erista's top clocks (CPU 1785, GPU 921, memory 1600 MHz); 0 leaves the system's.
online_delay=auto
offline=0
volume=100
hud=0
overclock=1
INI
  ok "$DEST/$out"
  printf '\n%sDone.%s Copy the folder %smelee-nx-netplay%s to your SD card as:\n\n' "$bold$green" "$reset" "$bold" "$reset"
  printf '    sdmc:/switch/melee-nx-netplay/\n\n'
  printf 'then copy your disc into that same folder, named exactly %sGALE01.iso%s.\n\n' "$bold" "$reset"
  printf 'On the Switch (Atmosphère): open the Homebrew Menu %sby holding R while launching any game%s\n' "$bold" "$reset"
  printf '(that gives the homebrew the full memory; the Album shortcut does not), then start Melee-NX-NetPlay.\n'
  printf 'The first screen asks for your Slippi account; B plays offline. Hold L+R+Start in game for the menu.\n'
  printf 'Logs are written to switch/melee-nx-netplay/Logs/latest.log if anything goes wrong.\n\n'
  printf '%sOpen the folder:%s  open "%s"   (macOS)   or   xdg-open "%s"   (Linux)\n' "$dim" "$reset" "$DEST/$out" "$DEST/$out"
}
main "$@"
