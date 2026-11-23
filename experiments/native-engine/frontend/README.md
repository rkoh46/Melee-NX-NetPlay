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
simulation thread   game -> gx_fifo -> GX decoder (port/runtime/gx/gx_core.cpp) -> finished frame -> queue
render thread       queue -> gx::AuroraBackend (port/runtime/gx/gx_aurora*.cpp) -> Aurora -> Dawn -> Vulkan
```

- The decoder is this repository's, the one the recompiled app feeds from the guest's write-gather pipe. It
  runs inside the game's pipe flush, so the textures and arrays a frame uses are copied while the game is
  stopped. `patches/0102` adapts its address handling: main memory is 40 MiB here and the game image, with
  static data GX may point at, is mapped right after it at `0x82800000`.
- The renderer is `gx::AuroraBackend`, unchanged, on its own thread, which owns the window and Aurora. It
  always draws the newest finished frame; when it is behind, older frames are skipped.
- melee-unlocked's own desktop renderers (OpenGL 4.5, Direct3D, a Windows Vulkan one) were not ported: the
  Switch stack built by this repository offers Aurora on Dawn/Vulkan and nothing else.
- Nothing here writes game memory. The runner is `../runner/runner_main.cpp`, unchanged, with its `main`
  renamed; the host services call the frontend through one weak hook (`patches/0101`).

| Path | What it is |
|---|---|
| `src/frontend.cpp`, `frontend.h` | FIFO sink, 60 Hz pacing, render thread, Aurora start-up, statistics. |
| `src/frontend_main.cpp` | `main`: takes the frontend's arguments out of the command line, then calls the runner. |
| `src/shim/` | The few host functions the decoder needs, on melee-unlocked's host services. |
| `src/switch_glue.cpp` | Two symbols the app's Switch layer expects. |
| `patches/0101-host-frontend-attach.patch` | Against the fetched tree: `set_frame_observer` tells the frontend when a run starts and ends. |
| `patches/0102-gx-core-native-memory.patch` | Against a build-time copy of `port/runtime/gx/gx_core.cpp`: 30-bit GX addresses, the memory size from the host, the scene from the game's state snapshot. The file in `port/` is not changed. |
| `CMakeLists.txt`, `build.sh` | Three variants: `null` (Linux, no graphics device), `linux` (Aurora on Vulkan, test build), `switch` (NRO). |
| `linux-gfx.Dockerfile`, `DawnLinux.cmake`, `test-linux.sh` | The off-console picture test: Xvfb and Mesa's software Vulkan driver. |

## Build

`../fetch.sh` and `../build.sh game` first. The NRO also needs `build/sdk` (the graphics SDK of the app's aurora
frontend) and `build/switch-aurora/_deps` (the sources Aurora fetched when the app was built), so that nothing is
downloaded here. Everything is written to `build/native-engine-gfx/`; `build/native-engine/` is only read.

```sh
./build.sh sync     # own copy of the fetched tree and the runner sources, patches applied
./build.sh null     # out/mu-native-gfx-null
./build.sh nro      # out/mu-native-gfx.nro
# test build with a picture:
./build.sh image && ./build.sh deps && ./build.sh linux    # out/mu-native-gfx-linux
```

Run `./build.sh sync` again after `../runner/` or `../patches/` change.

## Run

On the console (disc image and output folder as for the headless runner; the picture log is
`sdmc:/switch/mu-native/picture.log`, Aurora's pipeline cache `sdmc:/switch/mu-native/aurora/`):

```sh
nxlink -s -a <console ip> build/native-engine-gfx/out/mu-native-gfx.nro --args "--script parity_vs_onett --frames 9000"
```

All arguments of the headless runner work. Added: `--unpaced` (do not wait for 60 Hz after each retrace: the
match plays as fast as the console simulates it and the runner's own frame times stay comparable with the
headless runner's), `--novsync`, `--stretch` (fill 16:9 instead of 4:3), `--gfx-log` (Aurora's information
messages), `--gfx-null` (decode and check every frame, draw nothing), `--headless` (no frontend at all).

By default the simulation waits for a 60 Hz timer after every retrace, so the match plays at its real speed. The
runner's `VS match frames` lines then include that wait; the frontend prints the work time itself:

```
picture: render thread, VS match frames: ... avg ... ms
picture: VS match frames, simulation thread work (game + GX decoder + audio mixing): ... avg ... ms
picture: VS match frames, GX decoder share of that: ...
```

Diagnosis aids (environment variables; on the console they would have to be set in code): `MU_GFX_DRAW_DUMP=MAJOR:MINOR:N`
writes the draw state of the N-th frame drawn in that scene to `<out>/draws.txt`, `MU_GFX_TEX_DUMP=ADDRESS` the bytes
of one texture as the game presented them.

A first run compiles every shader pipeline when it is first drawn, so the picture stops for moments while the
simulation goes on (`stale frames skipped`); the pipelines are cached and compiled at launch from the second run.
With a picture the program closes the whole application at the end (as the app does) instead of returning to the
Homebrew Menu.

Off the console:

```sh
./test-linux.sh /path/to/disc-folder parity_vs_onett 9000 10     # screenshots every 10 s in build/native-engine-gfx/run-linux/shots/
```

## Verified

All on aarch64 Linux in a container (Apple M5 Ultra), with the same game image as the headless runner. Nothing
below was run on a console.

| Check | Result |
|---|---|
| Decoder and frame check, no graphics device (`mu-native-gfx-null`): `parity_vs_onett`, `vs4_busy`, `parity_vs_items_off`, `parity_lcancel`, 9000 retraces each | every FIFO byte decoded (1.35 to 2.26 GB per run), no unknown opcode, no address outside game memory, no draw or texture copy left out by the frame check; digests identical to the x86-64 reference |
| Picture (`mu-native-gfx-linux`): Aurora on Dawn/Vulkan, Mesa's software driver (llvmpipe) under Xvfb, `parity_vs_onett` and `vs4_busy`, 9000 retraces, paced to 60 Hz | window screenshots show the title screen, menus, character and stage select, the whole match (stage, fighters, items, HUD, the off-screen magnifier, which is a texture copy) and the results screen, as a 4:3 picture in a 1280x720 window; digests identical |
| Same, `--unpaced` | digest identical; the renderer skips most frames, the simulation is not held back |
| Launch pipeline warm-up | a second run compiles the cached pipelines at launch (0.7 s) |
| Textures in the game image | drawn (`0x8300F8E0`, `0x8325D100`...): what `patches/0102` is for |
| NRO | compiles and links with the app's toolchain (Clang 19, devkitA64, libnx, Mesa/NVK archives from `build/sdk`) |

Seen in the screenshots and not explained: on the results screen the winner's small portrait (yellow Kirby) is
noise. Dumped from game memory and decoded without the renderer (`MU_GFX_TEX_DUMP`), the bytes are not a picture in
the format the game declares for that texture, so the noise comes with the data; why is not known.

On the test machine (`mu-native-gfx-null`, `--unpaced`, `parity_vs_onett`) a VS match frame costs the simulation
thread 0.70 ms with the decoder, 0.37 ms of it the decoder, against 0.30 ms with `--headless`: decoding the stream
costs more than simulating the frame. Expect the same proportion on the console, where the headless frame is 3.2 ms.

## Not verified

- Anything on the console: the NRO links, and nothing more is known. In particular Aurora starting from this
  program (window, Vulkan device on NVK, the fixed mappings made before it), the thread placement (simulation
  alone on core 2, render thread on core 1, Aurora's workers on cores 0-1), frame times, memory use, and leaving
  through the application exit.
- The host services and the runner are compiled by Clang in the NRO (GCC in the headless NRO and in every Linux
  build). The game image is the same file, so the digest should not change; the run prints whether it did.
- The picture on NVK. The Linux test draws with Mesa's software driver, which shares none of NVK's code.

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
- No sound output, no controllers, no in-game menu or HUD overlay.

## Licensing

Our files are GPL-3.0-or-later. The program links this repository's GPL-2.0-or-later GX layer, Aurora, Dawn, SDL3
and Mesa as the app does, and melee-unlocked's GPL-2.0-or-later host services from the fetched tree. The built NRO
contains the compiled game and must not be distributed.
