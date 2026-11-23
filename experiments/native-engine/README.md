# Native-engine experiment

Can the source-built Melee engine of [melee-unlocked](https://github.com/Hero88go/melee-unlocked) v1.0.5
(commit `67868aa`) run on the Switch, and does it stay bit-identical to the x86-64 build there? This folder
holds only our own files: a loader/runner, build scripts and patches. melee-unlocked, the decompilation and
everything built from them live under `build/native-engine/` (gitignored) and are never committed.

## What is here

| Path | What it is |
|---|---|
| `fetch.sh` | Clones melee-unlocked at the pinned commit into `build/native-engine/mu`, runs its own source preparation in a container, applies `patches/`. |
| `build.sh` | `game`: the game image with GCC 15.2 (AArch64 Linux ELF linked at `0x82800000`). `linux`: the runner for aarch64 Linux (test build). `nro`: the runner for Horizon. |
| `patches/0001-aarch64-build.patch` | Lets the game and the headless host build for AArch64 (compiler flags, CPU check, image checker). |
| `patches/0002-host-foreign-elf-bridge.patch` | Lets the headless host load the image through our loader instead of `dlopen`/`mmap` (it reuses the code paths the project has for its macOS bridge). |
| `patches/decomp-0001-...patch` | Two disc read buffers get an explicit 32-byte alignment (x86-64 GCC aligned them by itself; AArch64 does not, and the game asserts). |
| `runner/fixedmap.h`, `fixedmap_horizon.c`, `fixedmap_linux.c` | Memory at fixed addresses. **The Horizon file is the unverified part** and is kept small on purpose. |
| `runner/mu_elf.cpp`, `mu_elf_bridge.h` | ELF loader: copies the segments and applies relocations in staging pages, then maps them at `0x82800000`. |
| `runner/mu_imports.c` | The 31 C library functions the image imports, bound to newlib (or glibc in the test build). |
| `runner/runner_main.cpp` | Arguments, digest, timing, snapshot timing, logs. |
| `reference/*.csv.gz` | The x86-64 reference state digests (9000 retraces per script), embedded in the runner so it can name the first differing retrace. Measurement output, not game data. |
| `scripts/make_vs4_busy.py` | Generates the four-player script from `parity_vs_onett.txt`. |
| `tools/compare_digests.py`, `tools/digest_hash.py` | Compare two digest CSVs row by row; hash one as the runner does. |
| `addrprobe/` | The earlier address probe. |

## Design

A Homebrew NRO is position independent and loaded at a random address, so the game cannot be the NRO. The
NRO is the host: melee-unlocked's headless Linux host services (disc, memory card files, timers, audio
mixing, scripted pads) compiled with devkitA64, plus our loader. The game image is the **same AArch64 Linux
shared object that was verified on Linux**, embedded in the NRO: same compiler output, so the exactness
result carries over unless Horizon itself behaves differently. It needs no dynamic linker: it is linked at
its run address, imports only standard C functions, and has no thread-local storage. The host passes its
services as a table of function pointers (`mu_game_entry`), as on every other platform.

Three ranges are mapped at fixed addresses with `svcMapProcessCodeMemory` + `svcSetProcessMemoryPermission`,
the way nx-hbloader places an NRO: main memory `0x80000000` (40 MiB, read/write), the image `0x82800000`
(code read/execute, data read/write) and the locked cache `0xE0000000` (16 KiB). They are unmapped on exit.

## Build

```sh
docker build -t mu-arm-gcc15 -f linux-gcc15.Dockerfile .   # once: aarch64 Linux, GCC 15.2
./fetch.sh            # MU_SOURCE=file:///path/to/a/clone works offline
./build.sh            # game image, Linux test runner, NRO
```

Output in `build/native-engine/out/`: `melee_game.so`, `mu-native-linux`, `mu-native.nro`.

## Run

On the console the disc image is `sdmc:/switch/melee-nx-netplay/GALE01.iso` (NTSC-U 1.02). Output goes to
stdout (nxlink) and is appended to `sdmc:/switch/mu-native/run.log`; the digest is written to
`sdmc:/switch/mu-native/digest-<script>.csv`.

```sh
nxlink -s -a <console ip> build/native-engine/out/mu-native.nro --args "--script parity_vs_onett --frames 9000"
```

Arguments (all optional): `--iso PATH`, `--script NAME|PATH`, `--frames N`, `--out DIR`,
`--snapshot-at VS_MATCH_FRAME` (times a full save and restore at that frame; off by default; it does not change the digest), `--print-digest` (also stream the CSV rows),
`--game PATH` (load another image instead of the embedded one). Embedded scripts: `parity_vs_onett`
(2 players, Onett, items, 7200-frame match), `vs4_busy` (4 players, random inputs), `parity_vs_items_off`,
`parity_lcancel`, the other `parity_*` scripts, and `native_vs_ko` (never reaches a match on v1.0.5).

Linux test build (same sources except the mapping back end):

```sh
docker run --rm --network none -v /path/to/disc-dir:/iso:ro -v "$PWD/../../build/native-engine/out":/b:ro -v /tmp/out:/out \
  mu-arm-gcc15 /b/mu-native-linux --iso /iso/GALE01.iso --out /out --script vs4_busy
```

What a good run prints: `scenes: same sequence and timing as the reference`, `digest vs x86-64 AVX2
reference: IDENTICAL`, the VS match frame times and, with `--snapshot-at`, `snapshot save/restore ms`. When
the digest differs it prints the first differing retrace with both rows and both scene timelines. The memory card folder (`<out>/card`) is emptied at every start, because the
scripts assume a first boot.

## Verified

- On aarch64 Linux (Apple M5 Ultra, container), with the GCC 15.2 image and this runner: all five scripts
  give digests identical to the x86-64 AVX2+FMA references (state digest: RNG and each fighter's action,
  animation frame, position, velocity, percent, stocks, facing, for 9000 retraces).
- The NRO compiles and links with devkitA64 GCC 15.2 and libnx.

## Verified on the console (2026-11-22)

Homebrew Menu opened through a game title (applet type 0, 39-bit address space), no picture, offline.

| Check | Result |
|---|---|
| Fixed mappings at `0x80000000`, `0x82800000`, `0xE0000000` | free and usable on four launches with different layouts; code ran at `0x82800000` |
| `parity_vs_onett` (2 players, 7200 match frames) | digest identical to the x86-64 reference for all 9000 retraces; 3.19 ms per match frame, p99 4.48, worst 8.01 |
| `vs4_busy` (4 players, 7200 match frames) | identical for all 9000 retraces; 4.97 ms per match frame, p99 6.43, worst 12.53 |
| Match frames over 16.67 ms | 0 in both |
| Full snapshot, 44.8 MiB (`--snapshot-at 600`) | save about 14.2 ms, restore about 9.7 ms; state unchanged afterwards |
| FPCR, 8 MiB thread stack, memory card write, exit to the Homebrew Menu | as expected |

Each frame time is the whole retrace on one core: simulation, the GX command stream (discarded) and audio
mixing (discarded). The CPU clock was not checked (1020 MHz unless something overclocks).

The very first run's save failed (`mkstemp`/`fsync`/`rename`), so the menus never reached the VS match; its
digest equalled a Linux run with the save forced to fail. The save is written with stdio since (patch 0002).

## Not verified

- Other process layouts: the ranges may not be free, or may lie in this process's heap/alias region (the
  layout is random per process: restart the host title; the error line prints the region addresses). A host
  title with a 36-bit address space (ASLR region ends at `0x80000000`) can never work.
- Other stages, characters and modes; anything online; a picture, sound output or real controllers.
- A rollback snapshot that fits a frame: the full copy above does not.

## Licensing

Our files are GPL-3.0-or-later. melee-unlocked's host code is GPL-2.0-or-later and is compiled in from the
fetched tree, never copied here; the patches are diffs against it. The built NRO contains the compiled game
and must not be distributed.
