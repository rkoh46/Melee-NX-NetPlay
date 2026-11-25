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
| `src/mods.cpp` | Mod profiles: what a patched disc image changes is served over the retail disc's files; the 20XX Hack Pack and 20XX TE; the mod commands of `slippi_command`; a memory card folder per profile. |
| `src/slippi_files.cpp` | Slippi's menu files (`slippi_menus 1`), built from this repository's vendored patches and the player's disc. |
| `src/shim/` | The few host functions the decoder and the app's input and sound sources need, on melee-unlocked's host services. |
| `src/switch_glue.cpp` | Two symbols the app's Switch layer expects. |
| `src/app.cpp` | The app (no arguments): the app's folders, `launcher.ini`, the memory card's one-time backup, controller configuration (remapping, deadzones, rumble), the HUD's numbers, the window queries of the app's menu code, the layout and disc screens. |
| `src/app_menu.cpp` | The in-game menu's rows wired to this engine (the app's main rows, four `settings.ini` pages, the feature modules' pages, the controller setup), through the app's menu interface (`game_menu.h`); `src/mu_panel.cpp` draws them. |
| `src/app_services.cpp` | Services program only: the screens before the game, which are melee-unlocked's launcher (`src/mu_home.cpp`), and a finished update's swap. |
| `src/mu_panel.cpp`, `src/mu_home.cpp`, `src/mu_mods.*`, `src/mu_ui.h`, `scripts/mu_ui_extract.py` | melee-unlocked's interface on the Switch (see "melee-unlocked's interface" below): its settings panel in the game (its drawing code compiled from the fetched tree; the app's menu interface implemented behind it, so `game_menu.cpp` is no longer built), its launcher as the home screen before the game (its layout, palette, labels and artwork), the Mods screens over the mod installer. |
| `patches/0115-aurora-imgui-host-gamepad.patch` | Against the build-time copy of Aurora: `aurora_imgui_host_gamepad`, so the panel's gamepad keys come from the app's controllers (melee-unlocked's `settings_imgui_input.h` feed) and ImGui navigates with them. |
| `src/features/overlays.cpp`, `src/overlay_state.h`, `src/overlay_draw.cpp` | The on-screen overlays and picture options (see "Overlays and picture options"): settings, menu rows and what the game reports on the simulation thread; the drawing on the render thread through Aurora's ImGui pass. |
| `test-overlays-linux.sh` | The overlays and picture options in play mode with controller scripts and screenshots. |
| `src/controls.cpp`, `src/controls_core.h`, `src/controls_files.*` | The controller setup beyond the button remap (see "Controller setup"): bindings, box mode, analog options, profiles, Dolphin import, chords, report rates. |
| `patches/0111-game-menu-open-page.patch` | After 0104: `menu_open_extra`, the menu opened on a host page by its title (the panel opens on that page's category). |
| `src/proai/onnx_lite.*` | Fight a pro's inference engine: a minimal ONNX reader and the operators Phillip's policies use (Gemm on half-precision weights with NEON, LSTM-style gates, layer norm, sampling), no third-party runtime (see "Fight a pro and the Practice menu"). |
| `src/proai/ort_lite.cpp`, `src/proai/include/` | The onnxruntime-shaped calls upstream's `phillip_ai.cpp` makes, on onnx_lite (not onnxruntime), and the thin headers upstream's `practice_menu.cpp` and `phillip_ai.cpp` are compiled on (host log and disc reads, ImGui 1.92's texture type over `aurora_imgui_add_texture`, inert warm-up settings). |
| `src/proai/catalog.json`, `src/proai/proai_fetch_curl.cpp` | The app's Fight a pro catalog (`../tools/proai_catalog.py` from Hugging Face's index and the models' metadata), embedded; the downloads (services program, libcurl, SHA-256 checked). |
| `src/features/proai.cpp`, `src/proai/practice_overlay.cpp`, `src/proai/practice_state.h` | Command 0xEA, the ProAI folder, upstream's practice_cpu / native_practice calls, the Practice menu's chord, controller and settings (simulation thread) and its drawing (render thread, from `overlay_draw.cpp`). |
| `patches/0112-phillip-ai-own-engine.patch`, `patches/0113-practice-menu-download-question.patch` | Against build-time copies of upstream's `phillip_ai.cpp/.h` and `practice_menu.cpp` (`build/native-engine-gfx/gen/proai`): the own engine, the app's folder, the device timing, downloads only when chosen; the download question. |
| `test-proai-linux.sh` | The Practice menu (four looks), Fight a pro in Training, the download from a local stand-in, with screenshots and onnxruntime run on the recorded steps. |
| `src/replay_view.h`, `test-replays-linux.sh` | The replay viewer's overlay state (replays.cpp to overlay_draw.cpp and the render loop's repaint); the replay viewer, takeover, coach and Replays page in play mode with screenshots. |
| `test-mods-linux.sh` | Training's random numbers and Practice CPU slots through the app's menu, and the vanilla-only session, with controller scripts and screenshots; `install`: the Mods page installs rogueMelee, then a match on it. |
| `src/modinstall/`, `src/mods_service.*`, `src/mu_mods.*` | The mod installer (catalog, download, zip/7z, VCDIFF, checks, profiles) and its Mods screens (the home screen's Mods page, the panel's MODS) (see "Mod catalog, download and install"). |
| `src/feature_calls.h` | Play mode's weak calls into the feature modules (`src/feature_hooks.h`, `src/features/`; the modules' own interface). |
| `patches/0104-game-menu-host-pages.patch` | Against a build-time copy of `game_menu.cpp/.h`: pages a host adds, greyed rows, hidden rows, row labels, the Quit action, a HUD extra, the console's row layout off the console (`MU_MENU_CONSOLE`). |
| `patches/0105-switch-signin-off-console.patch` | Against a build-time copy of `switch_signin.cpp`: builds off the console (no software keyboard there), controllers through the host's input path. |
| `test-app-linux.sh` | The app without a console: boot, menu, handover (the menu with the D-pad, B to close, then A), sign-in, update, online cases. |
| `app-build.sh`, `VERSION` | What the build computer runs to publish this app instead of the recompiled one (see "Publishing"). |
| `patches/0101-host-frontend-hooks.patch` | Against the fetched tree's host services, weak hooks: run start and end, controller read, rumble, each mixed sound block, `host::log` lines, the game's own log lines, the option words. All answer "not mine" in the scripted runs. |
| `patches/0103-gx-bridge-lines-points.patch` | Against a build-time copy of `port/runtime/gx/gx_aurora_bridge.cpp`: lines and points go to Aurora (the game's debug text screens are made of them), and a refused draw is reported once per reason instead of once per draw. |
| `patches/0108-aurora-imgui-draw-list-delete.patch`, `patches/0109-aurora-picture-options.patch` | Against a build-time copy of Aurora (`build/sdk/aurora` -> `build/native-engine-gfx/gen/aurora`, which both picture variants compile): ImGui's frozen draw lists are freed with `IM_DELETE` (they were made with `IM_NEW` and freed with `operator delete`: AddressSanitizer's alloc/dealloc mismatch, now on in the sanitizer runs); `aurora_set_picture` (brightness, contrast, vibrance, sharpening, the CRT looks) in the present pass. `build/sdk` is not changed. |
| `patches/0116-gx-looks.patch`, `src/looks_live.h`, `src/features/looks.cpp`, `test-looks-linux.sh` | The looks (after 0102, 0103, 0107, 0106): the decoder reads the game's draw-owner and stage tokens; the Aurora bridge applies melee-unlocked's look rules per draw (surfaces, tints, wire, the Lightweight background rule, Visual effects, the look's and the Arcade look's background) and True widescreen's projection rule; gx_aurora.cpp uploads treated textures. The module keeps the settings (`look`, `lightweight`, `effects`, `look_*`, `truewidescreen`) and the Looks page, and asks the game for owner tokens only while a look needs them. `test-looks-linux.sh all` shoots every preset. FEATURES.md "Look presets" and "True widescreen" have the rules and numbers. |
| `src/features/cosmetics.cpp` | Skins: costume and stage files from `Cosmetics/` on the SD card and the mod discs' costumes, picked on the select screens (commands 0xF9, 0xFB-0xFD), with melee-unlocked's online rule compiled from the fetched tree (`build.sh sync` extracts the portable parts of its cosmetic_mods.cpp into `gen/cosmetics_*.inc` and copies pack_skin_rule); mods.cpp serves a session that can go online only the profile's look-only costumes through it. `test-mods-linux.sh skins`. |
| `patches/0107-gx-core-array-pool-fence.patch` | After 0102: the decoder reuses a vertex-array buffer the render thread let go of only after an acquire fence (0.4.2; ThreadSanitizer). |
| `patches/0110-audio-follows-game-speed.patch` | Against a build-time copy of `port/runtime/host/audio_sdl.cpp`: the output's resampling ratio follows the game's speed (online time sync, 0.4.3). |
| `patches/0102-gx-core-native-memory.patch` | Against a build-time copy of `port/runtime/gx/gx_core.cpp`: 30-bit GX addresses, the memory size from the host, the scene from the game's state snapshot, no periodic log line, a timing scope around the decoder thread's work. The file in `port/` is not changed. |
| `CMakeLists.txt`, `build.sh` | Three variants: `null` (Linux, no graphics device), `linux` (Aurora on Vulkan, test build), `switch` (NRO). |
| `linux-gfx.Dockerfile`, `DawnLinux.cmake`, `test-linux.sh` | The off-console picture test: Xvfb and Mesa's software Vulkan driver. |

## Build

`../fetch.sh` and `../build.sh game` first. The NRO also needs `build/sdk` (the graphics SDK of the app's aurora
frontend) and `build/switch-aurora/_deps` (the sources Aurora fetched when the app was built), so that nothing is
downloaded here. Everything is written to `build/native-engine-gfx/`; `build/native-engine/` is only read.

```sh
./build.sh sync     # melee-unlocked exported from the fetched clone, the runner and ../patches (see below), patches applied
./build.sh null     # out/mu-native-gfx-null and out/mu-native-gfx-null-services
./build.sh nro      # out/mu-native-gfx.nro and out/mu-native-gfx-services.nro
# test build with a picture:
./build.sh image && ./build.sh deps && ./build.sh linux    # out/mu-native-gfx-linux
# sanitizer test builds (own build folders; this program's code and the services netplay object instrumented,
# not Aurora, Dawn, SDL or the game image):
MU_SANITIZE=address ./build.sh null|linux   # out/mu-native-gfx-<variant>[-services]-asan
MU_SANITIZE=thread ./build.sh null|linux    # ...-tsan
```

The sanitizer builds are run by `test-online-linux.sh` and `test-app-linux.sh` with `MU_SANITIZE=address|thread`
(reports in `san.*` beside each console log). `MU_DEAD_FIRST=N` makes the matchmaking stand-in pair each side's first
N searches with a peer that never answers (`../tools/mock_matchmaking.py --dead-first N`): each connection attempt
fails after 8 s and the side searches again, the path of the 0.4.1 console crash. ThreadSanitizer runs use
`MU_VARIANT=null` (test-online-linux.sh, no picture, `--online unranked`): with Mesa's software Vulkan driver
ThreadSanitizer's runtime once hung (the game thread waiting on the decoder, the decoder and Aurora's threads inside
the sanitizer's own allocator lock and a raw futex in llvmpipe). `../tools/tsan_graphics.supp` suppresses reports
from the graphics stack only (llvmpipe, Aurora's workers, the decoder's array pool fence).

`./build.sh sync` takes the runner and `../patches` from the working tree (`MU_SYNC_REF=<commit>` takes them from a
commit; play mode needs the runner's `--net-delay auto`, which is newer than `1c556fc`), and from `build/native-engine/`
the game image, its snapshot exclusion list and the netplay objects `../build.sh` made. It prints what it took and keeps
the line in `build/native-engine-gfx/sync-state.txt`.

**Two programs.** Each variant is linked twice from the same objects: `mu-native-gfx` with the netplay object that
has no services (local peers only), and `mu-native-gfx-services` with the services object (the app's matchmaking
client, user file, reporting and HTTP; curl and mbedTLS on the console). The console's services object is made by
`MU_NET_SERVICES=1 ../build.sh nro` (pinned to the service address in `tools/slippi_pin.json`); without it only the
first NRO is built. The netplay object is linked with only its defined symbols made weak (`netplay_link.sh`): the
first services NRO weakened its references too, so libcurl was never linked and the first HTTPS call jumped to address 0
on the console (`curl_global_init` inside `slippi::report::ensure_global`). Every build now checks the linked programs
(`netplay_link: ...` lines) and fails if that comes back. At start the services program runs a self-check before the
netplay host starts, contacting nothing: `curl_global_init`, one request handle, the TLS library libcurl reports and,
on the console, one context of the console's ssl service and a read of the random service (devkitPro's libcurl does TLS
through them). It prints `services check: libcurl ... OK ...` or `services check: FAILED: <step> ...`; after a failure
the session stays offline (`--online` ends with code 1). The Linux services object is made by this script from the synced runner and is pinned to
`ws://127.0.0.1:8765`, the loopback stand-in `../tools/mock_matchmaking.py`: a Linux build cannot reach a real service.

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
| `vanilla_only` | 0 / 1 | upstream's `--vanilla-only` (also the command-line flag here): no mod profile, no CPU, 20XX, Stadium or Training extras, its own memory card folder `<card>/VanillaOnly`, replays recorded with gameplay mods refused, and a disc that is not GALE01 revision 2 refused; the built-in code set stays (unlike `vanilla`) |
| `onlinedelayauto` | 1 / 0 | online input delay from the round trip (2..5 frames); default 1 |
| `onlinedelay` | 1..9 | frames, with `onlinedelayauto 0` |
| `blockcode1..3` | connect code | Unranked turns these opponents down and searches again (upstream's skip list; patches/net-0004) |
| `directpause` | 1 / 0 | 0: no pausing in Direct, when every player's build is a Melee-NX-NetPlay one (otherwise Slippi's rule: Start pauses Direct) |
| `portcolour1`, `portcolour2` | -1..3 | online 1v1 port colour, first and second choice (0 red, 1 blue, 2 yellow, 3 green), between Melee-NX-NetPlay builds |
| `shield_color_online` | RRGGBB | this player's shield online; the opponent's own choice shows when their build is a Melee-NX-NetPlay one |

What upstream sends only outside network play (the offline community codes, the hitboxes, the 2D camera, faster
sound effects, the CPU options) is sent here offline and cleared online (see Netplay below).
Left for later, because they need more than a word: the Training Lab bits, shield colours, True 16:9 (a renderer
feature), hiding the scenery behind the grid and the Arcade look's plain backdrop (renderer), m-ex discs, Training Mode CE, Akaneia, rogueMelee and Melee
Maker (their layers load as files; what the host does for each beyond that is not written).

### Overlays and picture options

`src/features/overlays.cpp` (settings, menu rows, the simulation-thread half) and `src/overlay_draw.cpp` (render thread,
Aurora's ImGui pass, under the app's menu and HUD). The overlays only read what the game hands its host anyway: the pads
it got, `MuHostApi.hud_player`/`hud_combo` (read-only on the game's side), `MuGameApi.lcancel_view`, the state snapshot,
Training's 0xF4, the netplay host's 0xB3 answer (names) and the option word 2 the game reads; nothing is written into the
game, so a session with overlays plays exactly as one without (online: `net check` equal with names and the frame counter
on). Play mode and the app; settings.ini keys (upstream's names), also on the menu's **Overlays** and **Picture** pages:

| settings.ini | |
|---|---|
| `inputoverlay 1`, `inputoverlayports BITS` (1 = P1), `inputoverlayvalues 1`, `inputoverlaystick 1..10` (size), `inputoverlayhideborder 1` | a GameCube pad per port along the top of the picture |
| 20XX TE's input display (its menu, `te_options2` bit 2000) | the same, one per player, in offline matches and replays |
| `lab_overlay 1` (Training page) | offline Training: per fighter its action, frame, percent, speed and pad, then the last frame advantage (0xF4) |
| `showfps 1`, `performance 1` | presented and game frames per second, simulation time; graph of the last 4 s (simulation time, time between pictures) |
| `adapterrate 1` | the GameCube adapter's measured report rate (console; the Linux build reads no adapter) |
| `playernicknames 1` | online: the players' Slippi names over the fighters |
| `arcade_combo_counter 1` | offline: "N HITS" over the attacker's damage |
| `lcancel_flash_mode 0..6`, `lcancel_success_flash 0/1/2` | off, MU missed (1), made (5), both (6): the fighter's model tinted red or green (white with success colour 1) for 8 frames (patches/0120, the game marks draw owners while a MU mode is chosen); TE missed (2), made (3), both (4) with the 20XX TE save: TE's own flash from its word 2 bits, the success colour from `lcancel_success_flash`. Online: this console's player only |
| `latencytest 1` | X + D-pad Down paints the picture black, then white; X + D-pad Up ends it |
| `aspect 0..4` | Auto (4:3, or the screen with the 16:9 code), 73:60, 4:3, 16:9, stretched; live |
| `scale 0..4` | internal resolution: Auto (the display) or 640x480 times N; live |
| `ssaa 1..4` | supersampling: that resolution rendered N times per axis and scaled down (off, 4x, 9x, 16x); live |
| `brightness`, `contrast` 0.5..1.5, `vibrance` 0..2, `sharpness` 0..1 | Aurora's present pass (patches/0109); live |
| `crt_filter 0/1/2`, `crt_curve 0..3` | off, studio monitor, home television (curved); live |
| `borderart grid` or `FILE.bmp`, `borderedge 0/1` | art in the side bars (`<folder>/Borders`, the app's folder in the app), soft shadow at the picture's edge |

The Picture page's **Quality preset** row sets internal resolution, sharpening and launcher.ini `msaa`/`anisotropy` (those
two at the next start): Low, Medium, High (the default), Ultra, or shows Custom.

```sh
./test-overlays-linux.sh /path/to/disc-folder match training latency te picture studio scale1 scale2   # MU_MODS_DIR for te
MU_APP_SETTINGS="playernicknames 1" ./test-app-linux.sh /path/to/disc-folder online overlaymenu
```

### Looks, True widescreen and skins (0.4.6)

All off by default and display only. settings.ini: `look N` (0 Off .. 11 Classic Style; or upstream's parts
`lightweight`, `effects`, `look_fighter`, `look_stage`, `look_menus`, `look_unfiltered`, `look_colours`, `look_background`,
`look_background_rgb`), the Arcade look's `arcade_backdrop 1|2` with `arcade_background_rgb`, `truewidescreen 1`; the
in-game menu's Looks page has a row for the preset and each part. Skins: costume files in `Cosmetics/Costumes/` (named for
their slot, `PlFxGr.dat` or `PlFxGr - Name.dat`, or a folder per skin), stage files in `Cosmetics/Stages/`, and every mod
disc's changed costumes; L / R on a picked costume, X / Y on a highlighted stage; `Cosmetics/selection.txt` keeps the
picks; `cosmetics 0` turns skins off. Online only a costume with the standard skeleton stays on, served with the standard
rest pose (melee-unlocked's rule and code); stage skins never. On the console: copy a costume file into
`sdmc:/switch/melee-nx-netplay/Cosmetics/Costumes/`, pick it with R on the character select, and check that an online
match shows it only when the log says `stays on online`.

### Controller setup

`src/controls.cpp`, `src/controls_core.h`, `src/controls_files.*` (the app only): melee-unlocked's controller options on the
console's two kinds of controller, SDL controllers (Pro Controller, Joy-Con, the Switch Online GameCube controller, pads the
system accepts) and GameCube controllers on the adapter (one setup per adapter port). The processing is adapted from upstream's
`input_bindings.h` (GPL-2.0-or-later, notice kept in the files); `box_melee.h` and `settings_chord.h` are compiled from the
fetched tree unchanged. A controller whose setup equals its defaults is passed through untouched, and a setup only changes that
controller's own pad, as pressing other buttons would: nothing new reaches the game or the other side online.

- **Bindings** for all 27 upstream actions: on an SDL controller the twelve buttons stay the Controls page's remap, and the
  menu's *Controller bindings* page adds C-stick and stick directions on buttons, L / R travel from a trigger or a button (with
  upstream's per-binding level), Mod X / Mod Y, light and mid shield buttons and the stick modifier (walk, `Stick modifier
  push`); an adapter port lists all 27 (upstream's gcadapter table: A and B swapped, L travel on X...). A or Right on a row,
  then the control: bound (the menu ignores the press until it is let go, so it is not read through the new binding). Left
  unbinds.
- **Box mode**: binding Mod X or Mod Y turns the button-made stick and C-stick into the B0XX Melee coordinates with the
  opposing-direction rule (upstream `box_melee.h`); Mod X + Mod Y makes the C buttons the D-pad. Here the light and mid shield
  buttons work without box mode too (upstream: only in it). A box in its GameCube mode on the adapter, or in a mode the system
  takes as a Pro Controller, already sends analog values from its firmware and needs none of this; the console's HID does not
  offer a box in DInput or keyboard mode at all.
- **Controller setup** page: stick and C-stick inversion per axis, stick radius (upstream's Radius), round deadzones in the
  game's units, the snapback filter (SDL controllers only, as upstream: not on a GameCube controller), the adapter's L / R click
  point (default: the trigger's own switch), L / R trigger caps (analog only, no click). An SDL controller's deadzone and press
  point remain the Controls page's rows.
- **Profiles** (*Controller profiles* page): `<app folder>/ControllerProfiles/NAME.profile` in upstream's format. Save as a new
  profile ("Controller 1", "GameCube 1"...), save over, load, delete (A twice), default setup. `gcadapter` profiles carry
  upstream's numbers, so one saved by melee-unlocked on a PC loads here and the other way round; SDL controllers get their own
  kind (`sdlgamepad`, SDL button numbers). Levels and analog settings follow as `opt.` / `app.` lines, which upstream skips.
- **Dolphin import**: copy a Dolphin `User/Config/Profiles/GCPad/NAME.ini` or `GCPadNew.ini` into `<app folder>/Dolphin/`; the
  page lists each profile (each `[GCPadN]` of GCPadNew.ini), and *Import it into this controller* reads its SDL or XInput
  bindings onto the selected SDL controller (keyboard profiles are refused: the console has none) and saves the result as the
  profile "Dolphin NAME". Plain controls and `A | B` are read; stick axes become the stick swap and inversion; Dolphin's dead
  zones, trigger threshold, modifier range and radius become the matching rows; anything else (modifier expressions, raw
  `Button 3` joystick numbers) is counted as skipped (`controls: Dolphin ... imported ...: N bindings read, M not understood`).
- **Chords** (upstream `settings_chord.h`): Start + D-pad Down + Z opens or closes the menu, Start + D-pad Up + Z opens it on the
  Training page (upstream's Practice panel); the chord's buttons are kept from the game while it is held. In offline Training a
  lone Start waits up to three polls for the rest of a chord, so Training's pause menu does not open under the page. L + R + Start
  still opens the menu.
- **Report rates** (`adapterrate 1`): beside the adapter's line, each Pro Controller, Joy-Con or Switch Online GameCube controller
  with the rate at which the system's HID gives this program new states (new sampling numbers per second). Console only.
- launcher.ini: `padx.<SDL GUID>=` or `padx.gc<port>=` (only what differs from the defaults), written when the menu closes.
- The remap now runs before the in-game menu sees the pads (inside play_io's `menu_frame`, before it was after it), so the
  menu, its hand-over masks and the chords see the pads as the game gets them; for the input-suggestion reads the box rule and
  the snapback filter keep their own state.

Tests: `../tools/controls_check.sh` (65 checks: box coordinates and the opposing-button rule, snapback, deadzone, radius,
click and cap, adapter and SDL bindings, profile round trip and an upstream gcadapter profile, Dolphin XInput, SDL and keyboard
profiles, GCPadNew.ini listing) and `./test-app-linux.sh DISC controls` (port 1's scripted pad read as adapter port 1 with
`MU_CONTROLS_TEST_ADAPTER=1`; `MU_CONTROLS_TEST_PAGE` makes the Start + D-pad Down + Z chord open that page; a second start loads the saved profile).
Results on Linux: A captured onto Z then Z gives the game A + Z, the saved X/Y swap, a full push right reaching the game as -114
(inverted, radius 90 %), launcher.ini `padx.gc1=m0=16,m2=2048,m3=1024,inv=1,rad=90`, the profile loaded on the second start;
after these changes the five reference scripts with `mu-native-gfx-null` and `parity_vs_onett` with a picture IDENTICAL,
`test-app-linux.sh online` and `test-net-linux.sh` with `net check` equal on both sides.

### Mod profiles

`mod_profile NAME` in settings.ini loads the layers of `<out>/mods/Profiles/NAME.ini` (upstream's form: `iso = file`
and `gci = file` lines in load order, paths relative to `<out>/mods`) or, without that file, every disc image and
then every save in the folder `<out>/mods/NAME/`. Several names separated by commas load one after the other. On
the console `<out>` is `sdmc:/switch/mu-native`.

- A disc image must be a patched NTSC 1.02 disc. It is compared with the retail disc file by file; only files that
  differ or are new are served, read from the image on demand. Later layers win. Nothing from a mod's `main.dol`
  runs. The comparison reads both discs whole (minutes from an SD card, the first time); its result is kept in
  `<out>/mods-cache/` while the image's size and time stamp stay the same.
- The 20XX Hack Pack is recognised by its own files. Its menu files and the files that carry its code are not
  served, and the game's native rewrites of its features get its data through command 0xFA (upstream's
  `hackpack_source.h`, compiled from the fetched tree).
- The 20XX Tournament Edition save is recognised by the SHA-256 of the released `20XXTE-v2d-r4-USA.gci` (upstream
  also accepts a copy that has been played on). It is not put on the memory card and none of it is loaded; it
  switches the game's native TE features on (`mod_te_enabled 0` keeps them off). Any other save is copied to the
  profile's memory card once.
- A profile has its own memory card folder, `<out>/card-NAME`.
- What the game changes from its own menus (TE's settings, the pack's choices) is written to `settings.ini.state`
  and read back at the next start.
- An online mode that plays the retail game gets the retail files back (command 0xF5), as upstream. A session that
  can go online (the services program with the Slippi menus and a user file, or `--net`/`--online`) loads no profile.

`slippi_menus 1` serves Slippi 3.7.0's menu files (ten of the disc's files with Slippi's patch applied, plus
`GameSetup_gui.dat` and `SlippiCSS_gui.dat` with their links into the disc resolved) and sets the game's
Slippi-menus bit: the game then starts on its Online Play page. Nothing is sent anywhere by this.

### Mod catalog, download and install

The app installs the mods of melee-unlocked's catalog for the player, as melee-unlocked's launcher does
(`port/app/launcher_mods.inl`, `launcher_mod_catalog.cpp` in the fetched tree), with this program's own code for every
step. melee-unlocked runs `xdelta3.exe`, `7zr.exe` and `tar.exe`; here nothing from a mod or from another program is run.

- **Catalog**: `mods-catalog-2.json` of the fetched tree, built in (`build/native-engine-gfx/<variant>/gen_mods/`). Checked
  with melee-unlocked's rules: every field, https links, well-formed hashes, unique ids. **A mod gets automatic download
  only when the catalog records a licence or a permission** (policy `licensed` or `permission_granted`: 20XX TE, Training
  Mode CE, Akaneia, ACE, the 20XX Hack Pack). The others (Melee 3D, Melee Maker, rogueMelee: `link_only`; Animelee and
  Animelee DBZ: MediaFire pages, which melee-unlocked's launcher cannot fetch either) are listed with their page: the
  player downloads the file and puts it in `mods/Downloads/`, and Install takes it from there. MediaFire is not scraped.
  melee-unlocked also asks GitHub's release API for the asset's digest and refreshes the catalog from its own GitHub;
  here the catalog's pinned URL and SHA-256 are used (melee-unlocked's fallback when the API does not answer), and the
  catalog changes with the fetched tree.
- **Download**: HTTPS through libcurl (the services program; devkitPro's libcurl on the console, TLS through the console's
  ssl service), redirects to https only, a size limit from the known size, continued with a range request after an
  interruption or a cancel (`<name>.part`). Free space is checked before anything is fetched or written (the download plus
  the finished disc).
- **Checks**: the download's SHA-256; the player's disc against the vanilla NTSC 1.02 MD5 (read in full once, then kept
  with the disc's size and time in `mods/.work/vanilla.txt`); the patch's SHA-256 and the finished disc's SHA-256 and MD5
  against the catalog, while the bytes go by. A disc that does not match is deleted and the installed one kept.
- **Archives**: zip (stored or deflated, zip64; zlib's inflate) and 7z (LZMA, LZMA2 or stored folders, encoded headers;
  the LZMA SDK's decoders from the fetched tree's `port/third_party/lzma`), read as streams: the patch is applied straight
  out of the archive, never unpacked to the SD card (ACE's patch alone is 1.3 GB). The first `.xdelta` entry is the
  patch, as melee-unlocked's installer picks it. Entry CRC-32s are checked.
- **Patch**: `src/modinstall/mi_vcdiff.cpp`, a VCDIFF decoder for xdelta3's patches: RFC 3284's code table and address
  cache, source windows, the per-window Adler-32, and xdelta3's secondary compressors DJW (Hack Pack, Melee 3D, Melee
  Maker) and LZMA (Training Mode CE, Akaneia; one xz stream per section kind that runs on across windows). It holds one
  target window (at most 16 MiB from xdelta3; 64 MiB accepted) and reads the source disc through eight cached 1 MiB
  blocks: the 1.4 GB disc is never in memory. (`port/runtime/host/vcdiff.cpp` decodes Slippi's small in-memory patches
  and has no secondary compression.)
- **Result**: `mods/<id>/<id>.iso` (20XX TE: `mods/te/20XXTE-v2d-r4-USA.gci`), which is the mod profile `<id>` that
  `mods.cpp` loads (the folder form: every disc and save in `mods/<id>/`; the TE save is recognised there and switches
  the native TE features on, any other save is copied to the profile's card `card-<id>`). `mods/<id>/installed.txt`
  records the version and the SHA-256. A fetched archive is deleted once its disc is built; a file the player put in
  `mods/Downloads/` stays. Remove deletes the disc or save and the record; the profile's memory card stays.
- A mod disc never goes online: a session that can go online loads no profile (above).
- On the console: Install runs on its own thread while the menu is open or the game runs; one job at a time. Measured on
  the Mac (M-series, NVMe): 13 s for a disc from a small patch, 42 s for ACE. The console's SD card is slower; not yet
  measured there.

| Path | What it is |
|---|---|
| `src/modinstall/mod_install.h/.cpp` | The API: `modinstall::Manager`. |
| `src/modinstall/mi_catalog.*` | The catalog and its rules; the known download and disc sizes (by SHA-256). |
| `src/modinstall/mi_vcdiff.*`, `mi_lzma.*`, `mi_archive.*`, `mi_hash.*`, `mi_io.*` | VCDIFF with DJW and LZMA; LZMA/LZMA2 and xz; zip and 7z streams; SHA-256, MD5, CRC-32, Adler-32; files, `Platform` (free space) and `Fetcher` (HTTPS). Plain C++17 and POSIX file calls. |
| `src/modinstall/mi_fetch_curl.*` | The `Fetcher` over libcurl (services program only). |
| `src/mods_service.*` | The app's `Manager` (its folders, the built-in catalog, libcurl when linked) and the profile at the next start. |
| `src/mu_mods.*` | The Mods screens' view and actions (the home screen's Mods page, the panel's MODS page); replaced the old menu's `mods_page.cpp`. |
| `../tools/mod_install_test.sh` | The host test (macOS or Linux, native): rules, licence gate, free space, an interrupted download, every catalog mod built and compared. |
| `test-mods-linux.sh install` | The Mods page with a controller script: rogueMelee installed, then a match on it. |

**API** (`src/modinstall/mod_install.h`, `src/mods_service.h`), for any screen (the minimal page, or a launcher in
melee-unlocked's look):

```cpp
modinstall::Manager& m = fe::mod_manager();          // the app's installer; plain C++, no Switch code
m.catalog_ok(); m.catalog_error();                   // the built-in catalog passed melee-unlocked's rules
std::vector<modinstall::ModStatus> mods = m.list();  // one per catalog entry, cheap enough for a few calls a second:
//   .mod (CatalogMod: id, name, version, credits, license, page, url, asset_pattern, kind, policy, note,
//         download_bytes, disc_bytes (0: not known)), .state (NotInstalled, Installed, UpdateAvailable, Working),
//   .automatic (Install downloads it), .file_ready (its file is in mods/Downloads), .installed_version,
//   .line (what is happening or how the last job ended), .percent (-1: none), .failed, .downloads_path
m.install(id, &why);   // Install or Update: a job on the installer's thread (false: why it cannot start)
m.cancel();            // stops the running job at the next block; a partial download is kept and continued later
m.remove(id, &why);    // the disc or save and its record; the profile's memory card stays
m.busy(); m.busy_id(); m.profiles();                 // profiles mods.cpp can load (folders and Profiles/*.ini)
fe::mod_profile_next(); fe::mod_profile_set(name);   // settings.ini mod_profile: what the next start loads ("" none)
fe::mod_profile_choices();                           // "" first, then m.profiles()
fe::mod_downloads_available();                       // false in the program without services (no libcurl)
```

Another program (melee-unlocked-macOS) builds the same library with its own `Config`: `mods_dir`, `disc_path`,
`catalog_json`, a `Fetcher` (`modinstall::curl_fetcher()` or its own) and optionally a `Platform` for free space.

**The Mods page** (in-game menu, the last host page, right above Controls; the main page's rows from Controls down are one lower than before): `Mod` picks one of the catalog's mods (Left/Right), then
its state, size and licence, the action (Install / Install from mods/Downloads / Update to ... / Cancel / Play it at the
next start), Remove (press A twice), for a mod that cannot be fetched its page and where to put the file, and `Mod
profile *` (the profile the next start loads). Install and Remove are greyed while the session is online.

### Netplay

`--play --net INDEX:LOCAL_PORT:REMOTE_IP:REMOTE_PORT` (and the runner's other netplay options: `--net-retry`,
`--net-delay FRAMES|auto`, `--track-remap`, `--net-inputs`, `--online ...` in the services program) runs the netplay
host of `../runner/net_host.*` under play mode: picture, the app's controller input and sound, with the engine's own
online code and this repository's Slippi netplay. With `--net` the match is played by the game's generated test
inputs (seed `--net-inputs`, default player index + 1, as the headless runner), not by the controllers; with `--online`
or a search from the menu the controllers play unless `--net-inputs` is given. Such a run
("direct": straight into a match, as the headless runner does) plays the retail game with the runner's seed and clock,
reads no mod profile and of settings.ini only the input delay; the host paces it. Its option words are the host's own,
the same the headless runner gives (`options 00000004`, the rest 0), so it can play against the headless runner.

**Online from the game's menus** (services program, `slippi_menus 1`, a user file): the game opens on its Online Play
page with Slippi's rows (Unranked, Direct, Teams, Party; this game build offers no Ranked row, and a Ranked request is
refused by the netplay host with "Ranked is not available in this build. Play Unranked, Direct or Teams." as the
search error). Picking a row and starting the search on Slippi's character select sends the game's own find-match
command (0xB4) through the netplay host, exactly the path `--online` takes; nothing is asked for at boot. The user
file is `sdmc:/switch/melee-nx-netplay/User/Slippi/user.json` (the app's sign-in) unless `--user-dir DIR` names another
folder; without it the page shows Log-in and the log says why. No mod profile is loaded in such a session. When the
match's scene ends the rollback store stops tracking; the next match starts it again. Log lines: `play: online at
retrace N (scene 8/0): the option words are now the online ones`, `play: online: the game asked for a match: mode 1
(Unranked), from its menu`, the netplay client's `[net] slippi: matchmaking ...`, `opponent found`, `online connection
successful`, then `play: match N starts ... the words the game last read: ...`.

**Option words online** (settings.cpp): while the game is in Slippi's online flow (major scene 8) or a search or match
is on, the words follow upstream's network-session rules whatever settings.ini says: 20XX TE and Hack Pack bits, word 2
(20XX CPUs, frozen Stadium) and word 3 (CPU options, locked content, results screen) are 0; the offline community codes,
the Arcade hitboxes and 2D camera, Boot to CSS and faster sound effects are cleared; the viewer's own display choices
(widescreen, PAL stock icons, language, HUD codes, no shadows, the grid) and muted voices stay, as upstream keeps them.
Upstream sends words 2 and 3 and lets the game ignore them online; here they are not sent. The game itself forces
Slippi's code set in an online match (`mu_general_codes`).

**Input delay**: `onlinedelayauto 1` (default, as the app) lets the netplay object pick 2 to 5 frames from the round
trip at each game's start (`[net] slippi: automatic input delay N frames ...`); `onlinedelayauto 0` with `onlinedelay
1..9` fixes it. `--net-delay FRAMES|auto` overrides the file.

**Decoder thread under netplay.** The decoder keeps its own thread. Before every state save or load (0xB1, 0xB2, and
the self-test's 0xF0, 0xF1) the simulation thread waits until the decoder has read everything handed to it, because
the snapshot store copies, and on the console with `--track-remap` unmaps and maps again, pages the decoder may be
reading. At the start of a frame the retrace has already waited, so this costs nearly nothing. The engine draws
nothing while it re-simulates (measured: 0 stream bytes from repeated frames), so a rollback produces no extra frame:
the frame drawn after it shows the corrected state, and the renderer still shows only the newest finished frame.
`--gfx-sync-net` decodes inside the game's flush instead (the earlier way, for comparison).

**No log writing from the game's threads while it runs** (`src/log_queue.cpp`): in play mode every line (the
frontend's, the host services' including the game's own `[game]` lines, the netplay host's, and the netplay object's
`fprintf(stderr)` through `-Wl,--wrap=fprintf`) is copied into a buffer and a writer thread prints it to the console
(nxlink) ten times a second and to `picture.log` only between matches. `--log-direct` writes where the line is made
(the earlier way). The end of the run has `log: N lines went through the log thread ...`, the netplay host's `net: slow
frame ...` lines and the frontend's own itemisation, `picture: match frames with 8 ms or more of simulation-thread
work ...` and `picture: slow frame N: ...` (stream hand-over, controllers, state saves and loads, the decoder wait
before them, the netplay device, log lines queued, disc reads, stream bytes from repeated frames).

**Process layout**: play mode makes the runner's `fixedmap_startup` check first thing in `main` (the runner's own call
is answered with that result), prints the layout line, restarts the title with `--relaunch` as the runner does, and
ends with `play: cannot start: ...` otherwise.

### Input suggestions (0.4.1)

Slippi 3.7.0 sends each player's newest controller state between frames as an input suggestion (message 0x87), so
the other side can predict that player's next frames from it instead of repeating the last real input; the real pad
still decides every frame and a wrong guess is rolled back as before. This build sends and uses them, by
`../patches/net-0001` on the netplay object's copy of `port/runtime/hle` (`../net_sources.sh`; `port/` is unchanged):

- Using: each received suggestion is kept per player and put into the 0xB0 reply's prediction block for the frames
  after that player's newest real input. Which suggestion applies, and when a player's suggestions are ignored for
  the rest of the game (they keep disagreeing with the real inputs), are Slippi's rules as melee-unlocked has them
  (`slippi_input_suggestions.h`, copied from the fetched tree at build time).
- Sending: at an online game's first frame the netplay object starts a thread that reads the local player's
  controller about a thousand times a second through `mu_net_suggest_read` (play_io.cpp: the app's whole input path,
  never at the same time as the game's own poll, nothing while the in-game menu is open; on the console on core 1 at
  priority 0x27). A change that can start an action (a button, a trigger past the light-shield line, a stick into
  walk, dash, tap-jump, crouch or smash range) goes out at once, unsequenced like a pad, stamped with the pad frame it
  first appears in (`mu_net_suggest_note_poll` marks each poll, so a change read after the game polled belongs to the
  frame after it). A session whose match is played by generated test inputs sends none.
- `input_suggestions 1` (default, as upstream) or the Game page's row; read at each online game's start. The log has
  `[net] slippi: input suggestions: sent and used (controller port N)` at the start and, at the end of each game,
  how many were sent, received and used, players ignored, and the controller reads with their average time.
- The headless runner uses received suggestions but sends none (it reads no controllers). `MU_NET_SUGGESTIONS=0|1`
  overrides the setting in any program (tests).

Test (`test-net-linux.sh` with `MU_NET_CONTROLLERS=1`, the controller scripts play the match instead of generated
inputs, and `MU_SUGGEST_LEAD_MS=12`, each script change is also read 12 ms before the poll that takes it, as a change
between frames would be; the same inputs either way; `--net-delay 2`; per side, re-simulated frames per 1000 match frames):

| Relay delay each way | Suggestions on | Suggestions off |
|---|---|---|
| 40 ms | side a 6 and 0, side b 100 and 308 | side a 6 and 5, side b 312 and 318 |
| 60 ms | side a 247, side b 317 | side a 246, side b 317 |

Every run: confirmed states identical on both sides, `net check` lines equal, no desync message, 0 players ignored;
about 100 to 130 suggestions sent per side per 20 s match, 2 us per controller read. The gain depends on where the
suggestion lands between the receiver's frames: in this test every change comes exactly 12 ms before a poll and the
relay delay is fixed, so one side's suggestions arrive in the useful window in some runs and never in others (side b
in the first 40 ms run lost two thirds of its re-simulated frames; the other runs show no change). With 60 ms each
way a suggestion arrives after the frame it is for has been simulated, so it cannot save a rollback. Not yet tried
against a real Slippi player (real changes come at any time between polls).

### Between two Melee-NX-NetPlay builds (0.4.3)

`../patches/net-0004` adds a build message (id 0xD0) that the netplay object sends first on the reliable channel at every
connection: the Direct pause rule, the shield colour and the port colour choices. Slippi Dolphin and melee-unlocked log an
unknown id and ignore it, and a side that never got one keeps Slippi's behaviour, so nothing changes against them. Since a
side's selections always follow its build message on the same ordered channel, both sides decide the pause rule from the
same bytes before a match can start (a rule only one side applied would split the game at the first Start press). The
headless runner takes the same choices as `--net-direct-pause 0|1`, `--net-port-colour A[,B]`, `--net-shield RRGGBB`,
`--net-skip CODE` and, for tests, `--net-as-slippi` (no build message, the other's ignored). Results: FEATURES.md rows
"Pause in Direct and Friends", "Online port colours", "Opponent shield colour", "Skip in Unranked". 0.4.7 (`../patches/net-0011`): two more flags say
whether a player offers Auto L-cancel (`autolcancel 1`); a Direct match uses it only when every player's build offered it
(melee-unlocked allows it in Direct; here both must agree), on the local player's own input (FEATURES.md "Auto L-cancel"). settings.ini comments
now start at a `#` at the start of a line or after a space, so a connect code keeps its `#`.

### Quick chat (0.4.3)

Slippi's D-pad chat on the online character select works as in Slippi: the game sends 0xBB, the netplay object sends the
message to the opponent, and the receiver's game shows it from the match state (0xB3; message 0x84 on the wire). The
netplay host logs each step (`net: chat: the game sends quick chat message 0x88`, `our message 0x88 went out`, on the
other side `message 0x88 received from player 2 (shown by the game)`). Test: `MU_PADS_A=scripts/online_unranked_chat.txt
MU_TWO_SCREENS=1 ./test-online-linux.sh DISC 3000` (side a presses D-pad Up, Up after the match, back on the character
select and still connected; side b has its own X screen, `shots/b-NNN.png`): side b showed "Mock 2: ggs" (screenshot),
two to five messages per run depending on when the match ended, the match itself identical on both sides.

### Online extras (0.4.4)

As melee-unlocked has them (its `port/runtime/hle/native_practice.cpp`, `slippi_warmup.h`, `host/net_trace*.cpp`,
`gx/net_overlay.cpp`, `abi/mu_pick_preview.h`), in `src/features/search.cpp` and `src/features/netoverlay.cpp`, with the
netplay object's side in `../patches/net-0005` (and `../runner/netplay_bridge.*`). Everything here stays compatible with a
Slippi Dolphin 3.7.0 peer: nothing new goes on the wire to a peer that did not say it reads it, and nothing changes the
simulation of a match.

- **Play while you search** (`play_while_searching`, on). On Slippi's character select with an Unranked search running and
  the fighter locked in, hold L and R (two thirds of a second, no A, Start or Z): the game goes to an offline Training match
  with that fighter while the search goes on. The netplay object skips the game's cleanup commands while the search is kept
  (the main menu sends one), and play mode counts the kept search as offline (the player's own option words, the offline
  tools). A line at the top shows the search time. When an opponent is found: "Opponent found: back to the match in 3", then
  back through the main menu to the select, still locked in, and the match starts as it would have. L and R held in
  Training go back at once. A search that stops while away leaves the player in Training with a note. Log lines
  `search: kept search: ...`, `slippi: kept search: ...`. Upstream keeps Slippi's Unranked only; so does this.
- **Warm-up room** (`warmup`, on). Connected in Direct or Teams and not locked in: the same L and R hold goes to Training while
  the connection stays up. When every other player has locked in: "Opponent is ready", three seconds, then back to the
  select to lock in. A Slippi player just sees this player not locked in yet. Upstream's "warming up" notice to its own
  builds (message 0xE9) is not sent.
- **Training after online** (`online_training_after 1`, `online_return_training 1`, both off as upstream): when a session ends
  after at least one match (the other player left, Z held, the connection dropped) and the player is still on the online
  screens, straight into Training against the last opponent's fighter (Direct and Teams; upstream leaves Unranked out), or
  to Training's character select (every mode).
- **Live pick previews** (`pick_previews`, on). The game's online select sends its pick and reads the others' through
  upstream's command 0xED; the netplay object sends upstream's pick preview bytes as message 0xD1 to a peer whose build
  message (version 2, flag 0x04) says it reads them, so only between Melee-NX-NetPlay builds. The select shows the
  opponent's fighter under "Playing:".
- **Network and timing panel** (`netoverlay 1`) and **session trace** (`session_trace`): see the table below. Each online tick
  is built from the netplay commands play mode hands the modules (`feature_hooks.h` `add_exchange`): the 0xB0 (frame, the
  local pad, ran, halted or advanced), the netplay object's wait or time sync reason and offset, the 0xB2 loads after it and
  the simulation thread's work in the frame. The trace is written after the match by the replay writer, beside the replay.
- **Quick chat** (`quickchat`) reaches the netplay object (Slippi's message ids only, which a Slippi 3.7.0 peer reads).

Tests (`test-online-linux.sh`; the loopback stand-in now keeps a waiting ticket alive with status messages):

| Run | Result |
|---|---|
| `MU_PADS_A=scripts/online_keep_search_a.txt MU_PADS_B=scripts/online_keep_search_b.txt MU_TWO_SCREENS=1 ... 6000` | side a searches, goes to Training at retrace 580 and plays it offline; side b searches 20 s later; found at 1654, side a back on the select at 1858 (search state 4, locked in), the online match identical on both sides, `net check` equal, 0 desync messages; both logs `pick preview from port N`, side b's select shows "Mock 2: Falco" |
| `MU_PADS_A=scripts/online_warmup_a.txt MU_PADS_B=scripts/online_warmup_b.txt MU_ARGS_A="--feature-test unranked-as-direct" MU_TWO_SCREENS=1 ... 7800` | after match 1 side a warms up in Training (Unranked treated as Direct for the test: the stand-in pairs Unranked searches); side b locks in, side a comes back and locks in, match 3 online; `net check` equal, 0 desync messages |
| side a `shield_color_online FF00FF`, `online_training_after 1`, `--feature-test unranked-as-direct`; side b `shield_color_online 00FF00`, `MU_PADS_B=scripts/online_shield_b.txt` | side a's screen shows the opponent's Fox shielding in green (port 2's own is blue), side b's too; after the match side b holds Z, side a goes into Training with Falco against a Fox CPU; `net check` equal |
| `MU_ARGS_B="--net-as-slippi"` (side b behaves as Slippi Dolphin) | no pick preview sent either way, side b logs the build message as unknown, `net check` equal |
| `test-net-linux.sh` with `MU_NET_DELAY=40` and `netoverlay 1`, `session_trace 2` | the panel on screen; 479 rollbacks in the records, depths 1:477, 3:1, 4:1, the same as the netplay host's own count; both sides wrote a 2963-row trace; `net check` equal |

Not ported: upstream's question on leaving the select while searching and its other ways out (only the L+R gesture), the
"search again?" prompt after a session, the arena rooms, early inputs (melee-unlocked peers only; Slippi's input
suggestions are what a Slippi 3.7.0 peer reads) and the P2P mode (melee-unlocked builds only). See FEATURES.md section 3.

### Feature modules: training, replays, options, mod discs, 120 Hz

`src/features/*.cpp` and `../runner/features/*.cpp` register through `src/feature_hooks.h` (commands by
`slippi_command` byte, the host table, a per-retrace call, settings keys, menu rows, overlay lines). Play mode only.

| settings.ini | What it does |
|---|---|
| `lab 1`, `lab_loop 1`, `lab_bubbles 1|2`, `lab_overlay 1` | Training Lab, offline Training: D-pad Right save state, Left load (hot-page store), Down record the dummy, Up play it back; collision bubbles over the model or alone; frame advantage (0xF4) as an overlay line |
| `frame_advance 1` | L+R+Z freezes offline Training, Z steps one frame (held: repeats), D-pad moves the CPU while frozen |
| `practicecpu_on 1`, `practicecpu_preset N`, `practicecpu HEX` | the Practice CPU (command 0xF6); the Training page also has "Go to Training" |
| `practicecpuslot1..3 HEX` | the Practice CPU's three slots (upstream's form, as `practicecpu`): the Training page's "save to slot N" keeps the current settings there, "load slot N" brings them back (the D-pad and lab switches stay as they are) |
| `practice_rng 0|1|2`, `practice_rng_seed 0..255` | Training's random numbers (items, turnips, hammers; upstream's CPU page RNG row, applied by the game's `mu_practice_cpu.c`): the game's own, the same seed at each Training match start and after a state load, or frozen (the same seed every frame); only in offline Training, never online, in a replay or a vanilla session |
| `shield_colorp1..4 RRGGBB`, `offline_delay 1` + `offline_delay_frames N`, `autolcancel 1`, `zelda_as_sheik 1` | offline only (shield colours: online the ports' own) |
| `savereplays 0`, `replay_dir DIR` | every VS and online match is written to `sdmc:/switch/melee-nx-netplay/Replays/Game_YYYYMMDDTHHMMSS.slp` (Linux: `<out>/Replays`) |
| `slippi_spectator 1`, `slippi_spectator_port N` | the Slippi spectator server (off by default; port 51441): every recorded match goes out live over ENet as Slippi Dolphin sends it, for the Slippi Launcher's Broadcast tab and slippi-js overlays (`src/features/spectator.cpp`; on the console on every interface, off it on 127.0.0.1). Test: `test-spectator-linux.sh` (an ENet client, `../tools/spectator_client.cpp`, and `../tools/spectator_check.py`: the stream is the .slp's raw element byte for byte) |
| `netoverlay 1` | during an online match, upstream's network and timing panel at the top right: the last 10 s of frame time and work, waits, time sync halts and advances, rollbacks by depth, ping, marks (`src/features/netoverlay.cpp`, drawn by `src/overlay_draw.cpp`; a build without a picture overlay shows one summary line). See "Online extras" |
| `session_trace 0|1|2` | the session trace beside each online match's replay (`Game_*.trace`, upstream's CSV): 1 (default) only for a match with a mark (D-pad Left/Right/Down during the match) or a 6-frame wait, 2 every match, 0 never |
| `play_while_searching 0`, `warmup 0`, `online_training_after 1`, `online_return_training 1` | play while you search (default on), the warm-up room (default on), Training after an online session (both off by default, as upstream). See "Online extras" |
| `quickchat 0|1|2`, `pick_previews 0` | quick chat on, Direct only, off (upstream's key and values); live pick previews off (default on; between Melee-NX-NetPlay builds only) |
| `compressreplays 1` | replays are written as `.slpz` (the slpz tool's format, compiled from the fetched tree's `port/app/replay_slpz.cpp`; 5 to 18 times smaller), each checked to unpack to the recording first; `--replay` and the Replays page read both (a `.slpz` is unpacked to `Replays/.slpz-playback.slp` to play). Round trip: `../tools/slpz_roundtrip.sh DIR` |
| `subframe 0|1|2`, `fps N` | presentation: Off / Predict / Interpolate sub-frames; `fps` caps presents per second (tests). The simulation stays at 60 Hz |
| `input_suggestions 0` | Slippi 3.7.0 input suggestions off (default on; the Game page's row; from the next online game). See "Input suggestions" |

`--replay FILE|latest|N` plays a replay (the game takes one only at boot; the Replays page's "Watch it" restarts the game
with it). The viewer's controls (upstream's controller layer, `gx/replay_hud.cpp`): Start or Z pause, D-pad Left/Right 5 s
back/forward (paused: Right steps a frame), L speed 1, 1/2, 1/4, R fast forward, B hides the overlay, A takes over the
player named top left (D-pad Up/Down picks another), X/Y jump to that player's last/next coach tip. In a takeover the
controller plays that port; Start retries from the takeover's frame, Start held goes back to watching. The overlay
(`src/replay_view.h`, drawn by `overlay_draw.cpp`) shows a timeline with the tips' markers, the time, the state and the
buttons (it fades while the replay just plays), the newest frame data reading, a tip's banner at its moment and a try's
result; while paused the render loop draws the last frame again so the overlay and the in-game menu keep working. The end
of the run compares the game's post-frame records with the file's (takeover frames are not compared). Jumps simulate the
frames on the way with their draw pass and do not show them (an undrawn frame is not exact with items).

| settings.ini | What it does |
|---|---|
| `replay_try_length 0..3` | a takeover's try: until the combo drops (or 10 s without a hit), 10 s, 20 s, no limit (default) |
| `replay_opponent 0..3` | the other players once the live game leaves the recording (command 0xE9): smart defence (default), no DI or techs, the Practice CPU's settings, follow the recording |
| `replay_tips 0` | no tip banners or timeline markers |
| `replay_sort 0..3`, `replay_hide_short 1` | the Replays page's list: most recent, oldest, longest, shortest first; games under 30 s hidden |

The Replays page (`src/features/replay_library.cpp`): the folder's replays one at a time (characters, stage, length, date),
Watch it, sort, hide short games, the shown replay's stats per player (kills, damage, openings per kill, neutral wins,
counter hits, conversions, L-cancels, inputs per minute, dodges, wavedashes, ledge grabs; upstream's
`launcher_replay_data.cpp` compiled from the fetched tree), Delete this replay (A twice within 5 s), the takeover and tip
settings, and Analyze (while a replay plays: the rest of it run unseen so the game measures every moment, then back).
`test-replays-linux.sh DISC record plain controls tips takeover practice analyze library` runs each with screenshots.
`--frame-mode off|predict|interpolate` and `--fps N` on the command line; `MU_FRAME_MODE`,
`MU_PRESENT_FPS` in the environment for scripted runs.

Presentation (`src/features/present/`, compiled with the renderer, not the module glob; `patches/0106` adds per-draw
matrix overrides to `gx::AuroraBackend`, lets it present a frame again, and lets the sub-frame solver pair
raw-captured draws). With sub-frames the decoder hands over every draw's full state (no deltas), the render
thread runs `present::loop`, measures the refresh from presents back to back at start (`present: display refresh
measured N Hz ...`, or `present: presents do not wait for a refresh ...; paced to the display's reported N Hz`)
and draws each refresh at phase t = time since the newest game frame / 16.67 ms. A rollback load (0xB2), a Lab
load or a replay jump is never blended across. Off mode keeps play mode's loop and repeats each frame
round(refresh / 60) times through its present hook.

Replay fidelity needs `../patches/decomp-0003` in the game image (`../fetch.sh` applies every `decomp-*.patch`; an
image built before it records one uninitialised shield-flag byte of the second fighter that a playback cannot
reproduce). Verification hooks (command line, play mode): `--feature-test training[:CHARACTER[:STAGE]]` (Go to Training from
the title screen; STAGE is the stage select's stage id, default 32), `--feature-test major:N` (ask the game for scene N: Event Match 43, Classic 3, Training 28),
`--feature-test replay-controls|replay-tips|replay-takeover|replay-practice|replay-analyze` (press the viewer's buttons on a
timeline of their own, since a controller script stops while the replay is paused; the takeover tests also press the live
player's buttons), `--feature-test replay-library` (with `--app`: the Replays page opened, its rows logged, a delete and Watch it).

Mod discs beyond 20XX (built on the PC with xdelta3 from each catalog archive's own patch, checked against
`mods-catalog-2.json`): put the disc in `mods/<name>/` and set `mod_profile <name>`. Training Mode CE sets
MU_GAME_OPTION_TMCE offline; an m-ex disc with added fighters (Akaneia, ACE) shows them on the character select;
rogueMelee sets MU_MOD_ROGUE; Melee Maker sets MU_MOD_MAKER and gets its data through command 0xEE (no 32 MB cache:
the game loads its moves into the match heap). Melee 3D loads as files only (upstream: Static Recomp only).
Checked in 0.4.1 on Linux (play mode, controller scripts, screenshots): Melee Maker's BUILD A FIGHTER panel opens with Z
on the character select, X randomises a build (Falco with Samus's Charge Shot, Ness's PK Fire, Puff's Sing, Marth's
Counter) and the match plays those moves; the 20XX Hack Pack's debug menu opens from VS Mode > Tournament Melee, a
Battlefield variant chosen on its TEXTURES page is saved on B and the match loads that variant's file (`pick: stage 31
page 1 -> 31, letter '2'`), and D-pad down and up on the stage select change the page (its own icons on page 2).
Since then (Linux, play mode, controller input through the test pad file): the pack's hidden characters (Z with the coin
held over Bowser: Giga Bowser, in Training and in VS Melee, and a VS match with him against a CPU), its color overlays
(hitstun red on the CPU in Training) and its music playlists (the global playlist set to random custom tracks: Final
Destination plays `AF.hps` / `CE.hps` instead of its single song `30.hps`). In VS stock singles upstream's Squad Strike
took the Z press (it adds the hovered fighter to a squad); `../patches/0005` leaves Z to the pack on its seven paired
icons when the pack is loaded.

### Audio buffer and Use custom music (0.4.7)

`src/features/audio.cpp`, with `patches/0117` (the output's buffer can be set while it plays), `patches/0118` (melee-unlocked's
`jukebox.cpp`, compiled from the fetched tree into `gen/jukebox/`) and `patches/0119` (the host asks play mode whether the game's
music goes through the jukebox). The panel's AUDIO page has the rows.

| settings.ini | |
|---|---|
| `audio_mode 0` (default) | Auto: melee-unlocked's `AudioBufferPolicy` (compiled from the fetched tree). Starts at what this output settled on last time (`<app folder>/audio-auto.txt`; the handheld speaker and the TV each keep their own) or 40 ms, grows 4 ms after each gap during a match (gaps over 60 ms are stalls and are left alone), settles back 1 ms per 10 s without one, never under 10 ms. Log: `audio: buffer Auto for the ...`, `audio: Auto buffer N ms after a gap in the match`, `... ends at N ms (kept for the next session)` |
| `audio_mode 1`, `audio_buffer_ms 5..120` | Low latency: that buffer, fixed (30 when picked from the panel, as upstream) |
| `musicpacks 1` (default 0) | Use custom music, from the next start: the game hands every song to the jukebox (commands 0xD6, 0xD7, 0xD8, 0xEB), which plays the disc's HPS song or a player's file of the same name from `<app folder>/MusicPacks/audio/` (`name.mp3`, `.wav`, `.flac`, `.ogg` or `.hps`; a folder `name/` or `name+other/` of files: a random one each time). `Tracks.txt` (the disc's song names) and `README.txt` are written into the folder at start. M4A/AAC is not decoded (the disc's song plays). A player's file is capped at five minutes on the console (upstream: ten). Off: the game's own music stream, as before |

The music only reaches the sound output, never the game, so a pack plays online too (upstream's packs play locally). Tests:
`../tools/music_decode_test.sh` (tones made with ffmpeg in WAV, MP3, OGG, FLAC, mono WAV: rate, length, pitch and level as
written; M4A refused; the length limit) and `./test-audio-linux.sh DISC auto fixed jukebox packs` (SDL's disk audio driver
records the output: with `packs` a 440 Hz test tone is in every second of the recording, the menu's song from `menu01.mp3`,
Onett's from the playlist folder `onetto+onetto2/`).

### Gecko codes, PhobGCC, the lag test and RVZ discs (0.4.7)

- **Your Gecko codes** (`src/features/gecko.cpp`, `patches/0121`, `0122`): `<app folder>/GeckoCodes.ini` in Dolphin's format; each code
  is a row of GECKO CODES > Your codes, switched there (greyed with melee-unlocked's reason when the Source Port cannot run it),
  kept as `geckocode NAME` lines and `geckochosen 1` in settings.ini. melee-unlocked's user_gecko.cpp decides and plans each code
  and the game image's own `mu_user_gecko_write` / `mu_user_gecko_read` store it, offline only (not online, in replays or a
  vanilla-only session). A code that rewrites a game function is refused here (the console does not let a program rewrite its
  own code). Test: `./test-mods-linux.sh DISC gecko`.
- **PhobGCC settings** (`src/phob_adapter.*`, `src/features/phob.cpp`, `patches/0123`): melee-unlocked's firmware port (host/phob.cpp)
  on the GameCube adapter's raw reports, stepped on the usb:hs reader thread once per millisecond of reports; `PhobGCC/port<N>.ini`;
  CONTROLS > PhobGCC (port, on/off, the firmware's values, Stick calibration, Notch fix, soft reset). Calibration: the steps on
  the overlay, A/L/R records, Z back, X/Y turn a notch, B resets it, Start stops; the game gets a resting controller from that
  port meanwhile. Console only (the Linux build reads no adapter). Test: `../tools/phob_check.sh`.
- **Lossless adapter lag test**: `lagtestnext 1` (VIDEO > Advanced) or `--lag-test` makes the next start the lag test (the switch
  goes back to 0), with an empty `LagTestCard` folder. Test: `./test-overlays-linux.sh DISC lagtest`.
- **RVZ and WIA discs** (`app.cpp` `app_disc_convert`): an .rvz or .wia named by `iso=`, or beside the expected GALE01.iso, is
  checked and written once as GALE01.iso (melee-unlocked's disc_rvz.cpp). Test: `../tools/rvz_check.sh` (upstream's own test).
- **Supersampling** (`ssaa 2..4`, upstream's key; the Picture page): the internal resolution rendered 2 to 4 times per axis and
  scaled down (4x, 9x, 16x SSAA); off by default, heavy on the console's GPU.

### Graphics: texture packs, ambient occlusion, the Lab view, menu videos, captures (0.4.8)

Every option here is display only and off by default, as melee-unlocked ships it; none of it reaches game memory, so the
reference scripts, replays and online play are the same with any of it on. The settings module is `src/features/gfx.cpp`
(settings.ini keys, the menu rows), the renderer side `src/gfx_render.cpp` (render thread, called from `overlay_draw.cpp`), the
shared state `src/gfx_live.h`.

| settings.ini (upstream's keys) | |
|---|---|
| `customtextures 1`, `texpackoff NAME\|NAME`, `dumptextures 1` | Dolphin-format texture packs from `Load/Textures/GALE01` and `TexturePacks` (each folder a pack), dumps as PNG into `Dump/Textures/GALE01` (VIDEO > Texture packs) |
| `ssao 0..1` | screen-space ambient occlusion in the present pass (VIDEO > Picture) |
| `labview 1`, `look_lab_fighters 0..3`, `look_lab_stage 0..3`, `lab_colour_p1..p4 RRGGBB`, `labskipscene 0/1` | melee-unlocked's Lab view; `look 12`, `13`, `14` are Stick Figures, Capsules and Minimal (VIDEO > Lab view, Looks) |
| `videobackgrounds 1` | `VideoBackgrounds/sss.mp4` in place of the stage select's background (VIDEO > Menu video backgrounds) |
| `capture_burst_frames N`, `flickerscan 1` | the capture burst's length (VIDEO > Advanced, "Capture frames": 3 s after the menu closes, into `capture/burst-N/`) and the one-frame glitch detector (`flicker/`, the log's `flicker:` lines) |

| Path | What it is |
|---|---|
| `patches/0124-aurora-texture-packs.patch` | Aurora's Dolphin-format replacement for a host: PNG dumps in Dolphin's layout, a decoded-cache budget, a reload, the Dolphin name of a texture, pointer aliases (a decoded copy found under its source texture's name) |
| `patches/0125-aurora-ssao.patch` | `AuroraPicture.ssao`: the occlusion from the EFB's depth in the resample pass |
| `patches/0126-aurora-frame-readback.patch` | `aurora_set_frame_readback`: the presented game picture read back per channel at a chosen size |
| `patches/0127-flicker-scan-on-the-switch.patch` | melee-unlocked's flicker_scan.cpp off Windows (build-time copy in gen/gfx) |
| `patches/0128-gx-texture-pack-palettes.patch` | gx_aurora.cpp: paletted textures it decodes itself tied to their Dolphin name, and dumped under it |
| `patches/0129-gx-lab-view-skips-the-scene.patch` | the bridge skips the world's draws under a covering Lab view |
| `patches/0130-gx-video-backgrounds.patch` | gx_aurora.cpp: the scene for `src/video_bg.cpp`, the stage select's learned background texture shown as the video |
| `src/lab.cpp`, `src/lab_shim/` | melee-unlocked's lab_view.cpp compiled from the fetched tree over small stand-ins; the game image's look snapshot; the silhouette packs' download (services program, SHA-256 checked) |
| `src/video_bg.cpp`, `src/video_bg.h` | the menu video backgrounds over melee-unlocked's learning rules and its linux_media.cpp (FFmpeg) |
| `src/modlooks.cpp`, `src/looks_fetch_curl.cpp`, `test-modlooks-linux.sh` | Get mod looks (ported from the macOS build): Diet Melee and Animelee built from the player's disc as mod profiles; the Diet and Crystal presets use them (mods.cpp) |
| `src/mod_fingerprint.cpp` | a mod disc's content fingerprint as melee-unlocked computes it (the 2D-style m-ex build's recognition, mods.cpp) |
| `test-gfx-linux.sh` | the cases below, with screenshots in `build/native-engine-gfx/run-gfx/<case>/shots` |

FFmpeg: the console build links devkitPro's FFmpeg portlib; the Linux test build needs `./build.sh ffmpeg` once (FFmpeg n7.1, fetched
by `../fetch.sh` from FFmpeg's own repository, LGPL-2.1-or-later as configured there); without it the Linux build simply has no menu
videos.

```sh
./test-gfx-linux.sh DISC textures textures-same ssao capture flicker lab-stick lab-capsules lab-minimal lab-blobs video
MU_LAB_ZIP=/path/to/MeleeUnlocked-Lab.zip ./test-gfx-linux.sh DISC lab lab-download   # melee-unlocked's content release zip
MU_MODS_DIR=/path/to/mods ./test-gfx-linux.sh DISC fingerprint                          # the Akaneia disc: "recognized as Akaneia 1.0.1"
./test-app-linux.sh DISC rvz                                                             # the RVZ conversion's progress screen
```

On the console: put a pack folder in `sdmc:/switch/melee-nx-netplay/TexturePacks/`, switch VIDEO > Texture packs > Custom
textures on and check a match; turn VIDEO > Lab view on in a match (the first time it downloads the silhouettes, about 85 MB, in the
app); put an MP4 (at most 1280x720) as `VideoBackgrounds/sss.mp4`, switch Menu video backgrounds on, and visit the stage select for a
few seconds; VIDEO > Advanced > Capture frames, close the menu, and look in `capture/`. Frame times with each on are what to note:
none of these has been measured on the console's GPU.

## melee-unlocked's interface

The app's in-game menu and its screens before the game are melee-unlocked's own (v1.0.5), as faithfully as the console
allows. Nothing of it is committed: `build.sh sync` runs `scripts/mu_ui_extract.py` over the fetched tree, which copies the
panel's code verbatim into `gen/mu_ui` (`pc_settings_visuals.inl`, `settings_page_navigation.h`, `settings_imgui_input.h`,
`settings_search.h`, the GD Melee motion and text tables, and by first and last line the Clean side functions and blocks of
`pc_settings.cpp`: category icons, title shards, category list, slide-in motion, page header, page entrance, smooth scroll,
the ImGui style, and each menu style's home and navigation: settings_dashboard_home, settings_gd_home and settings_gd_chrome,
settings_radial_nav, settings_wide_home, settings_nav_strip, settings_nav_tile; radial_navigation.h), the launcher's palette and
layout constants and its accent theme, its fonts (Source Sans 3 Black and Bold, OFL), its artwork (`bailey_cut.bmp`,
`melee_mark.bmp`), the GD Melee kit's pictures and font pages (PNG, decoded at first use), the launcher's `lang/*.txt`, the
easter egg's `walter_egg.cpp` (gen/mu_ui/egg, over two stand-ins in src/egg_shim), and converts `melee_unlocked.ico` into the
NRO's icon. whats-new.txt (this repository's) becomes the home screen's What's new.

- **Menu styles** (feat/ui3): CUSTOMIZE > Menu style picks Clean side (the default), Icon tiles, GD Melee, Radial, Wide tabs or
  Simple, each drawn by upstream's code and settings_frame's steps for it; four palettes each, Window transparency, a custom
  accent (hue, saturation, brightness), and the two legacy menus (Legacy New, Legacy Old) from the home screen's Settings with
  "Enable Legacy Menu". launcher.ini keys with upstream's names: `overlaystyle`, `overlaypalette0..6`, `settingstransparency`,
  `menucustom`, `menuhue`, `menusat`, `menuval`, `legacymenu`, `legacymenustyle`, `menusounds`, `settingsreminder`,
  `skin_online_notice`. Every page starts with "Search all settings": the console's software keyboard (src/software_keyboard.cpp;
  off the console the test answers of `MU_TEST_KEYBOARD="words|more|-"`) feeds upstream's search. Menu sounds are mixed into the
  game's output (play_io.cpp). The easter egg (src/egg.cpp) draws over any character select.
- **In the game** (`src/mu_panel.cpp`): the panel in the chosen style. L + R + Start (half a second) or
  Start + D-pad Down + Z opens it; D-pad or stick moves, A selects, B goes back a page, Start closes; Quit game asks first.
  Categories VIDEO, AUDIO, GAME, CONTROLS, OVERLAYS, CUSTOMIZE, GECKO CODES, MODS: the app's rows are placed where melee-unlocked
  has them (its section names), each drawn by its own widgets (toggle, combo, slider; a value the app's pages step is a
  `< value >` field), and melee-unlocked's rows the Switch lacks are listed greyed under "Not on the Switch". Every change is
  queued and applied on the simulation thread at the next controller read; the game gets neutral controllers while the panel
  is open and nothing held when it closes (play_io.cpp's hand-over). The mouse is left out (the search is the software
  keyboard's, the styles above). `menu_open_extra(title)` opens the category holding that page, and the focus walks down to
  it as a player's D-pad would. Practice menu: a "Practice menu" row on GAME > Training when features/proai.cpp is linked.
  Log lines: `settings: opened on ...`, `settings: <CATEGORY>`, `settings: focus <row>` (the tests follow these).
- **Before the game** (`src/mu_home.cpp`): the launcher's rail (mark, Play, Settings, Replay Viewer, Mods, Bailey) and content
  column at its 720 x 480 layout scaled to the screen (1.5 at 720p, 2.25 at 1080p). Play: the disc, GAME BUILD (Source Port or
  Vanilla), PLAY, the version line with the build computer's update ("Update and restart": download, check, swap, then the app
  closes), the Slippi account with Sign in (the app's sign-in screen, `switch_signin.cpp`), the mod profile, the launcher colour.
  Settings opens the panel over it. Replay Viewer: the replays with the picked game's stats, Watch (the game starts with it,
  offline), Delete. Mods: melee-unlocked's catalog cards over the mod installer (Get / Update / Cancel / Play it at the next
  start / Remove, where to download a file yourself) and the profile the next start loads. launcher.ini `home=0` skips it
  (test aid); `mu_palette` and `mu_accent` keep the two colours (`overlaypalette0` and `accent=#RRGGBB` take over once set).
  feat/ui3: the Language button (upstream's nine languages, launcher.ini `language`; CJK through the console's shared fonts), What's
  new (`notesseen`), the colour wheel, `rail-image.png`/`.bmp` in the app folder, "Save crash report" (Logs/melee_crash_report.zip and
  .md, scrubbed as upstream does, never sent). Replay Viewer: Watch, As played, the watch queue, Delete (to Replays/.trash) and Undo,
  Search, Folder... and Open replay... (an SD card browser), Compress all (.slpz), the session trace graph. Mods: each card's skin
  count and Choose skins; the catalog refreshed from upstream's raw URL at each start (`MU_NO_CATALOG_REFRESH=1` skips it).
  Test aids: `MELEE_TRACE_SLIPPI_CMDS=1` (play mode logs each menu command), `MELEE_NET_LAG_MS`, `MELEE_NET_JITTER_MS`,
  `MELEE_NET_LOSS_PCT` (patches/net-0012). test-app-linux.sh cases `styles`, `launcher`, `replaylib`, `crashreport`, `egg`.

## The app: Melee-NX-NetPlay 0.4.2

`out/melee-nx-netplay.nro` (the services program, NACP "Melee-NX-NetPlay" with the version in `VERSION`, 0.4.2, the app's icon: `build/icon.jpg` when
the app's build drew one, else `switch/icon.jpg`) replaces the recompiled app. Started with no arguments (the Homebrew
Menu, a forwarder that adds only `--relaunch`) it is the app; with arguments it is the test program described above.
Off the console `--app [--base DIR]` gives the same (default folder `./melee-nx-netplay`).

**From launch to an online match**: the update prompt when the build computer has a newer build; the Slippi sign-in
screen when `User/Slippi/user.json` is missing (A on the rows uses the console's keyboard; B or Play offline starts
offline; + quits); then the game boots to Slippi's Online Play page (Unranked, Direct, Teams, Party). Picking a row
and starting the search on Slippi's character select finds a match through the app's matchmaking client; the
player's controllers (GameCube adapter, Pro Controller, Joy-Con) play it, rumble included. L + R + Start held for
half a second opens the in-game menu at any time; with `hud=1` the HUD line shows frame time, late frames and, online,
the input delay and ping.

**Folders** (`sdmc:/switch/melee-nx-netplay`, the earlier app's): `GALE01.iso` (or `iso=`), `User/Slippi` (sign-in),
`User/GC/CardA` (memory card), `Replays`, `Cache` (Aurora's pipelines, shared with the earlier app: the same renderer),
`Logs/latest.log` (the one before: `previous.log`; `host.log`; `digest-net-confirmed.csv`), `launcher.ini`, `settings.ini`.
The memory card is the earlier app's `User/GC/CardA`: both engines keep slot A as a Dolphin-style folder of GCI files
and read every `.gci` in it, the native host keeps a loaded file's name when it saves, and the save data is the same
game's. So names, rules, records and unlocks carry over. The first session copies the folder once to
`User/GC/CardA-before-0.4` before anything writes to it, so 0.3.x can always go back to the card as it was. From 0.4.1
every start also backs the card up when it changed since the newest backup (upstream's automatic save backups):
`User/GC/Backups/CardA-YYYYMMDD-HHMMSS`, the 10 newest kept and, older than those, the newest of each day for 30 days
(`app: memory card backed up to ...`, or `app: memory card unchanged since the backup ...`).

**launcher.ini keys honoured**: `iso`, `volume` (default 100), `hud`, `online_delay` (`auto` or 1..9; wins over
settings.ini), `offline=1` (no online services, no sign-in, no update check), `widescreen` (16:9 code when settings.ini
does not set `widescreen`; default 1 as in the app), `filter` (live), `msaa`, `anisotropy` (at start), `trigger_range`,
`stick_range` (console), `update_host`, `overclock=0`, `shader_warmup=0`, `shader_warmup_all=1`, `script`, `frames`,
`menu_open`, `menu_page` (the app's test aids). New: `pad.<SDL GUID>=<port>;<map>` and `keyboard=<map>` (the menu's
Controls page writes them, `input_config.h` format); `padx.<SDL GUID>=` / `padx.gc<port>=` (the controller setup pages, see "Controller setup"). Not used here: `scale`, `render_skip`, `draw_dump`, the app's
diagnosis keys. Every `settings.ini` key above is honoured; the app's default for `slippi_menus` is 1.

**In-game menu** (the app's, `patches/0104`): Texture filtering \*, Aspect ratio \*, Screen filter, Antialiasing \*,
Volume, Online input delay \*, Light shield range, GameCube stick range, then the pages *Game options* (results screen,
everything unlocked, language, music, mute voices, faster sound effects, frozen Stadium and Final Destination, retail
game, Slippi menus), *Picture and HUD* (PAL stock icons, screen shake, HUD sizes, player tags, KO explosions, HUD
emblems, hitboxes, 2D camera, grid), *Community codes* (each bit), *Computer players* (20XX CPUs and the CPU options),
the feature modules' pages, Controls (each SDL controller: remap, deadzones, trigger press point, swap sticks,
rumble, test rumble), Performance HUD, Resume, Quit. Changes are written to `settings.ini` (keeping its comments) or
`launcher.ini` at once; controller pages when the menu closes. Rows upstream only allows offline are greyed while the
session is online ("Locked during online play"). \* = at the next start. The online input delay applies from the next
game (`netplay_bridge::set_delay`). The menu is driven with the stick or the D-pad. Hand-over to the game: the menu's
L + R + Start never reaches the game (held, it left the game ignoring A and Start afterwards: seen on the console,
reproduced on Linux without the menu opening), and whatever is held when the menu closes (the B that closed it) is
kept from the game until it is let go; everything else goes through. The game keeps running behind the menu, as in
the earlier app. The first ten seconds after a close log what the game gets (`menu: the game gets ...`). Quit stops the game cleanly and closes the application
(HOME Menu), as the app's Quit does; returning to the Homebrew Menu is not offered because the graphics stack leaves
threads behind in the process (see Run).

**Launch reliability**: when the address layout cannot hold the game (about one launch in 70, `fixedmap.h`) the app
restarts itself if `--relaunch` was given (a forwarder), otherwise it shows "PLEASE START THE APP AGAIN" with the reason
(services program; the other program logs it). A missing disc shows "DISC NOT FOUND" with the path.

**Self-update**: as the app: `update_host=http://address:port` in launcher.ini, the same `latest.json` (schema 1, the
`game` entry), prompt (A install, B later, + quit), download beside the NRO, size and SHA-256 check, rename into place,
`.bak` kept. The target is the NRO the app was started from (`argv[0]`), else `sdmc:/switch/melee-nx-netplay/melee-nx-netplay.nro`.
This program has no RomFS holding its file open, so the swap happens right after the download; a swap left waiting
is finished at the next start. The comparison is `VERSION` (0.4.2) / Slippi `3.7.0` (the netplay object's pin).

**Feature modules**: the other modules plug in through `src/feature_hooks.h` (their interface): play mode calls
`dispatch_tables` first in its tables hook, `dispatch_command` first in `mu_frontend_command`, `dispatch_retrace` each
retrace, `dispatch_setting` for unknown settings.ini keys, `dispatch_run_end`, `menu_entries` (menu pages) and
`hud_lines` (drawn centred near the top in every play session: Training's frame advantage, frame advance, the practice
CPU), `take_restart_request` (the Replays page's "Watch it": the game is started again in this process with `--replay
FILE`, offline). A menu row's `persist` (feature_hooks.h) writes its settings.ini lines after a change. The modules
are linked by default; `MU_FE_FEATURES=OFF ./build.sh ...` builds without them (the calls are weak, `feature_calls.h`).

**Presentation** has one path: the render loop presents each finished game frame, and the presenter
(`src/features/present`, `patches/0106`) either repeats it to the display's rate (`subframe 0`, through
`fe::set_present_hook`) or, with sub-frames on (`subframe 1` Predict, `subframe 2` Interpolate), replaces the loop with
its own (`present::loop`), which draws in-between poses at the measured refresh rate; the decoder then hands it every
draw's full state. `fps N` caps the presented rate. The simulation stays at 60 Hz. `fe::display_refresh_hz()` is SDL's
display mode. Scripted runs: `MU_FRAME_MODE=predict|interpolate` (test-linux.sh passes it).

### Publishing (the build computer)

`app-build.sh` builds this NRO into `dist/sd/switch/melee-nx-netplay/melee-nx-netplay.nro`. What it needs beyond
the current builder: Docker images `mu-arm-gcc15` (built by the script from `../linux-gcc15.Dockerfile` if missing) and
`melee-nx-netplay-build`; `build/sdk` and `build/switch-aurora/_deps` (both left by one `install.sh` run); one network
fetch of melee-unlocked v1.0.5 (`../fetch.sh`, pinned to `67868aa`; `MU_SOURCE=file://...` works offline); then
`../build.sh game`, `../build.sh linux`, `../build.sh nro`, `MU_NET_SERVICES=1 ../build.sh nro`, `./build.sh sync`,
`./build.sh nro`. `tools/update-host.py` publishes only the `game` entry with `MELEE_NX_UPDATE_NROS=game` and takes
the version from this `VERSION` with `MELEE_NX_UPDATE_VERSION_FILE=experiments/native-engine/frontend/VERSION`; with
`MELEE_NX_UPDATE_BUILD=<checkout>/experiments/native-engine/frontend/app-build.sh` these three `Environment=` lines go
in a drop-in for `builder/systemd/melee-nx-update.service` (`systemctl --user edit melee-nx-update`). Without them the
script publishes the recompiled app as before. 0.3.x consoles then offer 0.4.x and install it in place of their NRO.

### A HOME Menu icon (forwarder)

Not built here. A forwarder is an NSP whose program is nx-hbloader set to load one fixed NRO path (with arguments it
passes along, for this app `--relaunch`); tools that make them: hacBrewPack (packs the NCA/NSP), NTON and Nro2Nsp on a
computer, Switch-NSP-Forwarder and sphaira on the console. All need the console's own keys (`prod.keys`, from
Lockpick_RCM: `header_key`, `key_area_key_application_*`), the NSP is installed to the console (a title install), and it
only boots under custom firmware with signature patches, since it cannot carry Nintendo's signature. The path is fixed
when it is built. Unverified here: how each tool passes extra arguments.

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

Mods, Slippi menus and netplay on Linux (runner and patches of commit `8428269`, game image `6be32fe9...`):

| Check | Result |
|---|---|
| `mod_profile hackpack` (20XX Hack Pack 5.0.2 image) | 786 files differ, 59 replaced and 717 added, its menu and code files not served; the game logs the pack's music table, `music 34 -> 01.hps`, `stage swap table: 1488 bytes`, `pick: stage 9 page 1`, `match on stage 9: state 00008000`; screenshots show the pack's stage select (its page indicator and legal-stage borders) and a match on its files; the second start uses the kept comparison |
| `mod_profile te` (the 20XX TE save) | recognised; `play: options 00000010`; the game logs `[20xx] options 00000010`; Tournament Melee opens the native 20XX TE SETTINGS screen (screenshot, drawn through the lines-and-points patch); two switches set there arrive through command 0xF8 (`te_options D0` in `settings.ini.state`) |
| `mod_profile hackpack,te` | both load; TE's bit and the pack's files together |
| `slippi_menus 1` | 10 files replaced by Slippi's copies, 2 added; `play: options 00000008`; the game starts on the Online Play page (screenshot: 1-P Mode > Online Play, "Log-in") |
| Netplay: two `--play --net` instances in one container over loopback, each with a picture and its own controller script | 2670 match frames in common, no differing shown state, final state identical on both sides, 6 and 12 rollbacks, no desync message; `test-net-linux.sh` |
| Cost of options (`--profile`, match frames, unpaced): default 0.504 ms; widescreen 0.504; hitboxes 0.521; 20XX CPUs 0.540; HUD options 0.513; all of the console test's 0.543 | the options are not what makes a play-mode match frame cost more than the scripted run's (0.38 ms here): that is the match itself (two fighters fighting, one of them a CPU, against two that stand still) and upstream's built-in code set |
| Scripted mode after these changes | digest identical, with and without a picture |

Online play mode on Linux (runner and patches from the working tree after `1c556fc`, with `--net-delay auto`; game image
`6be32fe9...`), all in one container with no network:

| Check | Result |
|---|---|
| `test-online-linux.sh`: two `mu-native-gfx-linux-services` instances, `slippi_menus 1`, made-up users, each driven by a controller script through the game's menus (Online Play > Unranked, Falco / Fox, START), the loopback stand-in for matchmaking | the Online Play page shows Unranked, Direct, Teams, Party (screenshot); both send 0xB4 mode 1 from the menu; the stand-in matches them; they connect and play a match to its end and return to Slippi's character select; every shown state of the match scene identical on both sides, `net check` lines equal, 0 desync messages; automatic delay chose 2 frames |
| Same, side a with 18 settings lines (widescreen, PAL icons, HUD fade, no shadows, free pause camera, Arcade hitboxes and camera, grid, 20XX CPUs, CPU options, frozen Stadium, faster sound effects, muted voices, TE bits, fixed delay 3) | offline words `1000000A 01800000 40001003`, display `01801360`, audio 3; at match start online `1000000A 00000000 00000000`, display `01001100`, audio 2 (offline codes, hitboxes, 2D camera, words 2 and 3, faster sound effects cleared; widescreen, PAL icons, HUD codes, grid, muted voices kept); side b `00000008`, the rest 0; the match stays identical on both sides with different display words; delay 3 against automatic 2 |
| Words of a direct run (`--net`, what the headless runner gives) | `options 00000004`, everything else 0 |
| `test-net-linux.sh` with 60 ms delay and 40 ms jitter, decoder thread | 316 and 230 rollbacks (up to 7 frames deep), 2057 and 1638 decoder catch-ups before a save or load costing 0.3 ms in all, 0 stream bytes from repeated frames; `net check` equal, final state identical; simulation thread 0.70 ms per match frame plus 0.30 ms waiting for the decoder at the retrace; rollback frames 0.96 ms |
| Same with `--gfx-sync-net` (the earlier way) | 272 and 216 rollbacks, `net check` equal; simulation thread 1.47 ms per match frame (0.78 of it decoding) |
| Both runs: 6 to 7 shown states differ near match frame 30, right after the first pipeline compile stalled the picture for 0.7 s | the same in both decoding modes and the `net check` lines are equal, so these are predictions the confirmed-state bookkeeping did not mark (a 7-frame rollback after the stall), not a difference of the game state |
| Logs | 238 to 259 lines per online run went through the log thread, none dropped, nothing written to the file during the match; the netplay host measures 0.000 ms of log writing per frame |
| Services link fix (`netplay_link.sh`), Linux services program | `services check: libcurl 8.14.1 with OpenSSL/3.5.9: global set-up and a request handle OK`; the app's report path runs its `call_once` and curl (`[net] slippi http: GET ... failed: Could not resolve host`, no network in the container), the matchmaking client likewise; matched, played, identical. Console ELF: `ensure_global`'s lambda now branches to `curl_global_init` (it was a `nop` and a zero word); a copy weakened the old way is refused by the check |
| `test-net-linux.sh` without delay, scripted `parity_vs_onett` with a picture (9000 retraces) after all of this | final state identical; digest IDENTICAL |

The app on Linux (`test-app-linux.sh`, the Linux services program, runner `d53f7e7`, game image `6be32fe9...`, feature
modules not linked: `MU_FE_FEATURES=OFF`), one container each, loopback only:

| Case | Result |
|---|---|
| `boot`: no arguments at all, a made-up user.json | the app's folder made (User/GC/CardA, User/Slippi, Replays, Cache, Logs), launcher.ini read (iso, frames), settings.ini template written, card backup line, services check, online on; the game boots to Slippi's Online Play page in 16:9 (screenshot) |
| `menu`: a controller script through the app's input code holds L + R + Start on the main menu | the menu opens (screenshot: the console's rows, v0.4.0); Volume 100 -> 70 and Performance HUD on are in launcher.ini (`volume=70`, `hud=1`), Music 100 -> 80 on Game options is in settings.ini (`music 80`, replacing its template line); Start closes it and the HUD line shows (screenshot); a second start reads them back (`launcher.ini: ... hud=1 ... volume=70`, `music 80%`) |
| `signin`: no user.json | `the sign-in screen comes first`; the app's sign-in screen (screenshot); B through the test pad file: `sign-in: playing offline`, `online is off for this session`, the game boots with Log-in on Online Play (screenshot). Nothing is contacted |
| `update`: a fake build computer (`python3 -m http.server` on 127.0.0.1) with latest.json 0.4.1 and a 3 MB file | the prompt (screenshot), A: downloaded, size and SHA-256 checked, swapped in (the installed file now has the published hash, `.bak` holds the dummy), "UPDATE INSTALLED" (screenshot), A: the app closes before the game; with 0.4.0 published: no prompt, the game starts |
| `online`: two apps, made-up users, the matchmaking stand-in, each player's controller script (no `--net-inputs`, no generated inputs) through Online Play > Unranked | both search from the menu, matched, connected, played the match; confirmed states of the match scene identical on both sides (final frame IDENTICAL, 0 or 1 shown frame differing as in the earlier runs); 0 desync messages; the HUD in the match shows `sim`, `late 0`, `delay 2` (screenshot) |
| After all of this: scripted `parity_vs_onett`, 9000 retraces, no graphics device and with a picture | digest IDENTICAL |
| `test-online-linux.sh`, `test-net-linux.sh` | as before: final state identical on both sides |
| NRO (`./build.sh nro`) | `melee-nx-netplay.nro` links; NACP name Melee-NX-NetPlay, version 0.4.0 |
| After the first console session, with the feature modules linked (runner from the working tree: `netplay_bridge` ping and delay calls) | `handover`: the D-pad moves the menu and changes the volume (`volume=70`), B held 12 retraces closes it and is kept from the game (`closed with buttons 0200 held`), A then opens Unranked (scene 8 at retrace 569); before the fix the B reached the game and the later A was ignored. L + R + Start held 20 retraces without the menu opening left the game ignoring A too (the cause; now kept from the game). `menu`: also Training's "Inputs and frame advantage", saved as `lab_overlay 1` (a module's row through `persist`). `online`: ping on the HUD; confirmed states identical. Scripted `parity_vs_onett` IDENTICAL without a picture and with one, sub-frames off, predict and interpolate. `test-online-linux.sh`, `test-net-linux.sh`: final state identical |


0.4.1 on Linux (runner as of `5a4b4a8`, unchanged; game image `1ddc3690...` with `../patches/decomp-0003`; netplay object
with `../patches/net-0001`), one container per run, no network:

| Check | Result |
|---|---|
| The five reference scripts, 9000 retraces: headless runner, `mu-native-gfx-null`, and with a picture in sub-frame modes off, predict and interpolate | all 25 runs IDENTICAL to the x86-64 references |
| `../tools/net_test.sh 3600 60 40` (headless, suggestions used, none sent) | 580 rollbacks per side, every confirmed state identical, `net check` equal |
| `test-net-linux.sh` (generated inputs), `test-online-linux.sh` | final state identical on both sides, 0 desync messages |
| Input suggestions (`test-net-linux.sh`, controller scripts, 12 ms lead) | sent, received and used; states identical; numbers in "Input suggestions" |
| `test-app-linux.sh boot menu handover signin update online` | all pass as before; `update` offers the version after `VERSION` (0.4.2) and installs it, then no prompt for 0.4.1; the second start of `menu` backs the card up (`User/GC/Backups/CardA-...`, 1 save file); `online`: suggestions sent and used through the app's whole input path (5 us per read), confirmed states identical, 0 desync messages |
| `community_codes 100000` (full FoD reflections) | `play: options ... display 00100000` |
| Melee Maker, 20XX Hack Pack debug menu and stage variants | see "Mod discs" under Feature modules |
| NRO | `melee-nx-netplay.nro` links, NACP version 0.4.1; both programs pass the `netplay_link` check

Overlays and picture options on Linux (`test-overlays-linux.sh`, `test-app-linux.sh online overlaymenu`; screenshots
looked at): input display with values for two ports, frame counter and graph, the adapter line, combo counter ("2 HITS"
to "10 HITS"), L-cancel rings (3 made, 5 missed in `match`), Training panel with "Frame advantage -6 on hit", 20XX TE's
input display bit alone, latency test black / white / picture again (retraces 1061, 1241, 1421), curved television with
brightness, contrast, vibrance, sharpening, 73:60 and the grid border, studio monitor with a .bmp border, internal
resolution 640x480 and 1280x960 (no cropping), names over the fighters online (`net check` equal, final state identical),
the menu's Overlays and Picture pages writing `inputoverlay 1` and `scale 1`. AddressSanitizer with alloc/dealloc mismatch
on (`MU_SANITIZE=address test-app-linux.sh menu`): no report. After these changes the five reference scripts with
`mu-native-gfx-null` and `parity_vs_onett` with a picture: IDENTICAL. Not seen: any of it on the console (NVK, the adapter's rate).

Online and replay features of 0.4.3 on Linux (runner and patches from the working tree, `MU_SYNC_REF=worktree`; game image
`1ddc3690...`), one container per run, no network:

| Check | Result |
|---|---|
| The five reference scripts, headless; `parity_vs_onett` with `mu-native-gfx-null` | all IDENTICAL to the x86-64 references |
| Time sync frame rate (`../tools/net_test.sh 3600 30 0`, one clock 0.4 % fast) | nudged to x0.998 and x1.001, no frame advance after the start (7 with `--net-fixed-rate`); confirmed states identical, `net check` equal; play mode the same (`test-net-linux.sh`) |
| `.slpz` (`../tools/slpz_roundtrip.sh`, `compressreplays 1`) | 11 replays round-trip byte-identical; a play-mode recording written as .slpz plays back with 0 of 3302 post-frame records differing |
| Spectator server (`test-spectator-linux.sh`) | an ENet client got the match from cursor 0, at the start and 25 s late: byte-identical to the .slp's raw element |
| Quick chat (`test-online-linux.sh`, `scripts/online_unranked_chat.txt`, `MU_TWO_SCREENS=1`) | 0x88 sent after the match, received and shown ("Mock 2: ggs") on the other side |
| Build message (`net_test.sh`, `test-online-linux.sh`) | Direct pause rule agreed on both sides, Slippi's rule kept against a side acting as Slippi Dolphin; port colours on both screens; `net check` equal in every run |
| Skip in Unranked (`blockcode1`) | three matches with the listed code turned down, no match played |
| Network overlay (`netoverlay 1`, `test-net-linux.sh` with delay) | the line on screen during the match |
| NROs (`../build.sh nro`, `MU_NET_SERVICES=1 ../build.sh nro`, `./build.sh nro`) | link; both programs pass the `netplay_link` check |

Replay viewer, takeover, coach and library on Linux (`test-replays-linux.sh`, a 37 s offline match recorded by play mode
with the controller script, Falco against a level 1 Fox on Onett, items on; screenshots in
`build/native-engine-gfx/run-replays/<case>/shots/` looked at):

| Case | Result |
|---|---|
| `plain` | 4500 post-frame records compared with the file, 0 differ |
| `controls` | pause, two frame steps, play, 5 s back (from the kept state at -122), 5 s forward, 1/2, 1/4, 1x, fast forward on and off, overlay hidden and shown: 0 of 4794 differ |
| `tips` | 30 events read from the file in 0 ms, 4 tips for P1 and 9 for P2; next, next, next and last tip jumps land a second before each; the game's own readings logged (`coach:` lines); 0 differ |
| `takeover` | P1 taken over at frame 400 with the test's jabs, "P2 left the recording at frame 416" and defended (setting 0), Start retried, held Start back to watching: 0 of 3880 compared records differ, 914 takeover records not compared |
| `practice` | tip 1 jumped to, taken over there, `replay_try_length 0`: two tries ended (no hit in 10 s) and started again with the result shown, back to watching: 0 differ |
| `analyze` | the rest of the replay run unseen (31 events) and back to frame 301: 0 differ. Before jumps kept the draw pass, this pass differed from frame 1866 on (undrawn frames with items) |
| `library` | the app's Replays page opened on its own: both copies' stats, one deleted with A twice, Watch it restarted the game with the other: it played with 0 of 4500 differing |

After these changes: the five reference scripts headless IDENTICAL; `test-net-linux.sh` and `test-online-linux.sh` final
state IDENTICAL on both sides and `net check` lines equal. Not seen: any of it on the console (controller feel of the
viewer's buttons, the overlay at 1080p and on the handheld screen, the stats worker on the SD card's speed).

Not verified: anything on the console (the app's screens on NVK, the sign-in's keyboard and a real sign-in, the update
over the home network, the layout screen and the forwarder restart, the Controls page with real controllers, rumble
of SDL controllers, the shared pipeline cache, the card carried over from 0.3.x); a real Slippi match from the app;
the Replays page's "Watch it" restart (wired, not run: no replay was recorded in these tests); Training's lines on
screen (drawn only in Training mode, not looked at); 120 Hz on a display that has it.

Not looked at: the Hack Pack's random variants (its debug menu, a chosen variant and the stage select pages were checked
in 0.4.1; hidden characters, training displays and playlists since, see "Mod discs" under Feature modules); TE's features in a match; whether upstream's Hack Pack support (which its own notes call "written but never run")
holds beyond what is listed.

Mods and Training session options on Linux (`test-mods-linux.sh`, screenshots looked at): `training-menu` sets the
Practice CPU, Random numbers "Same each start" and seed number 3 on the menu's Training page, saves slot 1, turns the
Practice CPU off and loads slot 1 back (`practicecpuslot1`, `practice_rng 1`, `practice_rng_seed 3` written); the next
start's Training match ran the slot's settings and seeded the game's random numbers (`[pc] rng seed DAA66D14 (same each
start)`); `frozen`: `[pc] rng seed 8FF3473A (frozen)`. `vanilla-only` with `mod_profile hackpack` and CPU options: the
profile not loaded, option words 0, a VS match saved to `card-play/VanillaOnly`; `vanilla-refuse`: a disc header that
is not GALE01 revision 2 ends the start with code 2. `rogue`: Rogue Mode's map and first fight. `mex`: the Akaneia and
ACE discs' added stages built and shown on the stage select; Delfino Plaza picked on the Akaneia disc's second page and
a Training match played on it. With a
`practice_rng 2`, `practicecpu_on 1` settings file on both sides: `test-net-linux.sh` and `test-online-linux.sh` final
state IDENTICAL, `net check` lines equal, 0 desync messages; `parity_vs_onett` with a picture (`test-linux.sh`) and the five reference scripts headless IDENTICAL after the
merge with main. Not seen: whether items then come out the same, the replay refusal of a vanilla-only session, any of it
on the console. "Go to Training" picked in the menu leaves the menu open, and Training's menus are not pressed through
while it is (the press-through gives up after 15 s; close the menu first).

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

- On the console: the services NRO at all (curl and mbedTLS linked with the Clang-built picture code, the real
  matchmaking service, STUN, the app's user file from `sdmc:/switch/melee-nx-netplay/User/Slippi`), a search from the game's
  menu, automatic input delay against a real round trip, the decoder thread beside the rollback store's remapping, and
  whether queuing the log removes the 21 ms frames. Play mode's handling of a bad process layout (it mirrors the runner).
- A Ranked request from the game's menu: this game build offers no Ranked row, so the refusal was only seen in the
  headless runner (`--online ranked`).
- On the console: mod profiles (the disc comparison from the SD card, reading mod files during play), the Hack Pack,
  20XX TE and its settings screen (lines and points on NVK), Slippi's menu files, netplay in play mode (the netplay
  object linked with Clang-built code, sockets, write tracking beside the renderer), the GameCube adapter and
  rumble, a long session.
- Console results so far: `--auto-vs` with options reached a match (16:9, big HUD, PAL stock icons, tags); sound had
  no new underruns during the match; the simulation thread's work was 6.9 ms per match frame at 1774 MHz.
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
- melee-unlocked's private stream tokens: the draw owners and stage tags (`0xF1`, `0xF2`, `0xF7`) and the scope and
  hand markers (`0xF0`, `0xF8`) are read since patches/0116 (the looks ask for them); the render semantics (`0xF3`-`0xF6`,
  upstream's RTX path) are not, and nothing here asks the game for them.
- A decoder or renderer error stops the picture, not the run: `picture: DECODER STOPPED` or `RENDERER STOPPED`
  with the reason, and the simulation finishes as the headless runner would.
- Still written while a match runs, by code outside this folder: in scripted mode the runner's progress line every
  600 retraces (to nxlink and `run.log` on the SD card, from the simulation thread) and the game's own messages; in
  both modes the SQLite statistics lines of the app's Switch layer (to nxlink, from Aurora's threads). Play mode queues
  everything else (Netplay, above).
- Nobody services the applet message loop while the game runs (as in the headless runner).
- A first session compiles pipelines as they are first drawn and stutters; they are cached per device. The app is
  in the same position: this repository ships no pipeline seed, and the app's cache lives in its own folder
  (`--cache-dir sdmc:/switch/melee-nx-netplay/Cache` would share it; not tried).
- The app's adapter code looks for a GameCube adapter every two seconds from the simulation thread for as long as
  there is none; here it gets three looks and then one a minute until an adapter has answered once.
- Plain `--play` has no in-game menu or HUD (the app has them); no Slippi music player.
- The HUD's ping is `netplay_bridge::ping_ms()` (the device's round trip from the game's pad acknowledgements, both
  sides). `build.sh sync` must take a runner that has it (`netplay_bridge.*` newer than `d53f7e7`; `MU_SYNC_REF`).

## Licensing

Our files are GPL-3.0-or-later, except `src/controls_core.h` and `src/controls_files.*`, which are adapted from melee-unlocked's
`input_bindings.h`, `controller_profiles.cpp` and `dolphin_profile.cpp` and keep their GPL-2.0-or-later notice. In the mod
installer, the catalog checks of `src/modinstall/mi_catalog.cpp` follow melee-unlocked's `launcher_mod_catalog.cpp` and the
DJW decoder in `src/modinstall/mi_vcdiff.cpp` is adapted from xdelta 3.1.0's `xdelta3-djw.h` (both GPL-2.0-or-later, used
under GPL-3.0-or-later, credited in the files); the LZMA SDK's decoders (public domain) are compiled from the fetched tree
and zlib is the system's or devkitPro's. Use custom music compiles melee-unlocked's `jukebox.cpp` (GPL-2.0-or-later) from
the fetched tree with `patches/0118`, and decodes players' files with minimp3 (CC0 1.0), stb_vorbis (public domain or MIT),
dr_wav and dr_flac (public domain or MIT No Attribution), fetched by `../fetch.sh` at pinned commits and never committed. The program links this repository's GPL-2.0-or-later GX layer, Aurora, Dawn, SDL3
and Mesa as the app does, and melee-unlocked's GPL-2.0-or-later host services from the fetched tree. The display wave (0.4.8)
compiles more of melee-unlocked's GPL-2.0-or-later files from the fetched tree: lab_view.cpp (its drawing after Slippi Lab, MIT),
flicker_scan.cpp (with `patches/0127`), video_background_learning.h and video_background_style.h, port/linux/full/linux_media.cpp,
source_mod_overlay.cpp and layer_fingerprint.h, the present-pass occlusion of present_shader.h adapted in `patches/0125`, and
mapbox's earcut (ISC) beside the Lab view; FFmpeg (LGPL-2.1-or-later: devkitPro's portlib on the console, n7.1 built by
`./build.sh ffmpeg` for the Linux test) decodes the menu videos. The Lab view's silhouette packs (Slippi Lab's animations converted by
melee-unlocked) are downloaded by the player's app from melee-unlocked's release, never shipped. The built NRO
contains the compiled game and must not be distributed.

## Fight a pro and the Practice menu

**Practice menu.** In an offline Training match, Start + D-pad Up + Z opens melee-unlocked's own Practice menu
(`practice_menu.cpp`, patches/0113) in the look `practicestyle` picks (0 Main Menu bars, 1 Side rails, 2 Character
select slots, 3 HUD strip; also on the in-game menu's Training page). The controller that opened it drives it (A pick,
B back, Start close); the game gets neutral pads while it is open and whatever is held when it closes is kept from the
game until let go. Its settings go to settings.ini when it closes. Outside an offline Training match the chord opens the
in-game menu's Training page as before.

**Fight a pro.** Practice menu > Fight a pro. The first time, "Download Phillip's AI (about 64 MB)" asks first
(Download / Not now): the three base policies come from Hugging Face (vladfi/phillip-models, the pinned URLs in
`src/proai/catalog.json`) into `sdmc:/switch/melee-nx-netplay/ProAI/`, each checked against its SHA-256 (or copy the
`.onnx` files there by hand). Then each policy is timed on the console (`pro ai: <file> steps in N ms here: Fight a pro
is on`, 14 ms is the limit as upstream); picking Zain, Cody Schwab or aMSa asks about "Stronger Pro AI" (the matchup
policies, about 0.2 to 0.9 GB per pro; Not now / Always / Never) and starts Training again with the CPU as that pro's
character, which Phillip plays (command 0xEA, offline only) with an 18-frame reaction delay on a worker thread (cores 0
and 1 at priority 0x2C; `proai_threads 1..3` splits its large matrix products). Every 3600 steps the log says
`pro ai: P2 N ms a step`; a step that misses its frame says `answer for frame N is late`. The engine:
`src/proai/onnx_lite.cpp` (operators: Gemm, Add/Sub/Mul/Div/Max/Min, Where, comparisons, Sigmoid/Tanh/Gelu, Sqrt/Log/Neg,
Clip, Cast, Reshape/Squeeze/Unsqueeze/Expand/Concat/Split/Gather, OneHot, ReduceMean/ReduceSum, ArgMax), checked
against onnxruntime by `../tools/proai_check.sh MODELS_DIR` (and the app's own recorded steps,
`MELEE_PRO_AI_RECORD=<file>`, by `../tools/proai_replay.py`).
