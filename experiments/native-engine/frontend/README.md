# Native engine with a picture

`mu-native-gfx` is the native-engine runner (see `../README.md`) with its GX command stream connected to the
renderer the app already uses. It runs the same scripted matches, shows them on screen, and still prints the
digest comparison and the timing lines. No sound output, no controllers, no network.

## How it works

The game builds a GameCube GX command stream with its own GX library and hands the bytes to the host
(`MuHostApi::gx_fifo`); everything a command points at (textures, vertex arrays, display lists, palettes) is
referenced by console address in main memory or in the game image. The headless runner counts the bytes and
drops them. Here:

```
simulation thread   game -> gx_fifo -> appended to a chunk queue; waits for the decoder once per frame, at the retrace
decoder thread      GX decoder (port/runtime/gx/gx_core.cpp): parses, copies what the commands reference, builds frames
render thread       newest finished frame -> gx::AuroraBackend (port/runtime/gx/gx_aurora*.cpp) -> Aurora -> Dawn -> Vulkan
```

- The decoder is this repository's, the one the recompiled app feeds from the guest's write-gather pipe, and it runs
  the way the app runs it: on its own worker thread (gx_core's "GPU thread"), with the game waiting for it once per
  frame before its next update changes what the frame's commands point at. `--gfx-sync` decodes inside the game's
  pipe flush instead, as melee-unlocked's own hosts do: exact even where the game rewrites data between two draws
  of one frame, and the decoder's whole cost lands on the simulation thread.
- `patches/0102` adapts the decoder's address handling: main memory is 40 MiB here and the game image, with static
  data GX may point at, is mapped right after it at `0x82800000`. It also removes a progress line the decoder wrote
  to the log every two seconds and adds one timing scope.
- The renderer is `gx::AuroraBackend`, unchanged, on its own thread, which owns the window and Aurora. It
  always draws the newest finished frame; when it is behind, older frames are skipped.
- On the console the threads sit where the app puts them: the game alone on core 2 at priority 0x1C, the render
  thread on core 1 at 0x28, the decoder on core 0 at 0x26, Aurora's workers on cores 0-1, the shader compilers kept
  off core 2 (`AURORA_RESERVED_CORE_MASK`), GPU and memory clocks raised as the app raises them (the runner raises
  the CPU clock). The frontend logs nothing while the game runs.
- melee-unlocked's own desktop renderers (OpenGL 4.5, Direct3D, a Windows Vulkan one) were not ported: the
  Switch stack built by this repository offers Aurora on Dawn/Vulkan and nothing else.
- Nothing here writes game memory. The runner is `../runner/runner_main.cpp`, unchanged, with its `main`
  renamed; the host services call the frontend through one weak hook (`patches/0101`).

| Path | What it is |
|---|---|
| `src/frontend.cpp`, `frontend.h` | FIFO sink, the wait for the decoder, 60 Hz pacing, frame queue and render thread, Aurora start-up, thread placement, per-part timing. |
| `src/frontend_main.cpp` | `main`: takes the frontend's arguments out of the command line, then calls the runner. |
| `src/shim/` | The few host functions the decoder needs, on melee-unlocked's host services. |
| `src/switch_glue.cpp` | Two symbols the app's Switch layer expects. |
| `patches/0101-host-frontend-attach.patch` | Against the fetched tree: `set_frame_observer` tells the frontend when a run starts and ends. |
| `patches/0102-gx-core-native-memory.patch` | Against a build-time copy of `port/runtime/gx/gx_core.cpp`: 30-bit GX addresses, the memory size from the host, the scene from the game's state snapshot, no periodic log line, a timing scope around the decoder thread's work. The file in `port/` is not changed. |
| `CMakeLists.txt`, `build.sh` | Three variants: `null` (Linux, no graphics device), `linux` (Aurora on Vulkan, test build), `switch` (NRO). |
| `linux-gfx.Dockerfile`, `DawnLinux.cmake`, `test-linux.sh` | The off-console picture test: Xvfb and Mesa's software Vulkan driver. |

## Build

`../fetch.sh` and `../build.sh game` first. The NRO also needs `build/sdk` (the graphics SDK of the app's aurora
frontend) and `build/switch-aurora/_deps` (the sources Aurora fetched when the app was built), so that nothing is
downloaded here. Everything is written to `build/native-engine-gfx/`; `build/native-engine/` is only read.

```sh
./build.sh sync     # melee-unlocked exported from the fetched clone, the runner and ../patches as committed, patches applied
./build.sh null     # out/mu-native-gfx-null
./build.sh nro      # out/mu-native-gfx.nro
# test build with a picture:
./build.sh image && ./build.sh deps && ./build.sh linux    # out/mu-native-gfx-linux
```

Run `./build.sh sync` again after `../runner/` or `../patches/` change. It takes them from the last commit, not
from the files on disk, so a half-finished edit there cannot break this build (`MU_SYNC_REF=worktree` takes the
files on disk); it prints what it took and keeps the line in `build/native-engine-gfx/sync-state.txt`.

## Run

On the console (disc image and output folder as for the headless runner; the picture log is
`sdmc:/switch/mu-native/picture.log`, Aurora's pipeline cache `sdmc:/switch/mu-native/aurora/`):

```sh
nxlink -s -a <console ip> build/native-engine-gfx/out/mu-native-gfx.nro --args "--script parity_vs_onett --frames 9000"
```

All arguments of the headless runner work. Added:

| Argument | Effect |
|---|---|
| `--unpaced` | No 60 Hz wait after each retrace: the match plays as fast as the console simulates and decodes it. The runner's own frame times then stay comparable with the headless runner's (they include the wait for the decoder). |
| `--gfx-sync` | Decode inside the game's flush, no decoder thread. |
| `--warm-all` | Compile every pipeline in the cache at launch, not only the ones marked as drawn on this device. |
| `--novsync`, `--stretch` | No vsync; fill 16:9 instead of 4:3. |
| `--gfx-log` | Aurora's information messages. |
| `--gfx-null` | Decode and check every frame, draw nothing. |
| `--headless` | No frontend at all. |

**Measuring with a warm pipeline cache.** With a picture the program closes the whole application at the end (as the app does) instead of returning to the
Homebrew Menu.

Off the console:

```sh
./test-linux.sh /path/to/disc-folder parity_vs_onett 9000 10     # screenshots every 10 s in build/native-engine-gfx/run-linux/shots/
```

## Verified

On aarch64 Linux in a container (Apple M5 Ultra), with the same game image as the headless runner, runner and
patches of commit `861e50a`:

| Check | Result |
|---|---|
| Decoder and frame check, no graphics device (`mu-native-gfx-null`): `parity_vs_onett`, `vs4_busy`, `parity_vs_items_off`, `parity_lcancel`, 9000 retraces each, decoder thread; `parity_vs_onett` also with `--gfx-sync` | every FIFO byte decoded (1.35 to 2.26 GB per run), no unknown opcode, no address outside game memory, no draw or texture copy left out by the frame check; digests identical to the x86-64 reference |
| Picture (`mu-native-gfx-linux`): Aurora on Dawn/Vulkan, Mesa's software driver (llvmpipe) under Xvfb, `parity_vs_onett`, 9000 retraces, paced, decoder thread | window screenshots show the menus, character select, the whole match (stage, fighters, items, HUD, the off-screen magnifier, which is a texture copy) and the results screen, 4:3 in a 1280x720 window; digest identical |
| Same with `--gfx-sync` (the earlier build): `parity_vs_onett` and `vs4_busy` | same pictures; digests identical |
| `vs4_busy` with a picture, `--unpaced`, decoder thread | digest identical |
| Launch pipeline warm-up | a second run compiles the cached pipelines at launch |
| Textures in the game image | drawn (`0x8300F8E0`, `0x8325D100`...): what `patches/0102` is for |

Per-part times of a VS match frame on that machine (`parity_vs_onett`, no graphics device, `--unpaced`): headless
0.30 ms; with the decoder thread the simulation thread works 0.38 ms and then waits 0.28 ms for the decoder,
which works 0.48 ms (0.06 texture copies, 0.28 draw records); with `--gfx-sync` the simulation thread works
0.78 ms. The console is about ten times slower per thread at 1020 MHz.

On the console, by the session that ran it (the first build: decoder inside the flush, no thread placement beyond
the game's core, no reserved core for the shader compilers, CPU at its stock clock, cold pipeline cache):
`parity_vs_onett` was drawn correctly at 4:3 for the whole match, the digest was identical for all 9000 rows and
the program left to HOME. It was too slow: the simulation thread's median was 10.3 ms per VS match frame (3.65 ms
of it the decoder) with 1365 frames over 16.67 ms, and the render thread's median 6.2 ms. This build's changes
answer that run and have not been on the console.

Seen in the screenshots and not explained: on the results screen the winner's small portrait (yellow Kirby) is
noise, with either decoding mode. Dumped from game memory and decoded without the renderer (`MU_GFX_TEX_DUMP`),
the bytes are not a picture in the format the game declares for that texture, so the noise comes with the data;
why is not known. The same costume's portrait on the character select screen is drawn correctly.

## Not verified

- This build on the console: thread priorities and cores, the GPU and memory clock request, the decoder thread,
  the frame times it gives, `--warm-all`.
- That the decoder thread never shows a wrong picture. It reads game memory while the game is still drawing the
  same frame; the game waits for it only at the retrace. The app runs the same decoder the same way, and no wrong
  frame was seen in the Linux screenshots, but a screenshot every few seconds cannot rule out single frames.
  `--gfx-sync` is the exact mode.
- The host services and the runner are compiled by Clang in the NRO (GCC in the headless NRO and in every Linux
  build). The first console run's digest was identical.
- The picture on NVK beyond what was looked at in that one run.

## Known gaps

- Frames the renderer skips are not executed at all, so a texture copy made in a skipped frame (shadows, the
  Pokémon Stadium screen) is missing until the next one.
- Texture copies are kept as RGBA8 on the GPU whatever format the game asked for, depth copies are left out,
  half-size copies are made at full size (all as in the app's `gx::AuroraBackend`).
- melee-unlocked's private stream tokens (`0xF0`-`0xF8`: draw owners, stage tags, render semantics) are not
  understood by this decoder. The headless host never asks the game for them; a host that did would see
  `gx: unknown opcode`.
- A decoder or renderer error stops the picture, not the run: `picture: DECODER STOPPED` or `RENDERER STOPPED`
  with the reason, and the simulation finishes as the headless runner would.
- Still written while a match runs, by code outside this folder: the runner's progress line every 600 retraces (to
  nxlink and `run.log` on the SD card, from the simulation thread), the game's own messages (`host.log`), and the
  SQLite statistics lines of the app's Switch layer (to nxlink, from Aurora's threads).
- Nobody services the applet message loop while the game runs (as in the headless runner).
- No sound output, no controllers, no in-game menu or HUD overlay.

## Licensing

Our files are GPL-3.0-or-later. The program links this repository's GPL-2.0-or-later GX layer, Aurora, Dawn, SDL3
and Mesa as the app does, and melee-unlocked's GPL-2.0-or-later host services from the fetched tree. The built NRO
contains the compiled game and must not be distributed.
