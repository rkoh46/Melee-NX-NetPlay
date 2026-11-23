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
`--game PATH` (load another image instead of the embedded one). Rollback: `--rollback-test EVERY:DEPTH`, `--rollback-verify`, `--rollback-draw`, `--track-remap` (see below). Clock and profile: `--no-overclock`, `--cpu-mhz N`, `--core N`, `--profile`. Diagnosis: `--rollback-diff`, `--reference FILE.csv`, `--poll-ticks N` (changes the digest). Embedded scripts: `parity_vs_onett`
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

## Rollback snapshot (hot pages)

The state a rollback saves is 44.8 MiB (all of main memory plus the image's writable data); one full copy takes
14 ms on the console. Measured on Linux (`--dirty-dump`, `tools/dirty_stats.py`): a VS match frame changes 128
(2 players) to 209 (4 players) of its 11,499 pages in the median, 171 to 263 at most, and a whole match touches
250 to 445 pages (1.0 to 1.7 MiB). After the first second 82 to 93% of frames write no page that the previous 60
frames did not, but new pages keep appearing (114 to 220 over a match), so a fixed list would not be exact.

Design (`runner/rollback.*`, `runner/dirtytrack*`): every page of the state starts read-only. The first write to a
page faults; the handler keeps the page's bytes as they were and puts the page on the hot list, and the page stays
writable from then on. A save copies the hot pages; a restore copies them back (from the slot, or from the kept
bytes for pages that became hot after that save). Before a disc read the host makes the destination writable,
because the kernel fills that buffer and nothing would fault.

`--rollback-test EVERY:DEPTH` saves every VS match frame and, every EVERY frames, restores the state of DEPTH
frames earlier (and the console clock with it) and lets the game simulate those frames again. Every state, first
pass or repeated, is compared with the x86-64 reference state of the same match frame. `--rollback-verify`
also proves that no write escaped the tracking (it compares the whole state: slow, use for exactness only).
On Linux: IDENTICAL for `parity_vs_onett`, `vs4_busy`, `parity_vs_items_off` and `parity_lcancel`, with 234 to 574
rollbacks each and no escaped writes.

Repeated frames are not drawn by default, as in a real rollback (the game's loop asks the host whether to skip the
draw; the clock rewind keeps audio from being mixed). `--rollback-draw` draws them. Measured on Linux, a repeated
frame without drawing costs 13% (2 players) to 20% (4 players) of a first-pass frame; with drawing but no audio,
41 to 53%. BUT exactness is only proven with `--rollback-draw`: with undrawn repeated frames the 2-player runs
differ from the reference in 1 to 20 states and re-align, and `vs4_busy` diverges for good at match frame 517.
The draw section itself does not use the RNG (instrumented: 0 frames), so the cause is indirect and not found.
The engine's own game-driven rollback has a byte-for-byte self-test (`MELEE_ONLINE_SELFTEST=every:depth`) that
is the right judge of undrawn re-simulation; this outside-in test is not.

On the console the runner raises the CPU clock to 1785 MHz while it runs (`--no-overclock`, `--cpu-mhz N`), using
the app's `switch/src/switch_clocks.cpp`, prints the reported and measured MHz at the start and the end, and puts
the CPU clock back.

Where a frame goes (`--profile`): every entry of the two interface tables between the game and the host gets a
timing wrapper (`runner/hostprof.*`), and the run ends with a table per kind of frame: time inside the game
image, and calls and time per host service. On Linux a first-pass frame is about half game image and 35 to 46%
`dsp_mail` (the host's audio mixer); the game waits for the next frame with about 330 host polls, which cost 2 to
3%. Each run also times fixed integer, floating-point and memory work at the start and the end (`calibration`
lines), pins the simulation thread to one core on the console (`--core N`) and prints the measured MHz every 600
retraces, because the reported clock is not what the core necessarily runs at (sys-clk overrides requests).

Undrawn repeated frames, continued: the cause is not host polls (the same 329.6 per frame drawn or undrawn). The
render pass leaves state that gameplay reads on the next frame; the engine's own rollback makes up for it with
`camera_tasks()` (camera values and fighter bone matrices). Patches `0003` and `decomp-0002` make an undrawn
frame call it. With that, `parity_vs_items_off` and `parity_lcancel` are IDENTICAL undrawn; the runs with items
(`parity_vs_onett`, `vs4_busy`, four players on the other stage) still diverge, which points at item state the
render pass also refreshes and the engine's rollback does not. `--rollback-diff` lists the differing state bytes.

Caveats found on the way:
- The simulation depends on the console clock: restoring the state without the clock diverges (items on).
- This host lets the game stream its music from the disc; the test does not roll back across such a read. The
  full melee-unlocked host plays music itself and has no disc reads in a match.
- The test restores from outside, at the end of a retrace. The real engine's rollback is driven by the game
  inside one frame (`resim` in the host interface); wiring that up is not part of this experiment.

Horizon specifics, from the kernel source (Atmosphere's mesosphere) and the first probe run:
- `svcSetProcessMemoryPermission` cannot make these pages read-only once they have been writable (it only
  accepts pages in a code state; rc 0xd401 on the console). `svcSetMemoryPermission` accepts any reprotectable
  state, which they are in, both ways: that is what `dirtytrack_horizon.c` uses. `--track-remap` selects the
  alternative (unmap, map again, then the process-permission call).
- Retrying the faulting instruction takes two trips through the kernel, because `svcReturnFromException` does
  not preserve x12-x17 if an interrupt arrives while the handler runs (`runner/dirtytrack_horizon.S` explains).
  The runner's exception entry replaces libnx's.

`faultprobe/` checks all of this on the console in one run: which protection method works and what it costs,
how the kernel's memory blocks split and merge, the fault round trip, that every register survives 200,000
retries, and what a file read into a read-only page does.

## The engine's own online path (`--net`)

melee-unlocked's game has its own Slippi online code (`sourceport/game/shim/mu_online.c`): it sends the host the
same command bytes and payloads a Slippi game sends its EXI device (`slippi_command`), saves and loads state by
frame (0xB1, 0xB2), and drives re-simulation itself inside one frame. In `--net` mode the runner answers those
commands with:
- the hot-page store for 0xB1/0xB2 (and 0xF0/0xF1, the engine's rollback self-test, `--selftest EVERY:DEPTH`);
- **this repository's Slippi 3.7.0 netplay** (`port/runtime/hle/slippi_online.cpp`, `slippi_net.cpp`, ENet) for
  everything else, through `runner/netplay_bridge.*`. Matchmaking, login, reporting, HTTP and Discord are
  stubbed: two instances peer directly ("local peer"), nothing leaves the machine, and the user file is made up.

`tools/net_test.sh FRAMES DELAY_MS JITTER_MS [runner arguments]` runs two headless instances in one container
with no network, optionally through `tools/udp_delay_relay.py`, and compares the states both sides showed that
were never replaced by a rollback (`tools/compare_net_digests.py`). `MU_DISC_DIR` names the folder with the disc.
`--net-store engine` serves the savestates with melee-unlocked's own snapshot engine instead, for comparison.

Measured on Linux: with 60 ms delay and 40 ms jitter a 3,600-retrace match has about 580 rollbacks per side,
2 to 6 frames deep, and every confirmed state is identical on both sides. The engine's self-test is NOT clean:
every comparison finds 60 to 140 differing byte ranges (joint matrices and flags of non-fighter objects, the
camera, particle display and video state, and now and then one byte at fighter+0x6D4) with our store and with
the engine's own alike, so it is a property of the engine's undrawn re-simulation, not of the store.

The snapshot now also leaves out the sound driver's state listed in `melee_game.snapexcl`, as the engine's host does.

## Licensing

Our files are GPL-3.0-or-later. melee-unlocked's host code is GPL-2.0-or-later and is compiled in from the
fetched tree, never copied here; the patches are diffs against it. The built NRO contains the compiled game
and must not be distributed.

## Rollback on the console (2026-11-22)

Write tracking by remapping (`--track-remap`): the plain `svcSetMemoryPermission` path partly failed in the
probe, `svcSetProcessMemoryPermission` is one-way (writable pages cannot be made read-only again), and
unmapping and mapping the range again puts it back in a state that can be protected. A write fault costs
3.2 to 3.9 µs including the retry, and all registers survive it (204,800 faulting stores checked).

| `--rollback-test 30:7 --track-remap` | `parity_vs_onett` (2 players) | `vs4_busy` (4 players) |
|---|---|---|
| Every state, first pass and repeated, against the x86-64 reference | identical (8838) | identical (8838) |
| Hot pages at the end | 596 (2.33 MiB) | 898 (3.51 MiB) |
| Save, every frame | 0.34 ms avg, 0.53 worst | 0.47 ms avg, 1.27 worst |
| Restore | 0.25 ms avg | 0.42 ms avg |
| Whole rollback frame (restore + 7 repeated + 1 new) | 19.9 ms avg, 22.0 worst | 34.4 ms avg, 40.4 worst |

The whole rollback frame is still over 16.67 ms. Two things are not yet in this number: the runner does not
raise the CPU clock the way the app does, and repeated frames still build the GX stream and mix audio.

## Clock and picture on the console (2026-11-22, later)

- sys-clk is installed on the test console and sets the CPU to about 2091 MHz a moment after any program
  starts, whatever is requested. Every console number in this file was therefore measured at about
  2091 MHz, not at the stock 1020 MHz. The runner now prints the measured rate.
- A 2-player frame without a picture (3.31 ms): 1.82 ms inside the game image, 1.18 ms in the host's audio
  mixer, 0.22 ms in the game's wait-for-next-frame polling (330 polls).
- With a picture (`frontend/`, second run, pipeline cache warm), 2-player Onett, 7200 match frames:
  simulation thread 3.65 ms (p99 5.7, none over 16.67 ms), decoder thread 2.96 ms, render thread 4.28 ms;
  8987 of 8998 frames drawn. `--warm-all` took 357 s before the match, which is not usable as a default.
