# Native engine with a picture

`mu-native-gfx` is the native-engine runner (see `../README.md`) with its GX command stream connected to the
renderer the app already uses. It runs the same scripted matches, shows them on screen, and still prints the
digest comparison and the timing lines. With `--play` it is the game for a person: no script, the app's controller
input, sound, until the player quits. No network.

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
| `src/frontend_main.cpp` | `main`: takes the frontend's arguments out of the command line, then calls the runner or play mode. |
| `src/play_main.cpp` | `--play`: starts the host without a script, a reference or a frame limit. |
| `src/play_io.cpp` | Controllers and sound: connects the app's own `input_sdl.cpp`, `gc_adapter_usbhs.cpp` and `audio_sdl.cpp` (compiled unchanged) to the host's pad and mixer hooks; `--auto-vs`; the way out (L + R + Start); keeps log lines in memory while the game runs. |
| `src/settings.cpp` | Play mode's `settings.ini` and the option words the game asks its host for. |
| `src/shim/` | The few host functions the decoder and the app's input and sound sources need, on melee-unlocked's host services. |
| `src/switch_glue.cpp` | Two symbols the app's Switch layer expects. |
| `patches/0101-host-frontend-hooks.patch` | Against the fetched tree's host services, weak hooks: run start and end, controller read, rumble, each mixed sound block, `host::log` lines, the game's own log lines, the option words. All answer "not mine" in the scripted runs. |
| `patches/0102-gx-core-native-memory.patch` | Against a build-time copy of `port/runtime/gx/gx_core.cpp`: 30-bit GX addresses, the memory size from the host, the scene from the game's state snapshot, no periodic log line, a timing scope around the decoder thread's work. The file in `port/` is not changed. |
| `CMakeLists.txt`, `build.sh` | Three variants: `null` (Linux, no graphics device), `linux` (Aurora on Vulkan, test build), `switch` (NRO). |
| `linux-gfx.Dockerfile`, `DawnLinux.cmake`, `test-linux.sh` | The off-console picture test: Xvfb and Mesa's software Vulkan driver. |

## Build

`../fetch.sh` and `../build.sh game` first. The NRO also needs `build/sdk` (the graphics SDK of the app's aurora
frontend) and `build/switch-aurora/_deps` (the sources Aurora fetched when the app was built), so that nothing is
downloaded here. Everything is written to `build/native-engine-gfx/`; `build/native-engine/` is only read.

```sh
./build.sh sync     # melee-unlocked exported from the fetched clone, the runner and ../patches of commit ae1612e, patches applied
./build.sh null     # out/mu-native-gfx-null
./build.sh nro      # out/mu-native-gfx.nro
# test build with a picture:
./build.sh image && ./build.sh deps && ./build.sh linux    # out/mu-native-gfx-linux
```

`./build.sh sync` takes the runner and `../patches` from commit `ae1612e`, the last state this frontend links
against: the runner has since grown a netplay bridge with its own libraries. `MU_SYNC_REF=HEAD` or `=worktree`
tries a newer one. It prints what it took and keeps the line in `build/native-engine-gfx/sync-state.txt`.

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

**Pipeline cache.** A first run compiles every shader pipeline when it is first drawn: the picture stops for
moments and frames are skipped. The cache is kept in `sdmc:/switch/mu-native/aurora/`, and by default the pipelines
this device has drawn with before are compiled at launch (the app's default). `--warm-all` compiles everything in
the cache at launch, the app's `shader_warmup_all=1`: a one-time session that took six minutes on the console. For
timing, read a run that prints `pipeline warm-up took N s` with N above zero and few frames `skipped as stale`.

By default the simulation is paced like the app's (a 16.667 ms retrace grid), so the match plays at its real speed
and the runner's `VS match frames` lines include that wait. The frontend prints the split itself, per thread, after
the run:

```
picture: simulation thread: work (game, handing the stream over, audio mixing): ...
picture: simulation thread:   of that, handing the stream to the decoder: ...
picture: simulation thread: then waiting for the decoder at the retrace (inside the 60 Hz wait when paced): ...
picture: decoder thread: all work: ...
picture: decoder:   of that, texture copies out of game memory: ...
picture: decoder:   of that, draw records (state, vertex and array copies): ...
picture: decoder:   of that, handing the frame to the render queue: ...
picture: render thread: frame waited in the queue: ...
picture: render thread: all work on a frame: ...
picture: render thread:   of that, converting the frame and feeding Aurora: ...
picture: render thread:   of that, aurora_end_frame (waits for Aurora's FIFO and render workers; they present): ...
picture: controllers read -> the frame simulated from them decoded ...      (play mode)
```

Presenting happens on Aurora's own render worker, so it has no line of its own; a slow present shows as time in
`aurora_end_frame`. Input to picture is the last line plus the queue wait plus the render thread's work, plus
Aurora's worker and the display's wait for its next refresh, which are not measured.

Diagnosis aids (environment variables; on the console they would have to be set in code): `MU_GFX_DRAW_DUMP=MAJOR:MINOR:N`
writes the draw state of the N-th frame drawn in that scene to `<out>/draws.txt`, `MU_GFX_TEX_DUMP=ADDRESS` the bytes
of one texture as the game presented them.

With a picture the program closes the whole application at the end (as the app does) instead of returning to the
Homebrew Menu.

Off the console:

```sh
./test-linux.sh /path/to/disc-folder parity_vs_onett 9000 10     # screenshots every 10 s in build/native-engine-gfx/run-linux/shots/
```

## Play mode

```sh
nxlink -s -a <console ip> build/native-engine-gfx/out/mu-native-gfx.nro --args "--play"
nxlink -s -a <console ip> build/native-engine-gfx/out/mu-native-gfx.nro --args "--play --auto-vs"
```

The game boots to its own menus and runs until the player holds **L + R + Start** (without A) for a second and a
half; the program then shuts the renderer and the sound down, prints its timing lines and closes the application
(HOME menu). `--frames N` stops by itself after N retraces.

- **Controllers** come through the app's input code: Pro Controller and Joy-Con as SDL gamepads through Aurora's
  GameCube mapping, the GameCube adapter read directly over USB, port 1 always present. The game samples them in its
  retrace handler, as on the console.
- **Sound**: the host's mixer runs on the simulation thread, as it must (it reads and writes the game's voice blocks
  and effect buffers, and the game reads them back). Each finished 5 ms block goes into the app's audio ring; SDL's
  audio thread resamples it against the device clock and plays it. `--volume 0..100` (70 in play mode). The output
  runs dry when the game stalls (a scene loading from the SD card: the first console session had one frame of
  548 ms and 0.55 s of gaps in all, the app's own logs show the same kind). After such a gap this program puts the
  40 ms cushion back at once instead of leaving the app's output code to rebuild it by playing 1.5% slow. Every
  600 retraces it keeps the app's `audio: game made ... underruns ...` line, with the scene and what the last ten
  seconds added; the lines are printed after the run.
- **Pacing** is the app's retrace timing: a 16.667 ms grid on the steady clock, restarted after a stall of more
  than two frames. The display's vsync belongs to the render thread.
- **Memory card**: `<out>/card-play/`, kept between sessions (`--fresh-card` empties it). The scripted runs keep
  using their own folder.
- `--auto-vs` walks from boot into a VS match (a fighter against a level 1 computer player, the stage under the
  cursor) and then hands over to the controllers, which are not read before that. It goes by what the game
  reports: the scene from the state snapshot, the picks from the `css-state` lines the game prints; inside a screen
  the steps are counted in retraces from the moment the screen opened. It switches the Boot to CSS code on, so the
  match starts after about 12.5 s; `--auto-vs-menus` goes through the title screen and the menus instead (23 s).
  When a step does not take, it says which and gives up.
- `--play-script FILE` takes a controller script in the app's format; `--wav FILE` records what is played;
  `--audio-raw FILE` the mixer's blocks as it hands them over (any mode, also `--headless`); `--cache-dir DIR` uses
  another folder for Aurora's pipeline cache.

### Game options: settings.ini

Play mode reads `<out>/settings.ini` once at start (`--settings FILE` for another; a commented template is
written when there is none) and answers the game's option words from it, the way melee-unlocked's Windows host
builds them from its settings panel. One `key value` per line, upstream's `port-settings.ini` names. By default
the game runs as upstream runs it: its built-in code set on (UCF and the rest), everything unlocked.

| Key | Values | |
|---|---|---|
| `unlock_all` | 1 / 0 | 0: the memory card decides |
| `results_screen` | 1 / 0 | 0: back to the character select after a match |
| `palstockicons`, `noscreenshake` | 0 / 1 | |
| `screenshake` | 0..200 | percent |
| `stockhudscale`, `damagehudscale` | 75..175 | percent |
| `playertags`, `hide_ko_explosions`, `hide_hud_emblems` | 0 / 1 | |
| `community_codes` | hex | 1000 HUD fade, 2000 respawn platform colours, 4000 Nana percent, 10000 no GO!, 20000 / 40000 / 80000 hitbox id colours / order / angles, 200000 V-cancel sound, 1000000 no fighter shadows, 4000000 centred 2P HUD; offline codes: 400000 Instant Melee, 800000 free pause camera, 2000000 boot to CSS |
| `stadium_frozen`, `freeze_fd` | 0 / 1 | |
| `widescreen` | 0 / 1 | the 16:9 code; the picture then fills the screen (as `--stretch`) |
| `arcade_hitboxes`, `arcade_projectile_hitboxes`, `arcade_hitboxes_behind` | 0 / 1 | |
| `arcade_backdrop` | 2 | a grid behind the stage; `arcade_grid_rgb RRGGBB` its colour |
| `cpu_20xx` | 0 / 1 | |
| `cpu_tech`, `cpu_getup` | 0..5 | |
| `cpu_di` 0..3, `cpu_sdi` 0..6, `cpu_notaunt`, `cpu_lcancel`, `cpu_nojab`, `cpu_notransform` | | |
| `mute_voices` | 0 / 1 | |
| `gamelanguage` | 0 / 1 / 2 | the save's, Japanese, English |
| `music` | 0..100 | |
| `lowpoly`, `fodreflections`, `cssbackgroundfree`, `training_hud`, `arcade_camera_2d`, `audio_early_keyon` | | passed through, not looked at |
| `vanilla` | 0 / 1 | ours: the retail game without the built-in code set; the CPU, 20XX and Stadium options are then not given, as upstream's vanilla session does |

What upstream sends only outside network play (the offline community codes, the hitboxes, the 2D camera, faster
sound effects, the CPU options) is sent here because play mode has no network session, replay or tournament.
Left for later, because they need more than a word: the 20XX TE feature bits (upstream gives them only with a TE
save loaded), the Training Lab bits, shield colours, True 16:9 (a renderer feature), hiding the scenery behind the
grid and the Arcade look's plain backdrop and combo counter (renderer and overlay), the input display and
frame-data overlay, anything from a mod disc (20XX Hack Pack, TM-CE, m-ex).

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

Play mode on Linux (runner and patches of commit `ae1612e`, game image `6be32fe9...`):

| Check | Result |
|---|---|
| `--play --auto-vs` (Boot to CSS code), `--auto-vs-menus` with `vanilla 1` on an empty card, `--auto-vs-menus` with the code set on an existing card | the match starts at retrace 751, 1388 and 1388; the right-hand move onto the tag works from 10 to 14 retraces (12 is used) |
| `--play` with a controller script through the app's input code (`input_poll`) | menus, match, the fighter moving; L + R + Start ends the run cleanly |
| Options, one run with 19 lines set | the program's `play: options 50000003 01800000 40001005, display 06010D32, audio 2` line; the game's own lines `offline codes 2000000`, `community codes 4010000`, `option2 00800000 in effect`, `hitbox switches 20`, `cpu training options 00001005 in effect`, `cpu 1: L-cancel`; screenshots: Japanese text, all 26 fighters and every stage, a 16:9 picture, hitbox bubbles, the grid, tags that stay, PAL stock row, HUD at 150%, no emblems, the 2P HUD centred |
| Options not looked at, only passed | skip results (no match was played to its end), Stadium and Final Destination frozen, KO explosions hidden, no screen shake, voices muted |
| Sound: `--wav` of a scripted picture run against `--audio-raw` of a `--headless` run, `parity_vs_onett`, 149.9 s | equal sample for sample; not silent |
| Sound device path | SDL's dummy device only; it consumes slower than real time, so its drops say nothing about the console |
| Scripted mode after these changes | digest identical, with and without a picture: the option hooks answer only in play mode |

On the console, by the session that ran it:

- First build (decoder inside the flush, no thread placement, cold cache): picture correct, digest identical, too
  slow (simulation thread median 10.3 ms per VS match frame, 1365 frames over 16.67 ms).
- Decoder-thread build, second run with `--warm-all`, CPU at about 2091 MHz: simulation thread 3.65 ms average per
  VS match frame, none over 16.67 ms; waiting for the decoder 0.22 ms; decoder thread 2.96 ms; render thread 4.28 ms
  (2.22 converting and feeding Aurora, 1.92 in `aurora_end_frame`); 8987 of 8998 frames drawn; digest identical.
  `--warm-all` took 357 s there. Without it the first run had 9 match frames over budget.
- Play mode build (f357035): a remote controller bot, seen by the system as two pads, got through the title and
  the menus to the character select; picture correct; L + R + Start left to HOME cleanly. Sound ran with 66
  underruns (554 ms) in 90 s of booting and menus, none of them in a match because no match was reached: the
  time-driven `--auto-vs` of that build was thrown off by the second pad joining as a second player. Menu frames
  cost the simulation thread about 5 ms at 1774 MHz.
- The state-driven `--auto-vs`, `settings.ini`, the sound refill and the statistics lines have not been on the console.

Aurora's `tcg[N] source 21 (type 1) is not handled; the coordinate is zero` lines are not new with this engine: the
app's own console logs have the same line with the same numbers.

Seen in the screenshots and not explained: on the results screen the winner's small portrait (yellow Kirby) is
noise, with either decoding mode. Dumped from game memory and decoded without the renderer (`MU_GFX_TEX_DUMP`),
the bytes are not a picture in the format the game declares for that texture, so the noise comes with the data;
why is not known. The same costume's portrait on the character select screen is drawn correctly.

## Not verified

- On the console: the new `--auto-vs`, `settings.ini` and every option in it, whether the sound refill removes the
  underruns that follow a loading stall, sound during a match, the GameCube adapter and rumble, a long session.
- A real gamepad on Linux: the test drove the app's input code with a script, below its SDL and Aurora PAD part.
- Input to picture: the program measures up to the render thread's hand-off; Aurora's render worker, the swap
  chain and the display are after that.
- That the decoder thread never shows a wrong picture. It reads game memory while the game is still drawing the
  same frame; the game waits for it only at the retrace. The app runs the same decoder the same way, and no wrong
  frame was seen, but screenshots cannot rule out single frames. `--gfx-sync` is the exact mode.
- The picture on NVK beyond what was looked at in the console runs.

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
- Still written while a match runs, by code outside this folder: in scripted mode the runner's progress line every
  600 retraces (to nxlink and `run.log` on the SD card, from the simulation thread); in both modes the game's own
  messages (`host.log`) and the SQLite statistics lines of the app's Switch layer (to nxlink, from Aurora's threads).
- Nobody services the applet message loop while the game runs (as in the headless runner).
- A first session compiles pipelines as they are first drawn and stutters; they are cached per device. The app is
  in the same position: this repository ships no pipeline seed, and the app's cache lives in its own folder
  (`--cache-dir sdmc:/switch/melee-nx-netplay/Cache` would share it; not tried).
- The app's adapter code looks for a GameCube adapter every two seconds from the simulation thread for as long as
  there is none; here it gets three looks and then one a minute until an adapter has answered once.
- No in-game menu or HUD overlay, no controller remapping, no volume control while running, no Slippi music player.

## Licensing

Our files are GPL-3.0-or-later. The program links this repository's GPL-2.0-or-later GX layer, Aurora, Dawn, SDL3
and Mesa as the app does, and melee-unlocked's GPL-2.0-or-later host services from the fetched tree. The built NRO
contains the compiled game and must not be distributed.
