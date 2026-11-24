#!/usr/bin/env bash
# Slippi spectator server test without a console or a network: one play-mode instance (picture on a screenless X
# server, --auto-vs into a VS match) with `slippi_spectator 1`, and ../tools/spectator_client.cpp connected to it over
# loopback in the same container. The client writes each game's events; ../tools/spectator_check.py checks that they
# parse as Slippi events and are the recorded replay's raw element byte for byte.
#   ./test-spectator-linux.sh /path/to/disc-folder [retraces] [extra arguments]
#   MU_SPECTATOR_LATE=S   the client connects S seconds after the game starts (catching up from cursor 0)
# Output: build/native-engine-gfx/run-spectator/ (console.log, client.log, game-N.bin, Replays/).
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd "$here/../../.." && pwd -P)"
disc="${1:?folder with GALE01.iso}"; frames="${2:-2400}"; shift $(( $# > 2 ? 2 : $# ))
work="$repo/build/native-engine-gfx"; run="$work/run-spectator"
rm -rf "$run"; mkdir -p "$run"
printf 'slippi_spectator 1\nslippi_spectator_port 51441\n' > "$run/settings.ini"
timeout "${MU_TEST_LIMIT:-600}" docker run --rm --network none -v "$disc:/iso:ro" -v "$work/out:/b:ro" -v "$run:/out" -v "$repo:/w:ro" \
  -e FRAMES="$frames" -e EXTRA="$*" -e LATE="${MU_SPECTATOR_LATE:-0}" -e SDL_AUDIODRIVER=dummy "${MU_GFX_IMAGE:-mu-arm-gfx}" bash -c '
    set -e
    e=/w/port/third_party/enet
    gcc -O2 -w -DHAS_FCNTL=1 -DHAS_POLL=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -DHAS_SOCKLEN_T=1 -DHAS_MSGHDR_FLAGS=1 -include arpa/inet.h -I$e/include \
      -c $e/callbacks.c $e/compress.c $e/host.c $e/list.c $e/packet.c $e/peer.c $e/protocol.c $e/unix.c
    g++ -std=gnu++17 -O2 -w -I$e/include -I/w/port/third_party /w/experiments/native-engine/tools/spectator_client.cpp *.o -o /tmp/spectator_client
    rm -f *.o
    Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/out/xvfb.log 2>&1 &
    export DISPLAY=:99 XDG_RUNTIME_DIR=/tmp
    for i in $(seq 100); do xwd -root -silent >/dev/null 2>&1 && break; sleep 0.1; done
    /b/mu-native-gfx-linux --play --auto-vs --iso /iso/GALE01.iso --frames $FRAMES --novsync --window 640x480 --volume 0 --out /out $EXTRA >/out/console.log 2>&1 & game=$!
    sleep "$LATE"
    /tmp/spectator_client 127.0.0.1 51441 /out $(( FRAMES / 60 + 60 )) >/out/client.log 2>&1 & client=$!
    wait $game; rg=$?; wait $client; rc=$?
    echo "exit codes: game $rg, client $rc"'
cat "$run/client.log"
grep -E "^spectator|replay: (recording|cannot)" "$run/console.log" | cut -c1-200
slp="$(ls -t "$run"/Replays/*.slp 2>/dev/null | head -1)"
for g in "$run"/game-*.bin; do
  [ -f "$g" ] || { echo "no game received"; exit 1; }
  echo "== $(basename "$g") against $(basename "$slp")"
  python3 -I "$here/../tools/spectator_check.py" "$g" "$slp"
done
