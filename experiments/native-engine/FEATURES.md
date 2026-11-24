# melee-unlocked features: the Switch checklist

Upstream: [melee-unlocked](https://github.com/Hero88go/melee-unlocked) **v1.0.5**, commit **`67868aa`**.
Inventory date: **2026-11-22**; status column brought up to date for the app, Melee-NX-NetPlay **0.4.1**, on
**2026-11-23**. Goal: the Switch build brings over every feature listed here.

Upstream code is fetched at build time (`fetch.sh`) into `build/native-engine/` and is **never committed to
this repository**. This file describes it; it copies none of it.

## How to read this file

**Status column**

| Status | Meaning |
|---|---|
| active | On in the Switch app (`frontend/`: play mode, the app, the feature modules) with the game image `build.sh` produces; how it was checked is given. |
| partial, wired | Part of it is on, or it is wired and not yet tried; what is missing is given. |
| not wired | Exists upstream; the Switch host, renderer or menu does not provide it yet. |
| not applicable on the Switch | Cannot exist on the console as upstream built it; the reason is given. Kept in the list on purpose. |
| upstream stub | Advertised or present in upstream's code, but not implemented, disabled, or unvalidated upstream. |

**Other columns**

- **Side**: *game image* (compiled with the game, comes along once the engine runs and its switch is passed),
  *host* (a service our Switch host must provide), *renderer* (drawn by the host's renderer or its overlay
  layer), *launcher* (desktop UI outside the game process), *external* (a network service or tool).
- **Switch that turns it on**: option bit, command byte, ini key or CLI flag, with the upstream source path.
- **Sim**: does it change simulation results? *yes* means it must be off or matched on both sides online; the
  online rule upstream applies is given. *no* means picture or sound only.
- **Effort**: none / small (a setting to pass through) / medium / large.
- **[V]** verified in the cited source file. **[I]** inferred.

**Paths.** Upstream paths are relative to the melee-unlocked root. Short forms: `MH` =
`port/runtime/abi/mu_host.h`, `SH` = `port/app/source_host.cpp`, `MC` = `port/app/main.cpp`, `PS` =
`port/runtime/gx/pc_settings.cpp`, `RO` = `port/runtime/gx/render_options.h`, `hle/` `host/` `gx/` =
`port/runtime/hle|host|gx/`, `shim/` = `sourceport/game/shim/`. Paths starting with `experiments/`, `port/` (when
marked "this repo") or `switch/` are in this repository.

**How it was verified.** Read in full: `MH`, the sub-frame code (`gx/subframe.h`, `gx/authored_pose.h`,
`gx/native_pose_bridge.h`, `abi/mu_native_pose.h`, `gx/frame_queue.h`, `port/linux/renderer_presenter.cpp`), the
Linux host's interface table, this folder's patches and build flags, and this repo's `gx_aurora.cpp`. The rest
was read by four parallel reading passes that cite file and function; their line numbers are approximate, and
the largest bodies (`hle/yougame_link.cpp`, `hle/slippi_net.cpp`, `hle/slippi_online.cpp`,
`host/cosmetic_mods.cpp`, `PS`) were searched and sampled, not read end to end. Nothing was built, run or
contacted.

---

## 1. Facts that decide the effort

1. **One table each way.** `MuHostApi` version 33 and `MuGameApi` version 10 [V MH]. The player-selectable game
   features are five bit-words the host returns (`game_options`, `game_options2`, `game_options3`,
   `display_options`, `audio_options`) plus a few scalars (`hud_scales`, `shield_color`, `grid_color`,
   `screen_shake_scale`, `picture_widen`, `music_volume`, `mod_flags`) [V MH].
2. **Everything online and most extras go through `slippi_command(cmd, payload...)`** [V MH; SH
   `h_slippi_command`]. Command bytes are overloaded: `0xFA` is Hack Pack, voice mods, replay gate and Ranked
   query; `0xEB` is stage songs and tournament; `0xE6` has several YouGame sub-operations [V SH].
3. **What the headless runner passes** (the app, `frontend/`, answers far more: see the status column). The runner uses upstream's Linux minimal host
   (`port/linux/host_services.cpp`, built with `MELEE_NO_SLIPPI`): `game_options` is 0 (or VANILLA with
   `--vanilla-game`), option words 2 and 3 and the display word are 0, `slippi_command` returns -1,
   `native_draw_identity` and pose capture are 0 [V `port/linux/host_services.cpp` `make_api`].
   The runner also links `runner/netplay_bridge.*` (this repo), which puts this repo's Slippi 3.7.0 host code
   behind `slippi_command` for loopback tests only.
4. **Game image flags.** `build.sh` configures the image with `-DMELEE_DOUBLES=OFF` and otherwise upstream's
   defaults [V `experiments/native-engine/build.sh`]. So `MU_NO_SLIPPI` is OFF (the Slippi menus and the
   rollback loop are in the image) and Squad Strike, the Akaneia and ACE fighters, the m-ex stages, Rogue/Maker
   and Training Mode CE are compiled in [V `sourceport/game/CMakeLists.txt` option defaults].
5. **Three host tiers exist upstream** [V]: the Windows full host (`SH`); the Linux minimal host (no online);
   and the Linux "full" host (`port/linux/full/CMakeLists.txt`), which compiles the same `SH` and all of
   `hle/`, `host/`, `gx/` over a Win32 shim in `port/linux/compat/` (WinHTTP on libcurl, bcrypt on OpenSSL, SDL
   audio, FFmpeg). The third is the proof that the whole stack runs off Windows; it is large.
6. **The settings UI and nearly every overlay are Dear ImGui** on the host renderer, shared by upstream's
   D3D12, D3D11 and GL wrappers [V `gx/pc_settings_shared.h`]: practice menu, replay bar, network overlay, arena
   panel, sign-in, Lab view, input display, combo counter.
7. **The game reads environment variables only through `test_getenv`** [V MH], so the host decides which of
   upstream's roughly 500 variables exist.

---

## 2. Frame-rate unlock

**What the player gets** [V upstream README; RO]: the game still simulates at 60 Hz; the display runs at
monitor rate, a cap, or unlocked. In-between pictures are real intermediate poses sampled from the game's
animation data, not image interpolation.

| Feature | What the player gets | Side | Switch that turns it on | Sim | Effort | Status |
|---|---|---|---|---|---|---|
| Sub-frame animation: Off | One picture per 60 Hz tick | renderer | ini `subframe 0`; CLI `--frame-mode off` [V MC] | no | none | active |
| Sub-frame animation: Predict ahead | Extrapolates from the newest game frame, no added delay; eases turns and stops over a few frames | game image + host + renderer | ini `subframe`; CLI `--frame-mode authored` [V RO, MC]; `native_draw_identity`, `native_pose_capture_enabled`, `native_pose_snapshot` [V MH] | no; each player may choose their own online | large | partial: `subframe 1` / `--frame-mode predict` (`MU_FRAME_MODE` in scripted runs): this repo's matrix solver on paired draws (frontend/src/features/present, patches/0106); upstream's authored-pose path is not wired; needs the presenter call in frontend.cpp (frontend/README.md); Linux only |
| Sub-frame animation: Interpolate | Exact in-betweens of the last two game frames, one frame of display delay | same | ini `subframe 2`; CLI `--frame-mode authored-interpolate` | no | large | partial: `subframe 2`, as Predict |
| Generic matrix modes | Extrapolate or interpolate without the authored path | renderer | CLI only: `--frame-mode extrapolate|interpolate` [V MC] | no | large | partial: Predict and Interpolate are these modes here |
| Frame cap | Monitor / unlocked / 60..480 | renderer | ini `fps` (-1 monitor, 0 uncapped); CLI `--fps N|monitor|unlocked`; refused with mode Off [V MC] | no | small after the above | active: `fps N` / `--fps N` / `MU_PRESENT_FPS`; Off mode repeats each game frame (play mode's present hook) |
| Sub-frame pause when the game falls behind online | Keeps both games at full speed | host | `hle/online_frame_budget.h` (90% of frames late in 3 s) [V] | no | small after the above | partial: pauses when 90% of presented frames in 3 s overran a refresh (render thread), not upstream's late-frame count |

**How it works: interpolation in the renderer. The simulation is never run more often**
[V `gx/subframe.h` "Presentation only; guest state is never touched"].

1. *Game side (in the image).* When the host returns nonzero from `native_draw_identity()`, `HSD_PObjDisp`
   brackets every PObj in the FIFO with the private token `0xF0` (21 bytes: sequence, scope id, phase, joint,
   PObj) [V MH; `gx/gx_core.cpp`; `shim/mu_entry.c`]. When `native_pose_capture_enabled()` is nonzero it also
   calls `native_pose_snapshot()` once per scope with a `MuNativePoseSnapshot` [V `abi/mu_native_pose.h`;
   `sourceport/patches/melee-native.patch`]: the joint chain root to leaf (scale, rotation or quaternion,
   translation, world matrix), the animation frame, rate and end with the raw packed track bytes, for skinned
   draws the bones per matrix slot with weights and inverse-bind matrices, and the view matrix with the camera
   and model shake offset.
2. *Decoder (host).* `gx/gx_core.cpp` turns the stream into a `gx::Frame` of `DrawCall`s. Each draw gets a
   stable `identity` (PObj, its ordinal this frame, the draw's ordinal in the scope) and a
   `shared_ptr<AuthoredPose>` [V `gx/gx_core.cpp`, `gx/gx_core.h`]. `gx/native_pose_bridge.cpp` copies snapshots
   into `AuthoredPose`.
3. *Presenter (host render thread).* `FrameQueue` hands complete frames across.
   `SubFrameSolver::set_frames(prev, cur)` pairs draws by identity; `build(t, ...)` fills one `DrawMatrices` per
   draw for phase t in [0,1]: 256 position floats, 96 normal floats, an optional replacement vertex stream and an
   optional projection [V `gx/subframe.h`, `gx/gx_core.h`]. Authored draws are re-posed by sampling their tracks
   at the fractional frame (`gx/authored_pose.cpp`); other draws get a fraction of the rigid delta
   `M_cur * inverse(M_prev)`; unsupported draws hold their pose; menus use a conservative mode; Predict glides
   out the overshoot when motion reverses (`predict_glide`, `arrival_glide`).
4. *Renderer contract.* `Backend::submit_frame(frame, overrides)` draws the same frame again with replaced
   matrices. `set_skip_present(true)` executes a frame's EFB copies without presenting while a backlog is
   drained, because every frame must be executed [V `gx/gx_core.h`, `gx/frame_queue.h`]. Upstream implements
   both in its D3D12, D3D11, Vulkan and Metal renderers; the smallest reference loop is
   `port/linux/renderer_presenter.cpp` [V].
5. *Rollback.* The host calls `mark_discontinuity(rollback)` on a savestate load; `Frame::discontinuous` and
   `hard_cut` stop blending across it [V `gx/gx_core.h`].

**What our renderer (Aurora on Vulkan, fed from the GX FIFO stream) has to do**

This repo already holds older copies of `subframe.cpp`, `authored_pose.cpp`, `frame_queue.h` and
`threaded_backend.cpp` under `port/runtime/gx/` (this repo). `gx::AuroraBackend` refuses both hooks today:
`submit_frame(frame, overrides)` throws "unlocked interpolation is disabled pending locked-state fidelity
gates" and `set_skip_present` throws "drain-without-present is not implemented" [V this repo
`port/runtime/gx/gx_aurora.cpp`].

- [ ] a. Decoder: consume the private tokens `0xF0` (draw scopes) and, for owner tracking, `0xF1`, `0xF2`,
      `0xF7`, `0xF8`. `frontend/README.md` lists them as not understood.
- [ ] b. Host: return 1 from `native_draw_identity`; wire `native_pose_capture_enabled` and
      `native_pose_snapshot` to upstream's `gx/native_pose_bridge.cpp`.
- [ ] c. Take upstream's current `gx/subframe.cpp` and `gx/authored_pose.cpp` (CPU code, no renderer types).
- [ ] d. `AuroraBackend`: implement the override submit. Per draw, load `DrawMatrices.pos/nrm` in place of the
      draw's own XF matrices, use the replacement vertex pointer when set, and the projection when
      `has_projection`. Aurora replays GX state through its own GX layer, so this means emitting the frame's
      draws again with different matrix loads [I].
- [ ] e. `AuroraBackend`: implement skip-present drain, so EFB copies in frames that are not shown still
      happen (the frontend skips whole frames today, a gap its README lists).
- [ ] f. A repeated present of a frame must not redo that frame's texture copies [V `gx/gx_core.h`].
- [ ] g. Presenter loop with a frame cap and `mark_discontinuity` from the rollback path.

The console's own panel and normal dock output are 60 Hz; there the gain is Predict mode's earlier hand-over,
not extra frames [I].

---

## 3. Online

| Feature | What the player gets | Side | Switch that turns it on | Sim | Effort | Status |
|---|---|---|---|---|---|---|
| Slippi Unranked, Direct, Teams, Party | Rollback netplay from the in-game menu against stock Slippi 3.7.0 players [V `hle/slippi_net.h`; `hle/slippi_net.cpp` `SLIPPI_SEMVER "3.7.0"`] | game image + host + external | game option bit `MU_GAME_OPT_SLIPPI_MENUS` 0x8 [V SH]; CLI `--slippi-menus on|off`; commands B0, B3, B4, B5, B9, BA, BC, BE, BF, D5, EF, FA through `slippi_command` [V `shim/mu_online.c`, `hle/slippi_online.cpp`]; game side `shim/mu_slippi_menu.c`, `mu_slippi_css.c`, `mu_slippi_sss.c`, `mu_slippi_splash.c` | yes; `mu_online_codes()` forces Slippi's code set on, max rollback 7, delay 1..15 [V `shim/mu_online.c`] | medium (this repo has a Slippi 3.7.0 host; `runner/netplay_bridge.*` answers the commands in loopback) | active: the app's Online Play page (services program, `slippi_menus 1`); console: Unranked against real Slippi 3.7.0 players twice (640 and 1,940 checksum comparisons, 0 mismatched, games reported); Direct, Teams, Party offered, played only against the loopback mock (`test-online-linux.sh`, Unranked) or not at all |
| Slippi system files | The online menus' patched files, rebuilt from the player's disc | host | served through the disc interface [V SH `load_system_files`]; `host/sysgen.cpp` + `host/vcdiff.cpp` + `port/sys_deltas/`; CLI `--sys-dir`, `--sysgen-data-dir` | no | medium | active in our own form: `slippi_menus 1` builds Slippi 3.7.0's menu files from this repository's vendored patches and the player's disc (frontend/src/slippi_files.cpp); upstream's sysgen is not used |
| Slippi Ranked | Upstream refuses it: "Ranked is not available in Melee Unlocked" [V `hle/native_online_policy.h`] | host | none | yes | none | upstream stub |
| Rollback savestates | Per-frame snapshot and restore; also used by Training Lab saves and replay seek | host | commands B1 / B2; `MuGameApi.snapshot_ranges` minus `state_exclusions` [V MH; `shim/mu_exclusions.c`]; `hle/native_savestate.cpp`; `resim_phase` [V MH] | yes (is the rollback) | medium (`runner/rollback.*` and `runner/dirtytrack*` exist but restore from outside the frame) | active: the hot-page store (runner/rollback.*) answers 0xB1/0xB2 and the engine re-simulates inside the frame; console: 143 rollbacks in the first real match, rollback frame 6.1 ms average |
| Slippi account | Uses a Slippi `user.json` (uid, playKey, connectCode); no in-game sign-in upstream | host | `hle/slippi_account.h`; env `YOUGAME_SLIPPI_USER_JSON`, CLI `--user-dir` [V] | no | small (this repo's app has a sign-in screen) | active: the app's sign-in screen writes `User/Slippi/user.json`, the netplay object reads it (console: both real matches) |
| Matchmaking | Queue search with NAT discovery | host + external | `hle/slippi_mm_service.cpp` (TLS WebSocket queue), STUN from the netplay socket [V]; CLI `--netplay-port`, `--allow-matchmaking` | no | small (this repo has it) | active: this repository's client (WebSocket, STUN) in the services program; console: two real Unranked matches found |
| Online input delay | Auto or 1..9 frames | host | ini `onlinedelay`, `onlinedelayauto`; CLI `--online-delay`; Auto = clamp(ceil((ping/2 + 4 ms)/16.683), 2, 6) [V `hle/slippi_online.cpp` `effective_delay`] | yes; agreed per match | small | active: `onlinedelayauto` / `onlinedelay`, launcher.ini `online_delay`, the menu's row (next game); automatic is this repository's 2..5 frames from the round trip (upstream 2..6); console: 4 frames at 126 ms, 3 at 61 ms |
| Time sync, frame advance/skip | Keeps both games on the same frame | host | `hle/slippi_time_sync.h`, `host::set_emulation_speed` [V] | no | small | active: frame skip and advance in the netplay object; the frame-rate nudge reaches the host's pacing (patches/0004, runner/netplay_bridge.cpp, net_host.cpp) on Slippi's 59.94 Hz frame (patches/net-0003) and the sound output (frontend/patches/0110); `tools/net_test.sh` with one clock 0.4 % fast: the sides settle at x0.998 and x1.001, no frame advance after the start (7 without the nudge), `net check` equal |
| Desync detection and notice | Checksum compared from match frame 85 on; on-screen notice | game image + host | `compute_checksum` in `shim/mu_online.c` [V] | no | none after netplay | active: the game's checksums compared with the opponent's (console: 1,940 comparisons, 0 mismatched); the on-screen notice not seen (no desync) |
| Player drop handling (3.7.0) | A dropped player despawns after 16 frames | game image + host | message 0x88; `hle/slippi_gone_port.h` [V] | yes | none after netplay | active in the netplay object (message 0x88, despawn after 16 frames); only 1v1 played, no drop seen |
| Slippi input suggestions (3.7.0) | Mid-frame hints sent and received | host | message 0x87; `hle/slippi_input_suggestions.h` [V] | no (prediction) | small | active (0.4.1): sent and used, `input_suggestions 1` (default) or the Game page row; patches/net-0001 on the netplay object's copy of port/runtime/hle, pads read between frames through frontend/src/play_io.cpp `mu_net_suggest_read`; loopback tests in frontend/README.md (Input suggestions); not yet against a real Slippi player |
| Version gate | Search refused when Slippi has a newer release | host | `hle/slippi_version.h`; `Matchmaking::FindMatch` [V] | no | none (this repo has its own) | active in this repository's form: a search is refused with "Slippi X is out" when the service names a newer release (slippi_online.cpp, this repository); not seen on the console |
| Replay recording (.slp) | Every match written as a Slippi replay | game image + host | `replay_event` [V MH]; `shim/mu_replay.c`, `shim/mu_record_start.inc`; `hle/native_replay_stream.cpp`; CLI `--replay-dir`, `--record-native`; metadata `muOptions`, `muOptions2`, `muOptions3`, `modProfile` [V SH] | no | small to medium | active: offline VS and online matches, sdmc:/switch/melee-nx-netplay/Replays (`savereplays`, `replay_dir`); Slippi 3.20 stream with Dolphin metadata and muOptions*/modProfile (frontend/src/features/replays.cpp) |
| Compressed replays (.slpz) | Smaller replay files | host | `port/app/replay_slpz.cpp`; CLI `--compress-replays`; `compressreplays=1` in `launcher.ini` [V] | no | small | active: settings.ini `compressreplays 1` (or the Replays page's row) writes `Game_*.slpz` with the fetched tree's `replay_slpz.cpp` (compiled by replays.cpp, system or devkitPro zstd), checked to unpack to the recording before it is written; playback and the Replays page list read `.slp` and `.slpz`. `tools/slpz_roundtrip.sh`: 11 replays (Slippi Dolphin, the recompiled app and play mode) compress and unpack byte-identical, 2.3x to 18.4x smaller; a play-mode match recorded as .slpz played back with 0 of 3302 post-frame records differing |
| Replay playback | Plays a .slp or .slpz | game image + host | CLI `--replay <file>`, `-i <comm.json>`, `-b`; `replay_start`, `replay_frame`, `replay_stock_steal`, `replay_finished` [V MH]; `MU_REPLAY_CODE_*` | yes (replays the recorded codes and options) | medium | active: `--replay FILE|latest|N` (.slp or .slpz), Replays page "Watch it" (play mode restarts the game with it); with patches/decomp-0003 a recorded offline CPU match re-records with 0 of 1202 post-frame records differing (and 0 of 1926 after a jump back); recordings made by an image without that patch differ in one uninitialised shield-flag byte of the second fighter until it first shields |
| Replay viewer controls | Pause, step, 0.5x and 0.25x, fast forward, seek, "as it was played", Analyze | host + renderer | command 0xFA (5 bytes) [V SH]; `host/replay_control.cpp`; `gx/replay_bar.cpp`, `gx/replay_hud.cpp`; CLI `--trace`, `--as-experienced`; ini `clean_replays` | no | medium | partial: pause (Start or Z), frame step (paused: D-pad Right, held repeats), 5 s back and forward (D-pad), 1/2 and 1/4 speed (L), fast forward (R), the overlay hidden (B), Analyze (Replays page while one plays: the rest of the replay unseen, then back), an on-screen bar (timeline with the coach's tip markers, time, state, buttons; fades while just playing) and a paused picture that keeps repainting (frontend/src/replay_view.h). `frontend/test-replays-linux.sh` `controls` and `analyze`: every button exercised, 0 of 4794 post-frame records differing; screenshots `build/native-engine-gfx/run-replays/*/shots/`. Jumps now simulate their frames with the draw pass (not shown): undrawn frames differed from frame 1866 on with items. Not here: "as it was played" (needs the .trace files, which this build does not write) and upstream's mouse bar |
| Replay takeover | Take over a player from any moment | game image + host | `MuReplayFrame.live_ports` [V MH]; command 0xE9; `shim/mu_replay_defend.c` | yes (from that frame) | medium | active (offline: replays only): A takes over the player shown top left (D-pad Up/Down picks another) from that frame; MuReplayFrame.live_ports from features/replays.cpp, the taken port gets the player's controller (the port's own, else the first connected), the others follow the recording until the live game leaves it, then the Practice CPU model defends them through command 0xE9 (`replay_opponent`: smart defence, no DI or techs, the Practice CPU's settings, follow the recording); Start retries from the kept state, held Start goes back to watching. `test-replays-linux.sh takeover`: P2 left the recording at frame 416 and defended, one retry, back to watching: 0 of 3878 compared post-frame records differ (916 takeover records not compared). The other controllers' presses never reach the game during playback now (Start or L+R+A+Start would pause or end the match) |
| Replay coach tips | Per-hit frame advantage, shield, L-cancel and tech notes | game image + host + renderer | `shim/mu_framedata.c`, command 0xE8, `host/replay_coach.cpp` [V] | no | medium | active: upstream's replay_coach.cpp and replay_scan.cpp compiled from the fetched tree (features/replay_coach_glue.cpp); events read from the file at load (30 in the test replay, 0 ms) and measured by the game (command 0xE8); the moments (unsafe on shield, dropped combos, missed L-cancels and techs) as a banner at the moment, markers on the timeline, the newest reading top left; `replay_tips 0` hides them. `test-replays-linux.sh tips` (next/last tip jumps, coach lines in the log), screenshots |
| Game reporting | Finished games reported to Slippi | host + external | command BD; `hle/slippi_report.cpp` [V]. Upstream sends a hard-coded Slippi Dolphin user agent [V] | no | none (keep this repo's reporter) | active: this repository's reporter in the services program (console: the game report was sent) |
| Quick chat | D-pad chat on the online character select | game image + host | commands C3, BB; message 0x84; `shim/mu_slippi_chat.c`; CLI `--chat on|direct|off`; ini `quickchat` [V] | no | small | active: the game's 0xBB and incoming 0x84 go through the netplay object; only Slippi's own message ids exist (`shim/mu_slippi_chat.c` checks them), so a Slippi 3.7.0 peer reads every one; settings.ini `quickchat 0|1|2` (on, Direct only, off; upstream's key and values) or the Online page's row (0.4.4, features/search.cpp, `netplay_bridge::set_chat`); `test-online-linux.sh` with `MU_PADS_A=scripts/online_unranked_chat.txt MU_TWO_SCREENS=1`: back on Slippi's character select after the match, side a's D-pad Up, Up sends 0x88 ("ggs"), side b's log has `net: chat: message 0x88 received from player 2` and its screen shows the "Mock 2: ggs" bubbles (screenshot); the match before it identical on both sides. Chat against a real Slippi player not tried |
| Stage pool and random stage | Legal stages, bag random, Teams never draws FoD | game image + host | `hle/slippi_stage_pool.h`, `hle/slippi_online.cpp` [V] | yes; set by the server | none after netplay | active: the netplay object's stage bag from the service's stage list (console: Yoshi's Story) |
| Pause in Direct and Friends | Pausing allowed when every peer agrees | host | ini `directpause` [V] | yes | small | active in Direct: Start pauses Direct (Slippi's rule, as before); settings.ini `directpause 0` turns it off only when every player's build is a Melee-NX-NetPlay one (build message 0xD0, patches/net-0004, sent first on the reliable channel so it arrives before the selections that start a match); against Slippi Dolphin Slippi's rule stays. `tools/net_test.sh` (Direct): `directpause 0` on one side: both log `Direct pause off`; against a side behaving as Slippi Dolphin (`--net-as-slippi`) both `Direct pause on`; `net check` equal in every run. A Start press in such a match was not tried. Friends is upstream's own lobby: not applicable |
| Skip in Unranked | Turn down up to three connect codes | host | ini `blockcode1..3` [V] | no | small | active: settings.ini `blockcode1..3 CODE` (patches/net-0004, upstream's rule: Unranked only; a new search starts and the other side sees a failed connection); `test-online-linux.sh` with `blockcode1 mock#003` on side a: three matches with MOCK#003 turned down (`matched MOCK#003, who is on the Unranked skip list; searching again`), side b `connection attempt failed, searching again`, no match played. Works against any opponent |
| Early inputs ("subframe rollback") | Controller changes sent the moment they happen, between Melee Unlocked players | host + game image | ini `early_inputs2` (off by default in 1.0.5 [V RO, PS]); env `MELEE_EARLY_INPUTS`; `host/early_input.cpp`, `host/early_input_rules.h`; message 0xE1; receiver in `shim/mu_online.c`, `shim/mu_online_prediction.inc` | no (prediction only; the confirmed pad decides) | medium | not applicable with a Slippi 3.7.0 peer: upstream's early pads are its own message 0xE1, sent only to melee-unlocked builds whose build message (0xE0) says they read it; Slippi Dolphin 3.7.0 neither sends nor reads it. What a Slippi 3.7.0 peer does understand, Slippi's input suggestions (0x87), is active (row "Slippi input suggestions"). Not ported between Melee-NX-NetPlay builds either: the prediction it feeds is upstream's game-side receiver (`shim/mu_online_prediction.inc`), and a second prediction channel next to the suggestions would only matter against our own builds |
| Early rollback from early pads | Rolls back as soon as an early pad disagrees | host | env `MELEE_EARLY_ROLLBACK=1`, off by default [V reading pass] | no | medium | upstream stub |
| Build message and setup hash | Mod and retail players kept apart; both sides prove the same setup | host | messages 0xE0, 0xE6; `abi/mu_setup_hash.h`; `build_verdict` in `hle/slippi_online.cpp` [V] | yes (gate) | medium with this repo's netcode | not wired: only between melee-unlocked peers (0xE0/0xE6); a Slippi 3.7.0 peer neither sends nor reads them |
| Opponent shield colour | Your chosen shield and KO blast colour shows on Melee Unlocked opponents | game image + host | `shield_color(slot)` [V MH]; ini `shield_color_online`, `shield_colorp1..4` | no | small | active between Melee-NX-NetPlay builds: settings.ini `shield_color_online RRGGBB` is this player's shield online (any opponent, display only), and the opponent's choice is shown when their build sent it (build message 0xD0, patches/net-0004); `test-online-linux.sh` with side a `shield_color_online FF00FF`, side b `00FF00` and `MU_PADS_B=scripts/online_shield_b.txt` (Fox shields twice for 5 s in the match): both logs `opponent ... shield colour`, side a's screen shows the opponent's Fox (port 2, blue by default) shielding in green, side b's own screen the same (screenshots, 0.4.4); `net check` equal, 0 desync messages. melee-unlocked's own build message (0xE0) is not spoken, so a melee-unlocked opponent's colour is not shown nor ours to them; against Slippi Dolphin each side sees only its own |
| Online port colours | Tag and percent in a chosen port colour in 1v1 | game image + host | display bits 28-31 `MU_DISPLAY_OPTION_PORT_COLOURS` [V MH]; ini `portcolour1`, `portcolour2` | no | small | active between Melee-NX-NetPlay builds: settings.ini `portcolour1`, `portcolour2` (0 red, 1 blue, 2 yellow, 3 green), exchanged in the build message (patches/net-0004), upstream's rule for a clash; both sides log `port colours P1 3, P2 2`, display word `B0000000`, and both screens show the green and yellow HUD (screenshots); off against Slippi Dolphin and melee-unlocked (display only either way) |
| Live pick previews | See the opponent's cursor pick | host + game image | `abi/mu_pick_preview.h` [V] | no | medium | active between Melee-NX-NetPlay builds (0.4.4): the game's online character select sends and reads upstream's command 0xED (op 0 its pick every frame, op 1 the others'); the netplay object (patches/net-0005) sends upstream's pick preview bytes (`abi/mu_pick_preview.h`, its rate rule) as message 0xD1 only to a peer whose build message (version 2, flag 0x04) says it reads them; settings.ini `pick_previews 0` or the Online page's row turns it off. `test-online-linux.sh` (keep-search run): both logs `pick preview from port N (12 bytes, fighter 20/2)`, and side b's select after the match shows "Playing: Mock 2 / Mock 2: Falco" (screenshot); with side b behaving as Slippi Dolphin (`--net-as-slippi`): no preview sent either way, `net check` equal. Against Slippi Dolphin or melee-unlocked nothing is sent (their builds do not say they read it; upstream's own 0xE4 is for melee-unlocked peers only) |
| Opponent origin label | "(Slippi)" or "(YouGame)" on the character select | host | command 0xEF 0x49; `hle/opponent_display.h` [V] | no | small | not applicable on the Switch: the label exists for upstream's Both search (YouGame's and Slippi's pools at once); with no YouGame every opponent is a Slippi player and the character select's header already reads "Slippi Unranked" (screenshot in the quick chat test) |
| Play while you search | Keep training while an Unranked, Direct or Teams search runs; countdown into the match | host + game image + renderer | `hle/native_practice.cpp`; `MuGameApi.practice` ops [V MH]; `shim/mu_practice.c`; `gx/practice_menu.cpp`; ini `keep_search`, `search_banner` | no | large | active for Unranked (0.4.4; upstream keeps Slippi's Unranked only, its Direct/Teams keep is for YouGame): on Slippi's select with an Unranked search running and the fighter locked in, L+R held (40 frames) takes the game to Training with that fighter while the search goes on (features/search.cpp, patches/net-0005 `keep_search`: the game's cleanups are skipped while it is kept); the game sees no session meanwhile (play mode's option words are the player's, offline tools work); a line at the top shows the search time; when the opponent is found, "Opponent found: back to the match in 3", then back through the main menu to the select, still locked in, and the match starts; L+R in Training goes back at once; a search that stops leaves the player in Training with a note. settings.ini `play_while_searching 0` (or the Online page's row) turns the gesture off. `test-online-linux.sh` with `MU_PADS_A=scripts/online_keep_search_a.txt MU_PADS_B=scripts/online_keep_search_b.txt MU_TWO_SCREENS=1` (6000 retraces): side a searched, went to Training at retrace 580, played it (match 1, offline words), side b's search started 20 s later, found at 1654, back on the select at 1858 (search state 4, locked in), match 2 online, identical on both sides, `net check` equal, 0 desync messages (screenshots: Training with the search line, "Opponent found"). Upstream's ask-on-leaving question and its other ways out (to the menus) are not ported: only the L+R gesture |
| Warm-up room | Park a Direct or Teams connection and warm up in Training | host | `hle/slippi_warmup.cpp`; message 0xE9; ini `warmup_button`, `warmup_panel_style`, `warmup_turn_instant` [V] | no | large | active (0.4.4): connected in Direct or Teams and not locked in, L+R held on the select takes the game to Training (the fighter last locked in this session) while the connection stays up (the same `keep_search`); when every other player has locked in, "Opponent is ready", 3 s, back to the select to lock in; L+R goes back at once; a connection lost meanwhile leaves the player in Training ("Your opponent left"). settings.ini `warmup 0` turns it off. Works against Slippi Dolphin: a Slippi player only sees this player not locked in yet. Upstream also tells its own builds "warming up" (message 0xE9); not sent. Test: `test-online-linux.sh` with `MU_PADS_A=scripts/online_warmup_a.txt MU_PADS_B=scripts/online_warmup_b.txt MU_ARGS_A="--feature-test unranked-as-direct"` (the loopback stand-in pairs Unranked searches; Direct needs a typed connect code): after match 1 side a warmed up in Training from retrace 3950, side b locked in, side a back on the select at 4815, locked in, match 3 online; `net check` equal, 0 desync messages (screenshot). Not tried with a real Direct code; upstream's arena-room wait is not ported (row "Arena rooms") |
| Training after online | Drop into Training with your character when a session ends | host | ini `online_training_after`, `online_return_training` [V] | no | medium | active (0.4.4), off by default as upstream: settings.ini `online_training_after 1`: when a Direct or Teams session ends after at least one match (the other player left, Z held, the connection dropped) and the player is still on the online screens, straight into Training with this player's fighter against the last opponent's (Unranked excluded, as upstream); `online_return_training 1`: Training's character select instead, in every mode; Online page rows. Upstream's "search again?" prompt in Training afterwards is not ported. Test: `test-online-linux.sh` with side a `online_training_after 1` and `--feature-test unranked-as-direct`, side b holding Z on the select after the match (`scripts/online_shield_b.txt`): side a `online session over after 1 match(es) ... into Training with fighter 20 against fighter 2`, Training with Falco against a Fox CPU (screenshot) |
| Arena rooms and invites | Extra friends join a room, watch live, rotate in | host + renderer + game image | `hle/slippi_arena.cpp`, `hle/arena_rules.cpp`, `hle/arena_feed.cpp`; `gx/arena_panel.cpp`; commands 0xDB, 0xDC, 0xDD; message 0xE7; ini `room_watch`, `warmup_watch`, `spectate_view` [V] | yes (room rules) | large (upstream's viewer is a second process and window; needs an in-process view [I]) | not wired; only between melee-unlocked peers: upstream's rooms are its own messages (0xE7) and commands over Slippi Direct links that a Slippi 3.7.0 peer neither sends nor reads, and its viewer is a second process and window. Nothing of it reaches a Slippi player, so it would only work between our own builds; not started |
| Slippi spectator server | Feeds the Slippi Launcher's Broadcast tab and slippi-js overlays | host | `hle/slippi_spectate.cpp` (ENet, port 51441); ini `slippi_spectator`, `slippi_spectator_port` [V] | no | small to medium | active (off by default): settings.ini `slippi_spectator 1`, `slippi_spectator_port N` (frontend/src/features/spectator.cpp, adapted from upstream's server: same messages and cursors; every interface on the console, 127.0.0.1 off it); `frontend/test-spectator-linux.sh`: an ENet client (tools/spectator_client.cpp) received a VS match from cursor 0, at the start and 25 s late, and the bytes were the recorded .slp's raw element, IDENTICAL (867,854 bytes, 16,513 events). Not tried with the Slippi Launcher itself |
| Ping readout | Ping under the frame counter | renderer | ini `showping` [V PS] | no | small | active in our own form: the HUD (`hud=1`) shows ping and delay online (console: 61 ms, delay 3) |
| Network and timing overlay | Last 10 s of frame time, waits, rollbacks, time sync, ping | renderer + host | `gx/net_overlay.cpp`, `host/net_trace*.cpp`; ini `netoverlay` [V] | no | medium | active (0.4.4): settings.ini `netoverlay 1` (or the Online page's row) draws upstream's panel during an online match (adapted from `gx/net_overlay.cpp` in src/overlay_draw.cpp): the last 10 s of frame time with each tick's work, waits for the other player, time sync halts and advances, rollbacks by depth (top: 7), ping, marks; built from upstream's per-tick records (`host/net_trace.h`) by frontend/src/features/netoverlay.cpp from the netplay commands (0xB0, 0xB2) and the netplay object's frame report (patches/net-0005); a build without a picture overlay shows one summary line. `test-net-linux.sh` with 40 ms delay each way: the panel on screen (screenshot), `netoverlay: 10 s: ping 83 ms, 100 rollbacks (deepest 1), 0 waits, time sync x0.9990, frames 16.7 ms`; the record's rollbacks and depths equal the netplay host's own count (479; depth 1:477, 3:1, 4:1); `net check` equal |
| Session trace file | `.trace` beside each replay for the timing graph | host | `host/net_trace_file.cpp` [V] | no | small | active (0.4.4): `Game_*.trace` beside an online match's replay, upstream's CSV (`host/net_trace.h` csv_header/csv_row: frame, wall time, work, waits, rollbacks and depth, time sync offset and shed/advance, ping, the local pad, marks); settings.ini `session_trace 1` (default, upstream's rule: kept only when the match had a mark, D-pad Left/Right/Down on this player's controller, or a wait of 6 frames or more), 2 every match, 0 never (Online page row); rows kept in memory, written by the replay writer after the match (no card writes while playing). Columns upstream fills that this host does not know: presents and desync are 0. `test-net-linux.sh` (session_trace 2): both sides wrote a 2963-row trace beside their replay; keep-search run: kept by rule 1 for its 6-frame wait |
| Agreed Gecko codes online | Offer and accept codes on the character select | host | `host/gecko_online.cpp`, `host/gecko_session.cpp`; `kOnlineCodes = false` [V] | yes | none | upstream stub |
| YouGame online: sign-in | Sign in by short code with a QR | host + renderer + external | `hle/yougame_signin.h`, `hle/yougame_channel.h`; `gx/signin_overlay.cpp`, `gx/qr_code.cpp`; env `YOUGAME_BRIDGE`, `YOUGAME_TOKEN`, `YOUGAME_STDIO` [V reading pass] | no | large (upstream needs YouGame's desktop app or the launcher's web view as the other end; the console needs its own bridge to the service) | not wired |
| YouGame Ranked | Best-of-3 with stage striking and a rank | game image + host + external | `hle/yougame_ranked.h`; `shim/mu_ranked_*.c`, `shim/mu_rank_badge.c`; commands C0, C1, C2, C4, E3, E4, E5; CLI `--ranked-rules`, `--online-mode ranked` [V reading pass] | yes; rules digest must match | large | not wired |
| YouGame Unranked, with "Both" search | Search YouGame, Slippi or both at once | game image + host + external | `shim/mu_online_pages.inc`; `hle/yougame_blend.h`, `hle/yougame_unranked.h`; command 0xEF sub-ops 0x40-0x48 [V reading pass] | yes | large | not wired |
| YouGame Friends | Friend picker and a 4-player lobby on Melee's VS select | game image + host + external | `hle/yougame_lobby*.h`; `sourceport/game/include/mu_friends4_css.inc`; `abi/mu_lobby_wire.h` [V reading pass] | yes; setup hash | large | not wired |
| Friends live handoff | Join a lobby mid-flow | host | `YOUGAME_FRIENDS_HANDOFF_DEFAULT` OFF [V root `CMakeLists.txt`] | yes | large | upstream stub |
| YouGame Doubles | 2v2 with a friend | game image + host + external | `MELEE_DOUBLES` (we build OFF); `shim/mu_doubles*.c`; `hle/yougame_doubles*.h`; command 0xE6 sub-ops 11-16 [V reading pass]. `docs/doubles.md` says invite-Duo is "blocked on the page promotion" | yes | large | not wired |
| Local partner in Doubles | A second local controller takes the partner seat | host + game image | message 0xE5; `docs/local-partner.md`: verified offline only [V] | yes | large | upstream stub |
| Squad Strike | Pick four fighters, play them in order, one stock each | game image + host | `MELEE_SQUAD_STRIKE` (ON in `port/cmake/squad.cmake`, defines `MU_SQUAD=1`); `shim/mu_squad.c`, `shim/mu_squad_css.c`, `shim/mu_squad_online.c`; `hle/yougame_squad.h`; message 0xE3 [V]. See the licence section | yes | medium to large | not wired |
| YouGame tournament / CPU bracket | A bracket row when the service offers one | game image + host + external | `shim/mu_tourney.c`, `abi/mu_tourney.h`, `hle/yougame_tourney*.h`; command 0xEB [V reading pass] | yes | large | not wired |
| Console link | 2 to 4 machines run the whole game as one console, menus included | game image + host + external | `console_frame_begin`, `console_seed`, `console_rollback`, `console_alarm_clock`, `console_has_save`, `console_origin` [V MH]; `hle/yougame_console*.h`; `shim/mu_console_*.c`; env `MELEE_CONSOLE_ROLLBACK`. `docs/console-link.md`: device validation pending; the doc and `hle/yougame_console_rollback_default.h` disagree on the default | yes | large | upstream stub |
| YouGame relay and UDP transports | Direct, app-owned and room-relay links | host + external | `hle/yougame_udp.h`, `hle/yougame_room_relay.h`, `port/third_party/enet/yg_enet_socket.c` [V reading pass] | no | large | not wired |
| YouGame version pin | Restart into an older release the service asks for | host + launcher | `hle/yougame_update.h`; command 0xEF sub 3 [V reading pass] | no | n/a | not applicable on the Switch (the app is built from the disc; no version folders) |
| mu_net P2P Direct | Encrypted 1v1 rollback with no Slippi account, between Melee Unlocked builds | host + game image | `port/runtime/mu_net/*`, `port/app/source_p2p.cpp`; CLI `--p2p-port`, `--p2p-peer`, `--p2p-slot`, `--p2p-chars`, `--p2p-stage`, `--p2p-seed`, `--p2p-delay`, `--p2p-identity`, `--p2p-expect`, `--p2p-result`, `--p2p-names`, `--p2p-connect-seconds`, `--p2p-games`; game `shim/mu_p2p_entry.c`, `shim/mu_noslippi.c` under `MU_NO_SLIPPI`; spec `docs/mu-net-protocol.md` [V]. Upstream's launcher shows it only with env `MELEE_P2P_TEST` | yes; build id and content hash must match | medium (sockets, ENet, Monocypher) | not wired; only between melee-unlocked builds: upstream's own encrypted protocol (`docs/mu-net-protocol.md`, Monocypher), its build id and content hash, no Slippi account; a Slippi 3.7.0 peer cannot join it. This repository's `--net INDEX:PORT:IP:PORT` (runner, play mode) is a direct Slippi-protocol link for tests between two of its builds, not upstream's P2P |
| Discord Rich Presence and Join | Presence and join-by-code through the local Discord client | host | `host/discord_presence.cpp`; ini `discordon`, `discord_app_id` [V] | no | n/a | not applicable on the Switch (no local Discord client) |

---

## 4. Gameplay options and code packs

| Feature | What the player gets | Side | Switch that turns it on | Sim | Effort | Status |
|---|---|---|---|---|---|---|
| Slippi General Codes + Lagless FoD | Unlocks, tournament default rules, menu codes, neutral spawns | game image | always on; off only with `MU_GAME_OPTION_VANILLA` 0x4 (`--vanilla-game`) [V `shim/mu_gecko.c` `mu_general_codes`] | yes; "Online opponents always play with the General Codes, so an online match does too" [V] | none | active |
| UCF 0.84 | Dashback, shield drop, SDI, tumble wiggle and the rest of the controller fix | game image | part of the General Codes; `mu_ucf_*` in `shim/mu_gecko.c` [V] | yes; always on | none | active |
| UCF 0.8 | Only when playing back a replay that carries it | game image | `MU_REPLAY_CODE_UCF08` [V MH] | yes | none | wired with replay playback: a replay whose code list carries UCF 0.8 gets MU_REPLAY_CODE_UCF08 (features/replays.cpp); no UCF 0.8 replay tried |
| Unlock everything | All characters and stages; off means the save decides | game image | inverse of `MU_GAME_OPTION3_LOCKED_CONTENT` 0x80000000; ini `unlock_all` [V `shim/mu_gecko.c` `mu_unlock_all`] | content; a network session is always unlocked | none | active (option word 3 is 0) |
| Show results screen | Off: an offline match returns to character select | game image | `MU_GAME_OPTION3_SKIP_RESULTS` 0x40000000; ini `results_screen` [V] | offline flow only | small | active (settings.ini, settings.cpp; passed to the game, its effect not looked at) |
| Freeze Pokemon Stadium | No transformations, offline | game image | `MU_GAME_OPTION2_STADIUM_FROZEN` 0x01000000; ini `stadium_frozen` [V `shim/mu_te.c`] | yes; offline only, recorded as isFrozenPS | small | active (settings.ini, settings.cpp; passed to the game, its effect not looked at) |
| Freeze Final Destination | Background stays on its first form | game image | `MU_DISPLAY_OPTION_FREEZE_FD` 0x10; ini `freeze_fd` [V MH] | no | small | active (settings.ini, settings.cpp; passed to the game, its effect not looked at) |
| Fountain of Dreams reflections | Reflections, water and jets back, offline | game image | `MU_DISPLAY_OPTION_FOD_REFLECTIONS` 0x4; ini `fodreflections` (default off [V RO]) | no; online keeps Lagless FoD | small | active (settings.ini, settings.cpp; passed to the game, its effect not looked at) |
| Full FoD reflections | Full-resolution reflections with full models | game image | `MU_DISPLAY_OPTION_HQ_FOD_REFLECTIONS` 0x00100000 [V MH] | no; dropped online [V `shim/mu_code_features.c`] | small | active (0.4.1): `community_codes` bit 100000, a row on the menu's Community codes page; offline only (the game drops it online); passed to the game, its effect not looked at |
| Widescreen 16:9 (Slippi code) | The game's cameras widen | game image | `MU_GAME_OPTION_WIDESCREEN` 0x10000000; CLI `--widescreen` only [V] | no | small | active (settings.ini, settings.cpp; seen in the game's log or a screenshot, frontend/README.md) |
| True widescreen | The renderer widens; the game draws fighters in the added sides | game image + renderer | `MU_GAME_OPTION_TRUE_WIDESCREEN` 0x20000000 + `picture_widen()` [V MH]; ini `truewidescreen`, `truewsaspect`, `truewscustomw/h`; CLI `--true-widescreen[=...]` | no | medium | not wired |
| Disable screen shake | Camera quake zeroed | game image | `MU_GAME_OPTION_NO_SCREEN_SHAKE` 0x1; ini `noscreenshake`; CLI `--no-screen-shake` [V] | no | small | active (settings.ini, settings.cpp; passed to the game, its effect not looked at) |
| Screen shake strength | 0 to 200% | game image | `screen_shake_scale()` [V MH]; ini `screenshake` | no; network sessions keep the game's own scale | small | active (settings.ini, settings.cpp; passed to the game, its effect not looked at) |
| PAL stock icons | PAL-size stock row | game image | `MU_GAME_OPTION_PAL_STOCK_ICONS` 0x2; ini `palstockicons`; CLI `--pal-stock-icons` [V] | no | small | active (settings.ini, settings.cpp; seen in the game's log or a screenshot, frontend/README.md) |
| Stock icon and damage number size | 75 to 175% | game image | `hud_scales()` [V MH]; `shim/mu_hud_scale.c`; ini `stockhudscale`, `damagehudscale` | no | small | active (settings.ini, settings.cpp; seen in the game's log or a screenshot, frontend/README.md) |
| Always show player tags | P1 / P2 markers stay up | game image | `MU_DISPLAY_OPTION_PLAYER_TAGS` 0x2; ini `playertags` | no | small | active (settings.ini, settings.cpp; seen in the game's log or a screenshot, frontend/README.md) |
| Low poly fighters | The game's far model | game image | `MU_DISPLAY_OPTION_LOW_POLY` 0x1; ini `lightweight 1` | no | small | active (settings.ini, settings.cpp; passed to the game, its effect not looked at) |
| Hide KO explosions | Blast spawned out of sight | game image | `MU_DISPLAY_OPTION_NO_KO_EXPLOSIONS` 0x400; ini `hide_ko_explosions` | no; "the match is the same" [V MH] | small | active (settings.ini, settings.cpp; passed to the game, its effect not looked at) |
| Hide emblems behind percents | | game image | `MU_DISPLAY_OPTION_NO_HUD_EMBLEMS` 0x800; ini `hide_hud_emblems` | no | small | active (settings.ini, settings.cpp; seen in the game's log or a screenshot, frontend/README.md) |
| Training HUD on/off | Training's panels hidden while its menu is closed | game image | `MU_DISPLAY_OPTION_NO_TRAINING_HUD` 0x08000000; ini `training_hud` | no | small | active (settings.ini, settings.cpp; passed to the game, its effect not looked at) |
| Character select background plays through | | game image | `MU_DISPLAY_OPTION_CSS_BACKGROUND_FREE` 0x8; ini `cssbackgroundfree` | no | small | active (settings.ini, settings.cpp; passed to the game, its effect not looked at) |
| Game language | Japanese / English / the save's own | game image | `MU_GAME_OPTION_LANGUAGE_MASK` bits 30-31; ini `gamelanguage`; CLI `--language` | no | small | active (settings.ini, settings.cpp; seen in the game's log or a screenshot, frontend/README.md) |
| Shield colours per port | Colour wheel or presets for shield and KO blast | game image + host | `shield_color(slot)`; ini `shield_colorp1..4` | no | small | active offline: `shield_colorp1..4 RRGGBB`; a magenta P1 shield in a VS match (Linux screenshot); online the ports' own |
| Community codes: HUD transparency, Nana's percent, ally indicators, respawn platform by stocks, hide GO!, V-cancel sound, hide fighter shadows, centre the 2P HUD, hitbox colours by id, priority order, knockback angles | Native versions of popular Gecko codes | game image (+ renderer for Nana and ally overlays via `hud_extras`) | display mask `MU_DISPLAY_OPTION_CODES_ALL` 0x053FF000 [V MH]; `shim/mu_code_features.c`; ini `community_codes` | no; "none of them changes what the match does" [V]; V-cancel sound dropped online | small (overlays medium) | active (settings.ini, settings.cpp; seen in the game's log or a screenshot, frontend/README.md) |
| Offline codes: Instant Melee, unrestricted pause camera, Boot to CSS | Faster loads, free pause camera, start at character select | game image | display bits 0x00400000, 0x00800000, 0x02000000 [V MH] | menu timing and pause; the host sends them only outside network play [V SH `h_display_options`] | small | active (settings.ini, settings.cpp; seen in the game's log or a screenshot, frontend/README.md) |
| Zelda starts as Sheik | Human Zelda starts transformed, offline | game image + host | command 0xDE; ini `zelda_as_sheik` [V SH] | yes; offline only | small | active: `zelda_as_sheik 1` (command 0xDE), offline only |
| Offline input delay | 0 to 9 frames, to practise at online delay | host | `host/offline_input_delay.cpp`; ini `offline_delay`, `offline_delay_frames` | inputs only; offline only | small | active: `offline_delay 1`, `offline_delay_frames 1..9`, offline matches only |
| Mute character voices | Fighter voices at volume 0 | game image | `MU_AUDIO_OPTION_MUTE_VOICES`; ini `mute_voices` [V MH] | no; allowed online | small | active (settings.ini, settings.cpp; passed to the game, its effect not looked at) |
| Faster sound effects | New sounds start 5 ms sooner | game image | `MU_AUDIO_OPTION_EARLY_KEYON`; ini `audio_early_keyon`; env `MELEE_AUDIO_EARLY_KEYON` | timing of sounds the game reads; offline only [V MH] | small | active (settings.ini, settings.cpp; passed to the game, its effect not looked at) |
| Auto L-cancel | The host injects the L press | host | `host/lcancel.cpp`; ini `autolcancel`; CLI `--auto-lcancel` | changes inputs; allowed offline and in Direct, suppressed in Unranked, Teams, Party, Ranked [V `host/lcancel_policy.h`] | small to medium | partial: `autolcancel 1`, offline only (upstream also allows Direct); three scripted aerial landings with no L press were recorded as L-cancel success in the match's .slp |
| L-cancel flash (MU missed / success / both) | The fighter flashes on a missed or made L-cancel | host + renderer | `host/lcancel.cpp` reads `MuGameApi.lcancel_view`; FIFO token 0xF1 via `native_owner_tracking` [V MH]; ini `lcancel_flash_mode`, `lcancel_success_flash`; CLI `--lcancel-indicator` | no | medium (renderer per-player tint) | partial: `lcancel_flash_mode 1|5|6`, `lcancel_success_flash` (settings.ini, Overlays page) from the game's `lcancel_view`; drawn as a ring with "L-CANCEL" / "MISSED L-CANCEL" around the fighter for 10 frames, not as a tint of the model (that needs the draw-owner tokens 0xF1, which this decoder does not read); online only for this console's player. Linux: `frontend/test-overlays-linux.sh match` (3 made, 5 missed, both rings seen) |
| Player-pasted Gecko codes | Codes from `GeckoCodes.ini`, switched per code | host + game image | `host/user_gecko.cpp`, `host/user_gecko_targets.h`; `shim/mu_gecko.c` `gecko_store`/`gecko_load`; ini `geckocode`, `geckochosen` [V] | yes; offline only, suspended online and in replays [V `host/user_gecko.h`] | large (function patches overwrite x86-64 machine code from a table generated from the Windows and Linux images; an AArch64 table and patcher are needed) | not wired |
| Gecko code kinds accepted | RAM writes 00-06, serial 08, pointer forms 10-18, 42/4A/44/4C, ifs 20-2E, registers 80-88 when every byte lands in one proven game variable; code writes that reduce to "function returns N" [V `host/user_gecko.h`] | host | same | yes | (part of the row above) | not wired |
| Gecko code kinds refused | C0/C2/C6 PowerPC injection, 40/48 pointer loads, 60-68 loops, A0-AE compares, writes outside proven variables [V] | host | same | n/a | none | upstream stub (refused by design on the Source Port) |
| 20XX TE: master switch and Tournament Mode | TE features on; Tournament Mode keeps the tournament-safe set | game image + host | `MU_GAME_OPTION_TE` 0x10, `MU_GAME_OPTION_TE_TOURNAMENT` 0x20; sent only with a recognised TE `.gci` in the mod profile [V SH]; ini `te_options`, `te_options2`, `mod_te_enabled`; CLI `--te-options`, `--te-options2`, `--mod-gci` | yes; "Offline only: the game ignores every one of them in an online match and in replay playback" [V MH] | small to medium (bits, commands 0xF8 and 0xF7, save detection) | active offline: the released TE save in a profile sets MU_GAME_OPTION_TE (`mod_te_enabled 0` keeps it off); commands 0xF8 and 0xF7 served (frontend/README.md, Mod profiles) |
| 20XX TE word 1: hold Start to pause, frozen stages, no Star KO, infinite shields, taunt cancel, play after GAME!, fixed camera, CPU Zelda as Sheik, handicap sets stocks | | game image | `MU_GAME_OPTION_TE_*` 0x40..0x4000 [V MH]; `shim/mu_te.c` | yes; offline only | small | active offline: set from TE's own settings menu or `te_options` (hex), kept in settings.ini.state; the bits' effects in a match not looked at |
| 20XX TE word 2: neutral spawns, v1.00 rules, quieter DL64, reset tournament rules, Y toggles frozen, skip results, random music, shield colours on L/R, no screen rumble, TE L-cancel flash, spoof plugins, collision bubbles, CPU smart DI, colour overlays, hand-warmers, stage striking | | game image (quieter DL64 is host: jukebox gain) | `MU_GAME_OPTION2_TE_*` [V MH]; `shim/mu_te.c`, `shim/mu_te_menus.c`; `abi/mu_lcancel_flash.h` | yes; offline only | small | active offline: as word 1 (`te_options2`); effects not looked at |
| 20XX TE input display, lock settings | Host-only bits | renderer / host | `MU_GAME_OPTION2_TE_INPUT_DISPLAY`, `_LOCK_SETTINGS` [V MH] | no | medium (overlay) | input display active: the bit in the word the game reads (offline matches and replays) draws one pad per player (`frontend/test-overlays-linux.sh te`, TE save profile, `te_options2 2000`); lock settings is the game's own (mods.cpp 0xF8) |
| 20XX TE in-game settings menu | VS Mode > Tournament Melee opens a native TE settings menu | game image + host | `shim/mu_te_debugmenu.c`; command 0xF8 [V] | n/a | small | active offline: VS Mode > Tournament Melee opens it; changes arrive through 0xF8 (Linux screenshots) |
| 20XX TE menu music from Sound Test | | game image + host | command 0xF7; ini `te_menu_music` [V `shim/mu_te.c`] | no | small | wired: command 0xF7 and `te_menu_music`; not tried |
| 20XX TE L-cancel training wheels | The host strips the bit ("the port's Auto L-cancel does that") [V SH] | game image | `MU_GAME_OPTION2_TE_LCANCEL_WHEELS` | yes | none | upstream stub |
| 20XX TE memory-card replay browser, eight-character tags, save extensions | Not ported [V `docs/training-mods.md`] | n/a | none | n/a | none | upstream stub |
| 20XX CPUs | CPUs wavedash, shine, short-hop laser, recover with chosen angles | game image | `MU_GAME_OPTION2_TE_20XX_CPUS` 0x00800000; ini `cpu_20xx` [V `shim/mu_20xx_ai.c`] | yes; offline only, recorded in replays | small | active (settings.ini, settings.cpp; seen in the game's log or a screenshot, frontend/README.md) |
| CPU training options | CPU tech, getup, DI, SDI choices; no taunt; always L-cancel; Falcon no rapid jab; Zelda/Sheik no transform | game image | `MU_GAME_OPTION3_CPU_*` 0x00007FFF [V MH]; `shim/mu_20xx_cpu.c`; ini `cpu_tech`, `cpu_getup`, `cpu_di`, `cpu_sdi`, `cpu_notaunt`, `cpu_lcancel`, `cpu_nojab`, `cpu_notransform`; CLI `--cpu-training` | yes; offline only, recorded as `muOptions3` | small | active (settings.ini, settings.cpp; seen in the game's log or a screenshot, frontend/README.md) |
| Vanilla-only session | Retail gameplay only, separate memory card, mods and codes refused | host | CLI `--vanilla-only` [V `host/gameplay_policy.h`] | yes (forces retail) | small | active: `vanilla_only 1` in settings.ini or `--vanilla-only`: no mod profile, no TM-CE, CPU, 20XX, Stadium, Training extras or Practice CPU, the memory card folder `<card>/VanillaOnly`, replays recorded with gameplay mods refused, and a disc whose header is not GALE01 revision 2 refused (upstream also compares every disc file; here mods are separate files and never the disc). `frontend/test-mods-linux.sh vanilla-only` (with `mod_profile hackpack` and CPU options in settings.ini: profile not loaded, option words 0, a VS match saved to `card-play/VanillaOnly`) and `vanilla-refuse`; the built-in code set stays on (as upstream; `vanilla 1` takes it off). The replay refusal was not run |
| Code-free retail game | No General Codes at all, for parity runs | game image | `MU_GAME_OPTION_VANILLA`; CLI `--vanilla-game` | yes | none | active (the runner passes it on request) |

---

## 5. Training tools

All are offline only by upstream policy [V headers of `shim/mu_lab.c`, `abi/mu_practice_cpu.h`,
`shim/mu_tmce_codes.c`].

| Feature | What the player gets | Side | Switch that turns it on | Sim | Effort | Status |
|---|---|---|---|---|---|---|
| Practice menu | Controller menu in Training (Start + D-pad Up + Z): CPU, Fight a pro, Tools, Search; four looks | renderer + host | `gx/practice_menu.cpp`; `host/settings_chord.h`; ini `practicestyle`, `practiceopens`, `matchmakinghint` | no | large (menu must be redrawn in our overlay) | not wired |
| Practice CPU | Odds for the CPU's movement, attacks, defence, out-of-shield options, DI, techs, getups, ledge and recovery; presets and three slots | game image + host | `abi/mu_practice_cpu.h` (`MuPcConfig`); `shim/mu_practice_cpu.c`; `hle/practice_cpu.cpp`; command 0xF6; ini `practicecpu`, `practicecpuslot1..3` | yes; offline Training only | medium | active: `practicecpu_on 1`, `practicecpu_preset N`, `practicecpu <hex>`; Training page rows; the three slots (`practicecpuslot1..3`, upstream's hex form): the Training page's "save to slot N" / "load slot N" (`frontend/test-mods-linux.sh training-menu`: saved, the Practice CPU turned off, loaded back, written to settings.ini, and the next start's Training match ran the slot's settings: `practice CPU on (preset own)`) |
| Save states in Training | D-pad Right saves the whole game, Left loads | game image + host | `MU_GAME_OPTION_LAB` 0x8000; commands 0xF2 / 0xF3 on `hle/native_savestate.cpp`; `shim/mu_lab.c` [V] | yes; offline only | medium (shares the rollback snapshot) | active: offline Training, hot-page store (runner/features/snapshot_store.*); `lab 1` |
| Record and play back the CPU | D-pad Down records, Up plays back; optional loop | game image | `MU_GAME_OPTION_LAB`, `MU_GAME_OPTION_LAB_LOOP` 0x04000000 | yes; offline only | small after save states | active: `lab 1`, `lab_loop 1` (game side) |
| Collision bubbles | Hitboxes and hurtboxes over the model or alone | game image | `MU_GAME_OPTION_LAB_BUBBLES` 0x10000, `_BUBBLES_ONLY` 0x20000 | no | small | active: `lab_bubbles 1` (over the model) or `2` (bubbles only) |
| Frame data and input overlay | Frame advantage and inputs on screen | renderer + host | `MU_GAME_OPTION_LAB_OVERLAY` (host only); command 0xF4; `gx/training_overlay.cpp` | no | medium | active: `lab_overlay 1` in offline Training draws a panel per fighter (action name, frame, percent, speed, its pad) and the last frame advantage from 0xF4, green or red (`frontend/test-overlays-linux.sh training`: "Frame advantage -6 on hit" on screen) |
| Lab dummy DI, after-hitstun reaction, tech choice | Bits exist; the host never sends them ("Training Mode CE and the practice CPU replace them") [V SH] | game image | `MU_GAME_OPTION_LAB_DI_MASK`, `_REACT_MASK`, `_TECH_MASK` | yes | none | upstream stub |
| Frame advance | L + R + Z freezes Training, Z steps one frame | host + game image | `hle/native_practice.cpp` `frame_advance_tick`; game op `MU_PRACTICE_FRAME_PAUSE` [V MH] | yes; Training only | small to medium | active: `frame_advance 1`; L+R+Z freezes offline Training, Z steps, D-pad moves the CPU while frozen |
| Fixed or frozen random seed | Items, turnips and hammers come out the same | host + game image | `MuPcConfig.rng_mode` / `rng_seed` (`MU_PC_RNG_GAME`, `_SAME`, `_FROZEN`) in `abi/mu_practice_cpu.h`, applied by `shim/mu_practice_cpu.c` `pc_rng` [V]; ini `practice_rng`, `practice_rng_seed` | yes; offline Training only | small | active: `practice_rng 1` (the same seed at each Training start and after a state load) or `2` (frozen), `practice_rng_seed 0..255`, and the Training page's two "Random numbers" rows; the host sends the game's own mode online, in a replay and in a vanilla session. `frontend/test-mods-linux.sh training-menu` (set through the menu, then a Training match: `[pc] rng seed DAA66D14 (same each start)`) and `frozen` (`[pc] rng seed 8FF3473A (frozen)`); whether the items then come out the same was not looked at |
| Fight a pro (Phillip) | The CPU plays as Zain's Marth, Cody Schwab's Fox or aMSa's Yoshi | host + game image + external | command 0xEA (`abi/mu_ghost_ai.h`); `hle/phillip_ai.cpp`: ONNX policies on onnxruntime, `ProAI/*.onnx` + `catalog.json`, downloaded; ini `proaistronger`; tag robot `shim/mu_pro_tag_robot.c`, `hud_pro_tag` | yes; offline only | large (an inference runtime for AArch64 Horizon inside the frame budget; upstream expects AVX) | not wired |
| Practice ghost AI | Hand-written GRU from `ghost_ai.bin`; model files are not in the upstream repo | host | `hle/ghost_ai.cpp` | yes; offline only | medium | upstream stub (no models shipped) |
| Pro habit tables | Mang0/Falco, Zain/Marth, Cody/Fox; aMSa/Yoshi row is all zero, Jigglypuff never offered [V `hle/practice_ghosts.h`] | host | same | yes | small | upstream stub (two rows) |
| Training Mode CE events | Event Match becomes TM-CE: Lab, L-Cancel, Ledgedash, Wavedash, Tech Chase, Combo, Attack on Shield, Reversal, SDI, Powershield, Ledgetech, Amsah Tech, Shield Drop, Waveshine SDI, Slide Off, Grab Mash, Tech Counter, Edgeguard, Sweetspot, Laser Land, Escape Sheik, Escape D-throw Knee, Float Cancel, Eggs-ercise, Slalom, Multishine, Reaction, Ledgestall [V `sourceport/game/tmce/src/events.c`] | game image + host | `MU_GAME_OPTION_TMCE` 0x08000000, set when the mod overlay has `/tm/eventmenu.dat` [V SH]; ini `mod_tmce_enabled`; the player supplies a TM-CE 1.4 disc | yes; offline only | medium (needs the mod overlay, section 6) | active offline: with the TM-CE disc, Event Match opens TM-CE's event select (TM-CE v1.4 d2) and the Eggs-ercise event played on Onett (Linux screenshots); other events not tried |
| Training Mode CE on-screen displays | Wavedash, L-Cancel, Act OoS, Dashback, Powershield, SDI, lockout timers, fastfall, frame advantage, combo counter, grab breakout, ledge and more [V `sourceport/game/tmce/src/osds.h`] | game image | same | no | none after the row above | partial: as the row above |
| TM-CE console global OSD shortcut | Not ported [V `release/RELEASE_NOTES_0.8.5.md`] | n/a | none | n/a | none | upstream stub |
| Practice from replays | Take over from any moment of a replay, jump between tips | game image + host + renderer | see Replay takeover and coach in section 3 | yes | medium | active: X / Y jump to the shown player's last / next tip (a second before), A takes over there; `replay_try_length` 0 ends a try when the combo drops (or after 10 s without a hit), 1 and 2 after 10 or 20 s, then the try's result shows for 2 s and the next try starts. `test-replays-linux.sh practice`: tip 1 jumped to, taken over, two tries ended by the rule and retried, back to watching with 0 differing records |
| Lossless adapter lag test | Measures adapter and display lag with rumble | game image + host | `shim/mu_lagtest.c`, `host/lag_test_codes.h`; CLI `--lag-test`; ini `lagtestnext`; env `MELEE_LAG_TEST` | n/a | medium | not wired |
| Display latency test | X + D-pad Down paints black/white | renderer | ini `latencytest` | no | small | active: `latencytest 1` (Overlays page): X + D-pad Down paints the picture black, again white, X + D-pad Up ends, from the pads the game read for that frame (`frontend/test-overlays-linux.sh latency`) |

---

## 6. Mods, added content, cosmetics, music

| Feature | What the player gets | Side | Switch that turns it on | Sim | Effort | Status |
|---|---|---|---|---|---|---|
| Mod profiles and disc-file overlay | Layers of patched discs, loose folders and saves replace disc files; later layers win; conflicts listed; each profile has its own memory card | host | `host/mod_profile.cpp`, `host/mod_scan.cpp`, `host/source_mod_overlay.cpp`; CLI `--mod-iso`, `--mod-dir`, `--mod-gci`, `--mod-profile`, `--no-mod-autodetect`; ini `mod_profile`, `mod_enabled <key>`, `mods_in_direct`; `mod_flags()` bits ASSETS_PRESENT 0x1, HACKPACK 0x2, ROGUE 0x4, MAKER 0x8 [V MH] | yes; only files apply, a mod's code never runs [V SH] | medium (hashing and archive reading replace bcrypt and `tar.exe`) | active: `mod_profile NAME` (frontend/src/mods.cpp): patched discs compared file by file with the retail disc, later layers win, a memory card per profile; no loose folders, no conflict list |
| Content view switch | Offline: mod files. Unranked/Teams/Party: retail files. Direct: mod only with a matching build | game image + host | command 0xF5; `shim/mu_content.c` [V] | yes (the rule itself) | small after the overlay | active: command 0xF5; an online mode that plays the retail game gets the retail files back |
| m-ex content | Extra costumes, stage rows, character-select icons and menu rows from an m-ex disc | game image | reads `MxDt.dat`, `MxMn.dat`; `shim/mu_mex.c`, `shim/mu_mex_menu.c`, `shim/mu_mex_stage.c` | yes (content) | small after the overlay | active: Akaneia and ACE discs show their character select rows (Linux screenshots) |
| m-ex stages | 43 stage descriptors: Peach's Castle 64, Saffron City 64, Sector Z, Mushroom Kingdom 64, Hyrule Castle 64, Planet Zebes 64, Metal Cavern, Boxing Ring, Swordplay, Gamecube, Dolphin Park, Fichina, Village, Casino, Saturn Valley, Chocolate, Marquee, Smashketball, Maze, Tourney Floats, Volleyball, Delfino, Sprout Tower, Beat Block, Green Hill, Luigi's Mansion, Wario's Castle, Celestial, Rainbow Ride, The Pit 2, template stages, target tests | game image | `MU_MEX_STAGES` ON; `sourceport/game/mexstages/` | yes | small after the overlay | active offline: built into the game image (`MU_MEX_STAGES` ON) and on with an m-ex disc's profile: the Akaneia disc's 25 added stage rows all playable and 18 shown on its 2-page stage select, ACE 2.0.1's 78 rows playable and 68 shown on 5 pages, none locked (`[mex] stages: 78 added, 78 playable, 0 locked`); on the Akaneia disc's Training stage select R turned to page 2, the cursor picked Delfino Plaza (`icon 30 picked (ext 301)`, `ext 301 (internal 84, Delfino Plaza) starts`) and the Training match played on it (screenshot) (`frontend/test-mods-linux.sh mex`, `MU_MEX_PROFILE=ace`). Other added stages and VS matches on them not played |
| Akaneia fighters | Wolf, Diddy Kong, Charizard, Lucas, Sonic, King Dedede, Tails; VS, Special, Training and Direct only | game image + host | `MU_AKANEIA_FIGHTERS` ON; `sourceport/game/akaneia/`; the host accepts an Akaneia disc only if the image exports `mu_ak_native_build` [V SH] | yes; Direct only online | small after the overlay | active offline: the Akaneia disc profile; its fighters on the character select and a VS match against Wolf (Linux screenshots); online gets the retail files |
| ACE fighters | 25 added fighters (folders under `sourceport/game/ace/`) | game image | `MU_ACE_FIGHTERS` ON | yes | small after the overlay | upstream stub ("ACE remains experimental and its VS route is not validated" [V release notes]); here: the ACE 2.0.1 disc loads, its fighters show on the character select, and Shadow (an ACE fighter) stood in a Training match on Fourside; VS not played |
| ACE Games menu | Volleyball plays; All-Star Versus, Tag, Turbo are dimmed [V `shim/mu_mex_menu.c`] | game image | same | yes | small | upstream stub (all but Volleyball) |
| Rogue Mode | rogueMelee's roguelike under 1-P, offline | game image + host | `MU_ROGUE` ON; `MU_MOD_ROGUE` when the mod disc id is `GALERM` [V `sourceport/game/rogue/mu_rogue.c`, SH] | yes; offline | small after the overlay | active offline: with the rogueMelee disc's profile (mod flags 00000005), 1-P Mode > Regular Match > All-Star starts Rogue Mode's run: the map (ACT 1 / 3, FLOOR 1 / 15, encounters, "Pokemon Stadium / Duel, STANDARD DUEL") and the first fight with its HUD (`SHIELD 1`) (`frontend/test-mods-linux.sh rogue`: `[rogue] setup stocks=3 difficulty=2`, `[rogue] native_route_commit`, `[rogue] match floor=0 recipe=1 stage=6 enemies=1`, screenshots); a whole run not played |
| Melee Maker | Borrowed moves on any character | game image + host | `MU_MOD_MAKER`; command 0xEE (data read, build id, a 32 MB cache mapped right after main memory) [V MH] | yes; Direct only, build id compared | medium (fixed-address mapping on Horizon) | active offline (0.4.1, Linux): the disc sets MU_MOD_MAKER (mod flags 00000009), its bone tables come through 0xEE; Z on the character select opens BUILD A FIGHTER, X randomised Falco's specials to Samus's Charge Shot, Ness's PK Fire, Puff's Sing and Marth's Counter, and in the match Falco's neutral B was the Charge Shot and his up B Sing (screenshots); no 32 MB cache (the match heap holds the moves); Direct between Maker builds not tried |
| 20XX Hack Pack: stage variants and stage select pages | 4 pages, 16 variants per legal stage, random variants | game image + host | `MU_MOD_HACKPACK`; command 0xFA ops DOL_READ, GET/SET_SETTING, STAGE_STATE, STAGE_TABLE, CPU_WORD [V MH]; `shim/mu_hp.c`; ini `hp_settings` | yes; offline only, recorded in option word 3 | small after the overlay | active offline (0.4.1, Linux): D-pad down/up on the stage select changes the page (`stage select page 2`, the page's own icons in a screenshot); the debug menu's TEXTURES page set Battlefield to variant 2 and the match loaded its letter-2 file (`pick: stage 31 page 1 -> 31, letter '2'`, screenshot); a page-2 pick of Battlefield played the same variant; random variants not tried |
| 20XX Hack Pack: music playlists | Numbered tracks, 11 playlist slots, 5 types | game image + host | same | no | small after the overlay | active offline (Linux, play mode): the debug menu's CUSTOM MUSIC PLAYLISTS page set Final Destination to RANDOM - CUSTOM ONLY and was saved (`debug menu saved: 5 change(s)`, `hp_settings` in settings.ini.state); with the global playlist on (RANDOM - CUSTOM ONLY) Final Destination's song 27 played custom track `AF.hps` in Training and `CE.hps` in a VS match where the default single song is `30.hps` (`music 27 -> 30.hps`) |
| 20XX Hack Pack: debug menu, training displays, hidden characters | General, Training, Stage mods, CPU, Music, Mechanics pages; colour overlays; Z on an icon swaps in Sheik, Giga Bowser, solo Popo, wireframes, Master and Crazy Hand | game image | `shim/mu_hp_menu.c`, `shim/mu_hp_train.c`, `shim/mu_hp_css.c` | yes for characters; offline only | small after the overlay | active offline (Linux, play mode): VS Mode > Tournament Melee opens the native debug menu (its eight pages); training displays: its COLOR OVERLAYS page set P1 and P2 on, hitstun red, auto-cancel yellow, IASA green, and in Training the hit CPU turned red in hitstun (screenshot; `overlay hitstun: player 2, color 4`, `overlay IASA: player 1, color 11`); hidden characters: Z with the coin held over Bowser gave Giga Bowser (`character select: icon 3, character 5 -> 29`, the door reads G-Bowser) in Training and in VS Melee, and a VS match against a CPU on Final Destination started with Giga Bowser falling in (`fighter kind 31 enters falling`, screenshot). In VS stock singles upstream's Squad Strike took the Z press for itself (it adds the hovered fighter to a squad, `squad_buttons`), so the pack's swap never saw it; `patches/0005` leaves Z to the pack on its seven paired icons when the pack is loaded (no change without the pack: the five reference scripts IDENTICAL with the patched image) |
| Per-profile and vanilla-only memory cards | Mod sessions never write the ordinary card | host | SH card path rules [V] | no | small | active: a card folder per profile (`card-NAME`) and the vanilla-only session's `<card>/VanillaOnly` inside the ordinary card folder, never copied or cleared (`frontend/test-mods-linux.sh vanilla-only`: the match's save in `card-play/VanillaOnly`) |
| Costume, stage and effect imports | Import costume DAT/ZIP, stage DAT, effect variants, pause screens, portraits, Nucleus vault ZIPs; previews | host + renderer | `host/cosmetic_mods.cpp`; CLI `--import-cosmetic`, `--select-cosmetic`, `--deselect-cosmetic`, `--cosmetic-status`, `--restore-vanilla-cosmetics`, `--enable-project-effects`, `--scan-disc-skins`; `docs/cosmetic-imports.md` | no by validation: a costume stays on online only with the standard skeleton; stages pass `host/stage_dat_safety.h` [V] | large | not wired: upstream's importer (`host/cosmetic_mods.cpp`, 7.8k lines) is written for Windows (`windows.h`, `tar.exe`, bcrypt) and the desktop launcher's file picking; on the Switch the same files come in through a mod profile today (a patched disc's costume and stage files replace the retail ones, "Mod profiles" above) |
| Skin cycling on the select screens | L/R cycles skins on character select; stage skins on stage select; random stage skin per match | game image + host | commands 0xF9, 0xFB, 0xFC, 0xFD [V SH] | no | medium after imports | not wired: commands 0xF9, 0xFB to 0xFD are answered from the imported-cosmetics store above, which does not exist here |
| Voice mods | A `.ssm` voice bank per skin | host + game image | command 0xFA (voice); `docs/voice-mods.md` | no; "sounds are not part of the rollback checksum" [V] | medium after imports | not wired: a voice bank belongs to an imported skin (the row above) |
| Texture packs | Dolphin-format PNG packs; dump mode | renderer | `gx/texture_pack.cpp`; folders `Load/Textures/GALE01`, `TexturePacks`; ini `customtextures`, `prefetchtextures`, `texpackoff`, `dumptextures`; CLI `--custom-textures`, `--dump-textures` | no | medium to large (Aurora texture replacement; memory) | not wired: `gx/texture_pack.cpp` hooks upstream's own D3D11/D3D12/Vulkan back ends (`windows.h`); the Switch draws through Aurora, which has no texture replacement, and a pack's PNGs need a decoder and memory the console's budget has to allow |
| Game music | The game's own songs | game image | the game streams from the disc when env `MELEE_NO_JUKEBOX` is set, which the Linux minimal host does itself [V `shim/mu_audio.c`, `port/linux/host_services.cpp`] | no | none | active in the simulation; there is no sound output yet |
| Music volume | 0 to 100% | host / game image | `music_volume()` [V MH]; ini `music`; CLI `--music`, `--no-music` | no | small | active (settings.ini, settings.cpp; passed to the game, its effect not looked at) |
| Custom music packs / jukebox | Replace menu and stage songs with MP3, WAV, M4A, FLAC, OGG or HPS; folder playlists | host | `hle/jukebox.cpp`; commands 0xD6, 0xD7, 0xD8, 0xEB; folder `MusicPacks/`; ini `musicpacks` | no | medium (HPS decode is portable; other formats need a decoder) | not wired: `hle/jukebox.cpp` decodes through Media Foundation on Windows and `full/linux_media.h` elsewhere; neither is on the Switch. The game's own songs play, and a Hack Pack disc's custom HPS tracks play through its playlists (20XX rows above) |
| Look presets | 15 looks: Light, Lighter, Diet Classic, Diet 64, Crystal, Wireframe, Potato, Stick Figures, Capsules, Minimal, 64 Style, Silhouettes, Classic Style | renderer | `gx/gx_retro.h`, RO look fields; ini `look_*`; CLI `--look`, `--retro` | no | medium to large | not wired: `gx/gx_retro.h` is applied in upstream's own shader generator and back ends (`gx_core.cpp`, `gx_shader.cpp`, D3D/Vulkan); the Switch draws through Aurora's pipeline, which would need the same looks written for it |
| Lightweight levels and visual effects tiers | Cheaper rendering in steps; fewer particles | renderer (+ game image for low poly) | ini `lightweight` 0..5, `effects` 0..3; CLI `--lightweight`, `--effects` | no | medium | not wired |
| "Get mod looks" | Installs Diet Melee and Animelee built from the player's disc | launcher / external | PS; env `MELEE_BUILD_LOOKS` | no | medium (do it in our PC installer [I]) | not wired |
| Arcade look: attack and projectile hitboxes | Hitboxes on every fighter | game image | `MU_DISPLAY_OPTION_HITBOXES` 0x20, `_PROJECTILE_HITBOXES` 0x40, `_HITBOXES_BEHIND` 0x80; ini `arcade_hitboxes*` | no; never sent online [V SH] | small | active: `arcade_hitboxes`, `arcade_projectile_hitboxes`, `arcade_hitboxes_behind` (offline; screenshot in frontend/README.md) |
| Arcade look: grid or plain background | Grid every five units behind the stage | game image + renderer | `MU_DISPLAY_OPTION_GRID` 0x100, `grid_color()`; ini `arcade_backdrop`, `arcade_grid_rgb`, `arcade_background_rgb` | no | small (grid), medium (hide scenery) | partial: the grid (`arcade_backdrop 2`, `arcade_grid_rgb`); hiding the scenery and the plain backdrop are renderer work, not wired |
| Arcade look: 2D camera | Flat projection | game image offline; renderer online | `MU_DISPLAY_OPTION_ARCADE_CAMERA` 0x200; ini `arcade_camera_2d`; `shim/mu_arcade.c` | no | small offline, medium online | active offline: `arcade_camera_2d`; online not (upstream does it in its renderer there) |
| Arcade look: combo counter | "N HITS" over the attacker's damage | renderer | `hud_combo` [V MH]; ini `arcade_combo_counter` | no | medium | active: `arcade_combo_counter 1` (Overlays page), offline: "N HITS" over the attacker's damage, counted per game frame from `hud_player`/`hud_combo` as upstream counts (`frontend/test-overlays-linux.sh match`, `training`: up to "10 HITS") |
| 2D-style m-ex build | Its camera and HUD codes run natively | game image | `MU_GAME_OPTION2_ARCADE_PACK` 0x02000000, set from a disc fingerprint [V SH] | camera; offline and its replays | small after the overlay | not wired |
| Lab view | The match as flat Slippi-Lab-style silhouettes (Ctrl+F3) | renderer | `gx/lab_view.cpp`; assets in `Lab/` (downloaded); ini `labview`, `look_lab_*`, `lab_colour_*`, `labskipscene`; CLI `--lab-view`, `--lab-dir` | no | medium | not wired |
| Looping video backgrounds | MP4 behind the character and stage select | renderer | `gx/video_background.cpp`; ini `videobackgrounds` | no | large (an H.264 decoder on the console) | not wired |
| Border art | A picture or video in the side bars | renderer | ini `borderart`, `borderedge`; CLI `--border` | no | small (picture) | active: `borderart grid` (built-in) or a .bmp in `<folder>/Borders` (`borderedge` soft shadow), drawn in the side bars; no video, no PNG (`frontend/test-overlays-linux.sh picture`, `studio`) |
| Built-in CRT look | Studio monitor or home television, screen curve | renderer | `gx/crt_shader.h`; ini `crt_filter`, `crt_curve`; CLI `--crt` | no | small to medium | active: `crt_filter 1` studio monitor, `2` home television with `crt_curve` (Picture page), in Aurora's present pass (patches/0109): beam per scanline of 480, phosphor mask, curve and glow after upstream's model; at 720p the scanlines ease off as upstream's do (`frontend/test-overlays-linux.sh picture`, `studio`). The app's screen filter row stays too |
| RetroArch shader presets | `.slangp` presets over the picture | renderer | `gx/user_shader.cpp` (librashader); ini `shaderpreset`; CLI `--shader` | no | large (no librashader build for our stack) | not wired |
| Easter egg | A button sequence on character select sends bouncing dog faces | renderer + game image | `gx/walter_egg.cpp`; command 0xDA | no | small | not wired |

---

## 7. Controllers

| Feature | What the player gets | Side | Switch that turns it on | Sim | Effort | Status |
|---|---|---|---|---|---|---|
| Controller input to the game | Four pads as `MuPadStatus` | host | `pad_read` [V MH] | inputs | none | active: the app's input path (frontend/src/play_io.cpp); console: menus and a real match played with the controllers |
| GameCube adapter | Official adapter, rumble, origin reset | host | `host/gc_adapter.cpp` (libusb; 0x13 start, 0x11 rumble) | inputs | small (this repo's app has an adapter path) | active: the app's adapter code (gc_adapter_usbhs.cpp on the console); rumble through it |
| Adapter polling-rate readout | Measured from received reports | host + renderer | `host/gc_adapter.cpp` | no | small | partial, wired: `adapterrate 1` shows the GameCube adapter's report rate as the app's usb:hs code measures it (reports per second, the endpoint's polling interval) and, below it, each Pro Controller, Joy-Con or Switch Online GameCube controller with the rate at which the system's HID hands this program new states (new sampling numbers per second, `controls_rate_lines`, frontend/src/controls.cpp). The Linux test build reads neither ("console only" lines); compiled for the console, not yet seen there |
| Xbox, DualShock 4, DualSense, Switch Pro, Joy-Con, generic HID pads | Other pads as GameCube controllers | host | `host/window.cpp`, `host/playstation_pad.cpp`, `host/switch_pro.cpp`, `host/hid_pad.cpp` | inputs | small (libnx HID replaces Raw Input) | active as the app has them: SDL gamepads through Aurora's GameCube mapping (Pro Controller and Joy-Con on the console) |
| Keyboard | Keyboard as a controller | host | `host/input_bindings.h` defaults | inputs | small (USB keyboard through libnx) | Linux only (the app's SDL keyboard layout); not on the console |
| Remapping | 27 actions including Mod X, Mod Y, light and mid shield, stick modifier | host + renderer | `host/input_binding_types.h`, `host/input_bindings.h`; ini binding tables | inputs | medium (needs a screen) | active: all 27 upstream actions (frontend/src/controls_core.h, adapted from upstream's input_bindings.h). SDL controllers: the Controls page's twelve buttons plus the *Controller bindings* page (stick and C-stick directions, L / R travel from a trigger or a button with a level, Mod X, Mod Y, light and mid shield, stick modifier); adapter ports: all 27; stick and C-stick inversion per axis on *Controller setup*; launcher.ini `padx.<id>`. Checked: `tools/controls_check.sh` (65 checks) and `test-app-linux.sh controls` (port 1 read as adapter port 1: A captured onto Z through the menu and then Z gives A, a saved X/Y swap, the stick inverted; launcher.ini `padx.gc1=m0=16,m2=2048,m3=1024,inv=1,rad=90`). SDL controllers on the console: not yet tried |
| Controller profiles | Default / New / Save / Delete per device kind | host | `host/controller_profiles.cpp`; folder `ControllerProfiles/` | inputs | medium | active: *Controller profiles* page, `<app folder>/ControllerProfiles/NAME.profile` in upstream's format (default setup, save as new, save over, load, delete); `gcadapter` profiles use upstream's numbers and load both ways, SDL controllers get their own `sdlgamepad` kind. Checked: `test-app-linux.sh controls` ("GameCube 1" saved from the menu; a second start without the setup loads it from the menu and X reaches the game as Y again) and `controls_check` (round trip, an upstream-written gcadapter profile, another kind's file refused) |
| Dolphin profile import | Import a Dolphin GCPad profile | host | `host/dolphin_profile.cpp` | inputs | small | partial, wired: a Dolphin `Profiles/GCPad/*.ini` or `GCPadNew.ini` ([GCPad1]..[GCPad4]) copied to `<app folder>/Dolphin/` is listed on the profiles page and imported onto the selected SDL controller (SDL and XInput devices; `A \| B`; stick axes as swap and inversion; dead zone, threshold, modifier range, radius), then saved as profile "Dolphin NAME"; keyboard profiles refused (no keyboard on the console). Parsing checked by `controls_check` (XInput, SDL with swapped sticks, keyboard refused, GCPadNew listing); the import onto a real controller needs an SDL controller, so only the console can show it |
| Deadzone, radius, snapback filter, trigger cap and click point | Per controller family | host | `host/input_bindings.h`; ini `deadzone_*`, `radius_*`, `snapback_*`, `trigger_*` | inputs | small to medium | active, per controller (finer than upstream's per family): round deadzones in the game's units, stick and C-stick radius, snapback filter per axis with its window (SDL controllers; upstream leaves GameCube controllers out too), the adapter's L / R click point, L / R trigger caps; upstream's order (snapback, deadzone, radius, click, cap). The Controls page keeps an SDL controller's percent deadzones and press point. Checked: `controls_check` (each filter) and `test-app-linux.sh controls` (radius 90 %: a full push reaches the game as -114 with the inversion) |
| Box controllers | B0XX (vJoy), HayBox, Frame1 layouts with SOCD and modifiers | host | `host/box_melee.h` | inputs | small if the device enumerates as HID | partial, wired: upstream's box mode (box_melee.h compiled from the fetched tree: B0XX coordinates, second-input priority without reactivation, Mod X + Mod Y D-pad layer, light and mid shield) on any SDL controller or adapter port that has Mod X or Mod Y bound. The console's HID offers no DInput, vJoy or keyboard device, so upstream's named layouts (HayBox DInput, Frame1 DInput, vJoy B0XX) cannot appear; a box in GameCube mode on the adapter or in the system's Pro Controller mode sends firmware-made analog values and needs nothing. Checked by `controls_check` (coordinates, SOCD, D-pad layer) |
| PhobGCC settings on a stock controller | Calibration, notch fix, snapback, waveshaping, smoothing, cardinal snapping, radius scaling, button remap, 9 trigger modes, tourney lockouts, Setups | host + renderer | `host/phob.cpp`, `host/phob_store.cpp`, `gx/phob_menu.cpp`; folder `PhobGCC/` | inputs | medium to large | not wired, but applicable: `phob.cpp` is plain C++ (GPL-3.0-only, from PhobGCC) working on the adapter's raw 37-byte reports, which the console's usb:hs reader also receives at a 1 ms poll. Porting it needs the raw reports out of the app's adapter reader (a patch to its build-time copy), the firmware's 1 kHz step on that reader's thread, and its 44-step stick calibration as a menu flow; none of it can be checked without a stock controller on the console, so it was left for a pass with hardware |
| PhobScope and Phob Check | Input viewer, stickmaps, value-over-time plots, button timing, reaction test, oscilloscope, CSV export | renderer + host | `host/phobscope.cpp` | no | medium | not wired, but applicable: `phobscope.cpp` measures 1 ms samples of the adapter's reports (the same raw-report access as PhobGCC above) and its plots would draw through the overlay's ImGui pass; not done in this pass. The input display overlay (`inputoverlay 1`) is the nearest thing that exists |
| "Play as P2/P3/P4" | Assign an idle pad to a free port | host + renderer | PS Controls page; ini `port0..3`, `portname0..3` | inputs | small | active in the app's form: a controller's port on the Controls page (`pad.<GUID>=<port>;...`) |
| Rumble and its switch | Rumble on the adapter and pads | host | `pad_rumble` [V MH]; ini `rumble` | no | small | active: rumble per controller (Controls page), the adapter's rumble |
| Button chords | Start + D-pad Down + Z opens settings; Start + D-pad Up + Z opens Practice | host | `host/settings_chord.h` | no | small | active: settings_chord.h compiled from the fetched tree: Start + D-pad Down + Z opens or closes the in-game menu, Start + D-pad Up + Z opens it on the Training page (patches/0111 `menu_open_extra`), the chord's buttons kept from the game while held, offline Training's lone-Start wait; L + R + Start (half a second) still opens the menu. Checked: `test-app-linux.sh controls` (the chord opens the menu, `controls: Start + D-pad Down + Z on port 1`); the D-pad Up chord and the Training wait not run on Linux |
| Background input | Pads read while the window is unfocused | host | ini `backgroundinput` | no | n/a | not applicable on the Switch (one foreground program) |

---

## 8. Display

All are picture only.

| Feature | What the player gets | Side | Switch that turns it on | Sim | Effort | Status |
|---|---|---|---|---|---|---|
| Picture from the GX stream | The game on screen | renderer | `gx_fifo`, `vi_*` [V MH]; `frontend/` (this repo) | no | none | active |
| Aspect ratio | 73:60, 4:3, 16:9, stretch | renderer | ini `aspect`; CLI `--aspect` | no | small | active: `aspect 0..4` (Picture page, live): Auto, 73:60, 4:3, 16:9, stretched; the app's Aspect ratio row (16:9 code, next start) stays (`frontend/test-overlays-linux.sh picture`: 73:60) |
| V-Sync | Tearing against latency | renderer | ini `vsync`; CLI `--vsync` | no | small | active (`--novsync` turns it off) |
| Quality presets | Low / Medium / High / Ultra / Insane / Custom | renderer | ini `custompreset` | no | small once the parts exist | active: the Picture page's Quality preset row sets internal resolution, sharpening and launcher.ini msaa/anisotropy: Low (640x480, no AA, 1x), Medium (1280x960, 4x), High (Auto, 16x, the default), Ultra (High with 4x MSAA and 30% sharpening); "Custom" when the parts differ. MSAA and anisotropy at the next start |
| "Low spec" switch | One control for the cheapest settings | renderer | old ini keys `lowspec*`; now Lightweight level 4 [V reading pass] | no | small | upstream stub (no control with that label in 1.0.5) |
| Internal resolution | Auto, native, 2x to 8x | renderer | ini `scale`; CLI `--scale` | no | medium | active: `scale 0..4` (Picture page, live): Auto (the display) or 640x480 times 1..4; no cropping with this bridge in the Linux test (`frontend/test-overlays-linux.sh scale1`, `scale2`: 640x480 visibly softer, 1280x960 downsampled) |
| Anti-aliasing | MSAA 2x-8x, SSAA 4x-16x | renderer | ini `msaa`, `ssaa`; CLI `--msaa`, `--ssaa` | no | medium | partial: MSAA from launcher.ini `msaa` and the menu's row (next start); supersampling through Internal resolution above the display's size (scale 2..4); no separate SSAA key |
| Anisotropic filtering | 1x to 16x | renderer | ini `anisotropy`; CLI `--anisotropy` | no | small | active: launcher.ini `anisotropy`, the menu's Texture filtering row (next start) |
| Sharpening | 0 to 100% | renderer | ini `sharpness`; CLI `--sharpness` | no | small to medium | active: `sharpness 0..1` (Picture page, live), contrast-adaptive in Aurora's present pass (patches/0109) (`frontend/test-overlays-linux.sh picture`) |
| Brightness, contrast, vibrance | | renderer | ini `brightness`, `contrast`, `vibrance` | no | small | active: `brightness`, `contrast`, `vibrance` (Picture page, live) in Aurora's present pass (patches/0109) (`frontend/test-overlays-linux.sh picture`) |
| Screen-space ambient occlusion | Contact shading from depth | renderer | ini `ssao`; CLI `--ssao` | no | medium | not wired |
| Window size, borderless and exclusive fullscreen | | renderer | ini `window`, `fullscreen`, `exclusivefullscreen` | no | n/a | not applicable on the Switch (no windows) |
| Graphics backend picker | Direct3D 12 / 11 / Vulkan / OpenGL | renderer | ini `backend`; CLI `--backend` | no | n/a | not applicable on the Switch (one renderer: Aurora on Vulkan) |
| DLSS, DLAA, XeSS, FSR upscaling | | renderer | ini `dlss`; CLI `--dlss`, `--dlss-mode` | no | n/a | not applicable on the Switch (PC vendor libraries) |
| Frame generation | NVIDIA 2x-6x, AMD 2x | renderer | ini `framegen`; CLI `--frame-generation` | no | n/a | not applicable on the Switch (PC vendor libraries) |
| NVIDIA Reflex and latency readout | | renderer | ini `reflex`, `reflexstats`, `reflexflash`; CLI `--reflex` | no | n/a | not applicable on the Switch (NVIDIA PC driver feature) |
| HDR output | HDR10 with paper white and peak | renderer | ini `hdroutput`, `hdrpaperwhite`, `hdrpeak` | no | n/a | not applicable on the Switch (no HDR output) |
| Path tracing, RTX Pack, RTX Remix, Ray Reconstruction | | renderer + external | ini `pathtracing`, `rtx*`, `rayreconstruction`; `native_rtx_semantics` [V MH] | no | n/a | not applicable on the Switch (needs ray-tracing hardware and NVIDIA runtimes) |
| DLSS 5 neural rendering | | renderer + external | ini `dlss5*`; CLI `--dlss5` | no | n/a | not applicable on the Switch (NVIDIA model and runtime) |
| Shader pipeline precompile | First launch compiles pipelines; `--prewarm-only` does it without a disc | renderer | CLI `--prewarm-only`, `--shader-cache` | no | none (Aurora's cache and `--warm-all` in `frontend/`) | active (our own cache) |

---

## 9. Audio

| Feature | What the player gets | Side | Switch that turns it on | Sim | Effort | Status |
|---|---|---|---|---|---|---|
| Audio mixing | The game's own AX mixer builds 32 kHz stereo blocks | game image + host | `ai_*`, `dsp_mail`, `aram_*` [V MH]; `hle/ax_ucode.cpp` | no | none | active (mixed and discarded) |
| Sound output | Blocks played on the device, resampled, drift-tracked | host | `host/audio.cpp`, `hle/audio_core.cpp`; `vi_idle_step` paces one 5 ms block at a time [V MH] | no | medium | active: the app's audio path (SDL, 40 ms cushion, refill after a stall); console: sound in menus and matches |
| Master volume | 0 to 100% | host | ini `volume`; CLI `--volume` | no | small | active: launcher.ini `volume`, the menu's Volume row |
| Audio buffer | 5 to 120 ms software queue | host | ini `audio_buffer_ms` | no | small | not wired: the app's fixed 40 ms cushion; no setting |
| Speed nudging for online time sync | Audio follows 0.995 to 1.01 speed | host | `audio_set_speed` | no | small | active: `set_emulation_speed` goes to the host's pacing and to the sound output's resampling ratio (frontend/patches/0110, as upstream's `audio_set_speed`); under an induced clock skew (`--net-clock-skew`) the log shows the nudges (`net: time sync: frame rate x...`) and `net check` stays equal (README, "Time sync frame rate"); the pitch change itself was not listened to (SDL's dummy device in the test) |
| Mono / Stereo | The game's own Options > Sound, kept by the host | game image + host | `sound_mode`, `set_sound_mode` [V MH]; file `sound-mode.txt` | no | small | active in memory; not saved |
| Audio mode: shared, low latency, exclusive, ASIO; output device picker | | host | ini `audio_mode`, `audio_asio_driver`, `audio_asio_buffer`, `audio_device`; CLI `--list-audio-devices` | no | n/a | not applicable on the Switch (one system audio output) |
| Menu sounds in the settings UI | | renderer | ini `menusounds` | no | small | not wired |

Upstream has master and music volume only: no separate effects or voice slider [V reading pass].

---

## 10. Saves

| Feature | What the player gets | Side | Switch that turns it on | Sim | Effort | Status |
|---|---|---|---|---|---|---|
| Memory card as a folder of .gci files | Dolphin GCI-folder format, slot A, 2043 blocks; drop in an existing save | host | `card_*` [V MH]; `hle/hle_card.cpp`; CLI `--card-dir` | unlock state | none | active (the runner empties the folder at each start for its scripted runs) |
| Automatic save backups | Timestamped copies: the 10 newest plus one per day for 30 days | host | `host/card_backup.h` | no | small | active (0.4.1): at each app start, when the card changed, a copy in `User/GC/Backups/CardA-YYYYMMDD-HHMMSS`; the 10 newest kept, older ones one per day for 30 days (frontend/src/app.cpp); plus the one-time `CardA-before-0.4` |
| Unlock everything without touching the save | Unlocks are not written into the save | game image | see section 4 | content | none | active |
| Save import for mods | `--mod-gci` brings in a TE save | host | CLI `--mod-gci` | yes | small | partial: a save in a profile's folder is copied to that profile's card once; the 20XX TE save is recognised instead |

---

## 11. In-game UI, overlays, accessibility, diagnostics

| Feature | What the player gets | Side | Switch that turns it on | Sim | Effort | Status |
|---|---|---|---|---|---|---|
| PC settings overlay | Eight pages: Video, Audio, Game, Controls, Overlays, Customize, Gecko Codes, Mods; opened by F1, the chord, or a row in Melee's Options menu | renderer | PS; `gx/pc_settings_shared.h`; CLI `--pc-settings`, `--pc-settings-open`, `--settings-tab`; ini `autoopenoverlay` | no | large (give the keys rows in this repo's own menu instead of porting 12,900 lines) | not wired |
| Settings appearances | Six menu styles with four palettes each, custom accent, transparency, legacy menu on F11 | renderer | ini `overlaystyle`, `overlaypalette0..6`, `menucustom`, `menucolor`, `settingstransparency`, `legacymenu`, `legacymenustyle` | no | large | not wired |
| Settings search | Search across pages with typo matching | renderer | `gx/settings_search.h` | no | medium | not wired |
| Settings file | `port-settings.ini`, one `key value` per line, saved on change | host | PS; CLI `--settings-path`, `--load-settings` | no | small | active in our own form: `settings.ini` (upstream's key names) and launcher.ini, written by the in-game menu at once |
| Pause menu on Esc | Back to game / settings / quit | renderer | `gx/pc_settings_shared.h` | no | small | active in the app's form: L + R + Start opens the in-game menu (Resume, Quit) |
| "Settings: F1" reminder | A hint in menus | renderer | ini `settingsreminder` | no | small | not wired |
| Frame counter and performance graph | | renderer | ini `showfps`, `performance` | no | small | active: `showfps 1` (presented and game frames per second, simulation time), `performance 1` (the last four seconds of simulation time and of the time between pictures, 16.7 ms line) (`frontend/test-overlays-linux.sh match`); the HUD line stays |
| Player nicknames above fighters | Names at the game's tag positions | renderer | `hud_player` [V MH]; ini `playernicknames` | no | small to medium | active online: `playernicknames 1` draws "P1  name" over each fighter from the names in the netplay host's match state (0xB3); offline there are no names (as upstream). `MU_APP_SETTINGS="playernicknames 1" test-app-linux.sh online`: "P1 Mock 2" / "P2 Mock 1" over the fighters, `net check` equal, final state identical |
| Controller input overlay | Draggable pad display per port, with values | renderer | ini `inputoverlay`, `inputoverlayvalues`, `inputoverlaystick`, `inputoverlayports`, `inputoverlayhideborder`, `inputoverlayrect0..3` | no | medium | active: `inputoverlay 1`, `inputoverlayports` (bits), `inputoverlayvalues`, `inputoverlaystick` (size 1..10), `inputoverlayhideborder` (Overlays page): one GameCube pad per port along the top of the picture; fixed places (not draggable, no `inputoverlayrect`) (`frontend/test-overlays-linux.sh match`) |
| Note when skins are off online | | renderer | ini `skin_online_notice` | no | small | not wired |
| Accessibility-type options | No screen shake, HUD sizes, PAL icons, hide KO explosions / GO! / emblems / shadows, port and shield colours, brightness / contrast / vibrance, language, mute voices | see sections 4, 8 | see there | no | small each | not wired |
| Text size or colour-blind setting | None exists upstream [V reading pass] | n/a | none | n/a | n/a | upstream stub (does not exist) |
| Logs | `melee_port.log` with rotation; crash text and dump | host | CLI `--log-file`; `host/session_logs.h` | no | none | active (our runner writes `run.log`, `host.log`) |
| Trace-mark keys | F6 to F9 mark "looked wrong / input wrong / plain / sounded wrong" in the trace | host | `host/window.cpp` | no | small (map to a chord) | not wired |
| Frame capture burst | Ctrl+F2 saves 90 frames | renderer | RO; CLI `--capture*` | no | small | not wired |
| One-frame glitch detector | Per-frame readback check | renderer | ini `flickerscan`; CLI `--flicker-scan`; `gx/flicker_scan.cpp` | no | medium | not wired |
| Frame time CSV, simulation time CSV, latency trace | | host / renderer | CLI `--frame-times`; env `MELEE_SIM_TIMES`, `MELEE_TRACE_LATENCY` | no | small | active in our own form (`--profile`, the runner's timing lines) |
| Scripted runs | Scripted pads, forced match, fixed seed, state digest | host | CLI `--script`, `--match`, `--rng-seed`, `--time-base`, `--state-digest`, `--frames`, `--headless`; `match_override`, `rng_seed_override`, `mark_match_start` [V MH] | n/a | none | active (the runner's `--script`, digests) |
| Benchmark mode | None in the game binary; upstream benchmarks through `tools/` scripts [V reading pass] | n/a | none | n/a | n/a | upstream stub (does not exist in the game) |

---

## 12. Launcher and desktop

Upstream's launcher is a separate Win32 program (`port/app/launcher*.cpp`); a second one exists for Linux
(`port/linux/launcher/`). This repo's app has its own launcher; each entry needs an equivalent there or is
marked.

| Feature | What the player gets | Side | Switch that turns it on | Sim | Effort | Status |
|---|---|---|---|---|---|---|
| Play page | Disc field, PLAY, account line, version line | launcher | `port/app/launcher.cpp` | no | small | not wired |
| Engine picker | Source Port / Static Recomp / Vanilla | launcher | `port/app/launcher_engine.h`; `launcher.ini` `engine`, `vanilla`, `vanillaowncode` | yes (Vanilla) | n/a | not applicable on the Switch (one engine per build); Vanilla is the `--vanilla-only` row in section 4 |
| CPU build auto-pick | AVX2 or compat build | launcher | `launcher.ini` `cpubuild` | no | n/a | not applicable on the Switch (one CPU) |
| Disc check | Game id, revision, `main.dol` SHA-1; m-ex and NKit discs recognised | launcher + host | `port/app/launcher.cpp` `verify_iso`, `base_disc_problem` | no | none (our installer verifies the disc) | active (in our own installer) |
| RVZ and WIA conversion | Compressed disc images converted to ISO | launcher | `port/app/disc_rvz.cpp` | no | medium (do it in our PC installer [I]) | not wired |
| CISO, GCZ | Detected only, with a "convert in Dolphin" message [V] | launcher | same | no | none | upstream stub |
| Pipeline precompile page | Runs the game hidden for 30 frames | launcher | `--hidden --frames 30 --volume 0` | no | none | active (our own cache warm-up) |
| Update check and one-click update | | launcher + host + external | `host/updater.cpp` | no | n/a | not applicable on the Switch (the app is built from the player's disc; this repo has its own update path) |
| Version rollback | Older releases kept in `Versions/` | launcher | `launcher.ini` `activeversion` | no | n/a | not applicable on the Switch (same reason) |
| Optional part downloads | Compat build, DLSS 5 model, XeSS, FSR, RTX Pack, Ray Reconstruction | launcher + host + external | `host/updater.cpp` | no | n/a | not applicable on the Switch (PC-only parts) |
| Lab view and Pro AI asset downloads | Fetched on first use with pinned hashes | host + external | `host/updater.cpp` `fetch_verified` | no | medium (place them on the SD card from our installer [I]) | not wired |
| "What's new" | Release notes on first start of a version | launcher | `launcher.ini` `notesseen` | no | small | not wired |
| Launcher languages | English plus es, pt-BR, fr, de, ru, ja, zh-Hans, ko | launcher | `port/app/launcher_lang.h`, `lang/*.txt`; `launcher.ini` `language` | no | medium | not wired |
| Launcher colour and rail picture | Accent colour, a custom side picture | launcher | `launcher.ini` `accent`; `rail-image.*` | no | small | not wired |
| Mods page | Cards with Get / Update / Remove / On-Off / Play, drop files, "Use it where it is" | launcher | `port/app/launcher_mods.inl`; `Mods/` folders | yes (enables mods) | medium | partial: the in-game menu's Mods page (`frontend/src/mods_page.cpp`, a thin view of the installer's API, ready to be replaced by a launcher in melee-unlocked's look): one mod at a time with its state, size and licence, Install / Update / Cancel / Remove (A twice), "Play it at the next start" and the `Mod profile` row (settings.ini `mod_profile`). Not here: cards, drag and drop (a file goes in `mods/Downloads/` instead), "Use it where it is" links, an On/Off per pack (one profile at a time; several by hand as `a,b`), starting the mod right away (it is loaded at the next start) |
| Mods catalog | 10 entries: 20XX TE, Training Mode CE, Akaneia, ACE, 20XX Hack Pack, Melee 3D, Melee Maker, rogueMelee, Animelee, Animelee DBZ; one-click only for entries marked `licensed` or `permission_granted` | launcher + external | `mods-catalog-2.json`, `port/app/launcher_mod_catalog.cpp`; builds with xdelta3 and 7zr | yes | medium | active (see "Mod catalog, download and install" below): the catalog built in from the fetched tree and checked with upstream's rules; the five entries with a recorded licence or permission download with Install, the three `link_only` GitHub entries install from a file the player puts in `mods/Downloads/`, the two MediaFire entries are shown with their page (upstream cannot fetch them either). Not here: the refresh of the catalog from upstream's GitHub and the GitHub release API's digest (the catalog's pinned URL and SHA-256 are used, upstream's fallback) |
| Mod catalog, download and install | The player installs a catalog mod on the console: download, checks, the patched disc built from their own disc, a mod profile | frontend (`src/modinstall/`, portable C++) | the Mods page; API `modinstall::Manager` (frontend/README.md) | no (a mod disc never goes online: such a session loads no profile) | large | active offline, not yet run on the console: downloads through the services program's libcurl with resume and a free-space check, SHA-256 of download, patch and disc (and MD5 of the vanilla disc and the result) against the catalog, zip and 7z read as streams, xdelta3 patches applied by our own VCDIFF decoder (DJW and LZMA secondary compression), result `mods/<id>/<id>.iso` = profile `<id>` (20XX TE: the save in `mods/te/`). Evidence: `tools/mod_install_test.sh` (macOS, native build of the library): 80 checks, 0 failed; Training Mode CE, Akaneia, ACE, the Hack Pack, Melee 3D, Melee Maker and rogueMelee built from the official files and each IDENTICAL (SHA-256) to the hand-patched disc and the catalog's output hash, 20XX TE's save equal to the release; licence gate (a link-only mod never fetched), free-space refusal, an interrupted download continued. `frontend/test-mods-linux.sh install`: the Mods page installed rogueMelee from mods/Downloads with a controller script (screenshots), chose it for the next start, and the next start played Rogue Mode's first fight on that disc (`[rogue] match floor=0`). With no mod installed the five reference scripts stay IDENTICAL with a picture (`test-linux.sh`, this build) and `net check` is equal on both sides in `test-net-linux.sh` and `test-online-linux.sh`. Not measured: speed on the console's SD card |
| Melee 3D | Catalog entry for the Static Recomp only | launcher | `mods-catalog-2.json` | n/a | n/a | not applicable on the Switch (needs the other engine) |
| Replay library | Search, sort, hide short games, delete, watch queue, replay folder choice | launcher | `port/app/launcher_replays.inl`; `launcher.ini` `replaydir`, `replaysort`, `replayhideshort`, `savereplays` | no | medium | partial: the in-game menu's Replays page (features/replay_library.cpp, upstream's launcher_replay_data.cpp compiled from the fetched tree): the folder's .slp/.slpz one at a time with characters, stage, length and date; sort (`replay_sort` 0..3: most recent, oldest, longest, shortest), hide games under 30 s (`replay_hide_short`), Watch it, delete (A twice). `test-replays-linux.sh library` (app mode, screenshots of the page): stats of both copies, one deleted, Watch it restarted the game with the other and it played with 0 differing records. Not here: search (no keyboard), the watch queue, a folder chooser (settings.ini `replay_dir`) |
| Replay stats | Slippi-Launcher-style overall, offence, defence, neutral, kills, conversions; network and timing graph | launcher | `port/app/launcher_replay_stats.inl`, `port/app/launcher_trace_view.cpp` | no | medium | partial: on the Replays page, per player: kills, damage done, openings per kill, neutral wins, counter hits, conversions, L-cancels, inputs per minute, rolls/spot/air dodges, wavedashes/wavelands/dash dances, ledge grabs (upstream's stat definitions, read on a worker thread, 1 to 2 ms on Linux for a 37 s game). Not here: the network and timing graph (no .trace files) |
| Watch live | Follows the Slippi Launcher's Spectate folder | launcher | `port/app/launcher_replays.inl` | no | n/a | not applicable on the Switch (no Slippi Launcher on the console) |
| Multiplayer Lobby | Opt-in roster, public and private chat, friends, presence, match requests, match history | launcher + external | `port/app/launcher_lobby*.cpp`; `lobby/README.md`; hands the game `--lobby-direct`, `--lobby-character`, `--lobby-status-file` | no | large | not wired |
| Hosted lobby service | Development only [V `lobby/README.md`] | launcher | `lobby/server.py` | no | none | upstream stub |
| Slippi account line | Shows which account will be used; link to get the Slippi Launcher | launcher | `hle/slippi_account.h` | no | none | active (this repo's sign-in screen) |
| YouGame connect | Sign in in a web view; background bridge to the game | launcher + external | `port/app/launcher_yougame*.cpp`; env `YOUGAME_BRIDGE`, `YOUGAME_TOKEN` | no | large (needs a console-side replacement, see section 3) | not wired |
| Crash report upload | Asks, then sends crash text and logs through a relay; "Send recent game logs" | launcher + external | `port/app/launcher_crash.inl` | no | medium | not wired |
| Settings window from the launcher | The settings panel without starting a match | launcher | CLI `--settings-window`, `--settings-standby`, `--settings-tab` | no | small | not wired |
| Standalone double-click start | The game finds or asks for the disc | host | `port/app/standalone_launch_win.h` | no | none | active (fixed disc path on the SD card) |

---

## 13. Advertised upstream but not found, or not as advertised, in 1.0.5

- **Slippi Ranked**: `docs/v0.7-feature-list.md` lists Ranked matchmaking; the code refuses it
  [V `hle/native_online_policy.h`].
- **"Low spec" switch** (README): no control with that label; replaced by Lightweight and Look. The `lowspec*`
  keys remain [V reading pass].
- **"Widescreen 16:9 (Slippi's own optional code)"** as a setting: only `--widescreen`; the panel offers the
  renderer's True widescreen [V reading pass].
- **Fountain of Dreams reflections "on by default"** (README): default is off [V RO].
- **Lab view "F3" in the Overlays tab**: it is Ctrl+F3 and sits under Video [V reading pass].
- **Squad Strike "off by default"** (NOTICE): the CMake default is ON [V `port/cmake/squad.cmake`].
- **20XX Hack Pack "not supported"** (docs, release notes): a full native rewrite is present [V `shim/mu_hp*.c`].
- **Akaneia "Static Recomp only / disabled"** (docs): native fighters are compiled in and flagged ready
  [V `sourceport/game/akaneia/`].
- **GameCube adapter "needs the WinUSB driver"** (README): the code also works with the stock HID driver
  [V `host/gc_adapter.cpp`].
- **Online Gecko codes**: implemented, disabled [V `host/gecko_session.cpp`].
- **TE L-cancel training wheels; Lab dummy DI / react / tech; ACE Games modes except Volleyball; aMSa and
  Jigglypuff pro rows**: stubbed or stripped.
- **P2P Direct**: test-only behind `MELEE_P2P_TEST` in the standard launcher.
- **Console link, Friends live handoff, local partner**: unvalidated per their own docs.
- **Mac**: experimental, embedded in the YouGame app only [V `port/mac/README.md`].
- Not determinable from source: which compile-time variant (`MELEE_NO_SLIPPI`, `MELEE_YOUGAME_COMING_SOON`) the
  shipped 1.0.5 archive used.

## 14. Implemented upstream but not advertised (README / feature list)

- Vulkan backend, HDR, FSR and FSR frame generation, MSAA.
- Community codes block; CPU training options; Freeze FD and Freeze Stadium; offline input delay; screen shake
  strength.
- Snapback, radius and trigger sliders; Dolphin profile import; stick modifier.
- Border art; RetroArch shader presets; built-in CRT.
- Mute voices; faster sound effects; ASIO and exclusive audio; music packs.
- Slippi spectator server; save backups; the separate `--vanilla-only` card; trace-mark keys.
- Rogue Mode, Melee Maker, m-ex stages, ACE fighters (only in the catalog and notes); the CPU bracket row;
  mu_net P2P; the replay coach.
- `--prewarm-only` (the Windows launcher does not use it).
- About 500 environment variables, of which about 190 are `MELEE_TEST_*` hooks.

---

## 15. Licences as stated upstream

Upstream code is fetched at build time and is never committed to this repo. The statements below are
upstream's own, quoted from its files; nothing here is a judgement of ours.

**Project** (`NOTICE`): "Melee Unlocked is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version." "Source files whose header names another project keep that
project's notice (for example GPL-2.0-or-later for code ported from Dolphin and Slippi Ishiiruka)". `MH` and
most `hle/` files carry `SPDX-License-Identifier: GPL-2.0-or-later`; the ranked files carry GPL-3.0-or-later.

**Squad Strike** (`NOTICE`, verbatim):

> Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0
> (idea: Chris's friend Tyler, Squad Strike Gecko codes). Optional MELEE_SQUAD_STRIKE mode, off by default.
> Includes fixes from v1.0.1; the author's confirmation for those is pending.
> These files: licence not stated; used with the author's permission (11-03-2026); not covered by
> this repository's GPL grant until the author confirms:

followed by `port/cmake/squad.cmake`, `port/runtime/abi/mu_squad_wire.h`, `port/runtime/hle/squad_test_spec.h`,
`port/runtime/hle/yougame_squad.h`, `port/runtime/host/squad_digest.h`, `sourceport/game/include/mu_squad.h`,
`sourceport/game/shim/mu_squad.c`, `sourceport/game/shim/mu_squad_css.c`,
`sourceport/game/shim/mu_squad_label_art.h`, `sourceport/game/shim/mu_squad_online.c`,
`sourceport/game/tests/squad_lineup.c`, and: "The MU_SQUAD-guarded hooks in other files carry the same
permission." Our `build.sh` defines `MU_SQUAD=1` for the host and leaves the game image's default (ON).

**Code ported or adapted, as `NOTICE` lists it**

- "Dolphin (GPL-2.0-or-later): the RVZ and WIA disc image reader, GX shader generation, AX audio emulation, the
  fres and frsqrte tables, the GameCube adapter protocol"
- "Slippi Ishiiruka (GPL-2.0-or-later): the Slippi EXI device, online play, replay playback, custom texture
  naming"
- "project-slippi/slippi-ssbm-c (GPL-3.0): Ranked selection policy and client screen groundwork"
- "Slippi's game code, slippi-ssbm-asm (GPL-3.0): the Slippi codes built into the Static Recomp and their native
  form in the Source Port"
- "Slippi's Rust extensions (published with the GPL version 2 text): the music player and game reporting"
- "Slippi Launcher (GPL-3.0): the launcher's default stock icons"
- "PhobGCC (GPL-3.0, https://github.com/PhobGCC/PhobGCC-SW)": stick calibration, notch adjustment, snapback
  filter, waveshaping, smoothing, trigger modes, tourney lockouts, the PhobVision menus and PhobScope
  recordings; "its Melee stickmaps (by venus23gg)"
- "Melee Stick Coordinate Visualizer by Altimor (GPL-3.0, https://github.com/AltimorTASDK/ssbm-stickmap)"
- "slippi-js (LGPL-3.0-or-later): the landing lag table in the launcher"
- "hps_decode (MIT)"; "Slippi Lab (MIT)"
- "Phillip / slippi-ai by Vlad Firoiu (x_pilot) (MIT, https://github.com/vladfi1/slippi-ai)"; "onnxruntime C API
  headers (MIT)"; "Phillip's trained policies (ProAI\*.onnx, MIT, from huggingface.co/vladfi/phillip-models)"
- "libmelee (LGPL-3.0, https://github.com/altf4/libmelee): Randall's path on Yoshi's Story"
- "GD's Melee (GPL-2.0-or-later): settings motion and art"; "Twemoji graphics (CC BY 4.0)"; "Source Sans 3 (SIL
  Open Font License 1.1)"
- "Training Mode Community Edition: docs/third-party-training-mode-ce.txt"
- "The Super Smash Bros. Melee decompilation (doldecomp/melee), which the Source Port's game library is built
  from"
- "Rank badge images (sourceport/game/shim/assets/rank_badges/*.png) © 2026 YouGame, licensed under
  GPL-3.0-or-later as part of this program. The YouGame name and logo are trademarks of YouGame and are not
  licensed here."

**Libraries built in, as `NOTICE` lists them**: ENet (MIT); Dear ImGui (MIT); libusb (LGPL-2.1-or-later);
stb_image (MIT or public domain); Vulkan-Headers (Apache-2.0 OR MIT); earcut.hpp (ISC); JSON for Modern C++
(MIT); dht (MIT); Monocypher (BSD-2-Clause or CC0-1.0); Zstandard 1.5.7 decoder (BSD-3-Clause); bzip2 1.0.8
(BSD-style); LZMA SDK 25.01 decoders (public domain); SlippiLib (GPL-2.0-or-later, from Slippi Ishiiruka);
NVIDIA Streamline headers (MIT); NVIDIA NGX library (NVIDIA RTX SDKs License); Intel XeSS headers (Intel
license); AMD FSR SDK 2.4 API headers (MIT); Steinberg ASIO SDK host files (GPL Version 3 option, parts under a
BSD-3-Clause style notice); GCC runtime library (GPL-3.0-or-later with the GCC Runtime Library Exception).

**Statements in other upstream files**

- Early inputs: `docs/early-inputs.md` credits "Arte's subframe rollback (JulienBernard3383279, the Kristal
  build and the 'Subframe rollback' draft for Slippi's Dolphin)". No licence is stated there.
- Training Mode CE: `docs/third-party-training-mode-ce.txt` says "Training Mode CE by UnclePunch and
  contributors" with the project's address. **No licence is named** there or in `NOTICE`.
- slpz: `port/app/replay_slpz.cpp`: "A port of the slpz tool's lib.rs (Alexander Harrison, MIT or Apache-2.0)".
- Replay stats: `port/app/launcher_replay_stats.inl`: "an independent implementation of the stat definitions the
  Slippi Launcher shows".
- Mod tools: `port/app/launcher_mods.inl`: "xdelta3 3.0.11 (GPL) ...; 7zr.exe is the official 7-Zip standalone
  (LGPL)".
- QR codes: `gx/qr_code.cpp` uses Nayuki's qrcodegen, "port/third_party/qrcodegen, MIT" [V reading pass].
- Mods catalog (`mods-catalog-2.json`) licence fields: 20XX TE "MPL-2.0"; Training Mode CE, Akaneia, ACE and
  the 20XX Hack Pack "Shared by its authors"; Melee 3D, Melee Maker, rogueMelee, Animelee and Animelee DBZ "Not
  stated".

**Directories and files with no licence text** [V recursive search by a reading pass]

- `sourceport/game/tmce/` (attribution only, see above)
- `sourceport/game/akaneia/`
- `sourceport/game/ace/`
- `sourceport/game/mexstages/`
- `sourceport/game/rogue/`
- `sourceport/game/maker/`
- `sourceport/game/shim/mu_20xx_ai.c`, `mu_20xx_cpu.c`, `mu_hp*.c` (rewritten from the 20XX Hack Pack's code, per
  their own headers)
- The individual community codes (source comments credit Achilles1515 and "rmn, PKFreeZZy")

**Missing from `NOTICE`** [V search by a reading pass]: librashader (`port/third_party/librashader/LICENSE.md`
is "Mozilla Public License Version 2.0") and `port/third_party/slang-shaders`; 20XX TE (the catalog says
MPL-2.0).

**Other upstream statements worth knowing**: the README says the project "is not affiliated with, endorsed by,
or supported by the Slippi team, Nintendo or HAL Laboratory". `port/sys_deltas/README.md` says three Slippi menu
files are rebuilt from the player's disc because a "compliance ruling prohibits shipping these Nintendo-derived
whole files from YouGame in production".

---

## Appendix A. Settings file keys (`port-settings.ini`)

One `key value` per line; parsed in `PS`. Source for this table: a reading pass over `PS` and `RO`.

| Key(s) | What it controls | Feature |
|---|---|---|
| `fps` | Frame cap (-1 monitor, 0 uncapped, 30..2000) | Frame-rate unlock |
| `subframe` | Sub-frame mode (0 off, 2 interpolate, other predict) | Frame-rate unlock |
| `vsync`, `vsyncchoice`, `vrrfound`, `vsync_yougame` | V-Sync and its auto choice | Display |
| `fullscreen`, `exclusivefullscreen`, `window` | Window mode and size | Display (not applicable) |
| `aspect` | Aspect mode 0..4 | Display |
| `truewidescreen`, `truewsaspect`, `truewscustomw`, `truewscustomh` | True widescreen | Gameplay options |
| `scale`, `msaa`, `ssaa`, `anisotropy`, `sharpness`, `ssao` | Internal resolution, anti-aliasing, filtering, sharpening, ambient occlusion | Display |
| `brightness`, `contrast`, `vibrance` | Picture adjustments | Display |
| `custompreset` | The Custom quality preset's six numbers | Display |
| `backend` | Renderer choice | Display (not applicable) |
| `dlss`, `framegen`, `fgmax`, `fgdynamic`, `reflex`, `reflexstats`, `reflexflash` | Upscaler, frame generation, Reflex | Display (not applicable) |
| `hdroutput`, `hdrpaperwhite`, `hdrpeak` | HDR | Display (not applicable) |
| `pathtracing`, `rtxintegrator`, `rtxpreset`, `rtxlook`, `rtxbrightness`, `rtxreflstrength`, `rtxfloorstrength`, `rtxshadowstrength`, `rtxbouncestrength`, `rtxshadows`, `rtxao`, `rtxbounce`, `rtxreflections`, `rtxglossyfloors`, `rtxskin`, `rtxphotomode`, `rtxfightershadows`, `rtxstageshade`, `rtxmoddedstages`, `rtxmodfloors`, `rtxeffectlight`, `rtxglowlight`, `rtxradiancecache`, `rayreconstruction` | Ray tracing | Display (not applicable) |
| `dlss5*` (`dlss5preset`, `dlss5tonerestore`, `dlss5downsample`, `dlss5upsample`, `dlss5reconstruction` and the profile keys) | DLSS 5 | Display (not applicable) |
| `lightweight`, `effects`, `lowpoly`, `lowspec`, `lowspec_look`, `lowspec_prev_*`, `retro` | Lightweight level, effects tier, old low-spec keys | Looks |
| `look_fighter`, `look_stage`, `look_menus`, `look_unfiltered`, `look_colours`, `look_background`, `look_background_rgb`, `look_own`, `look_full_models`, `look_cap_lifted`, `look_saved_fps`, `look_saved_subframe` | Look presets and their parts | Looks |
| `labview`, `look_lab_fighters`, `look_lab_stage`, `lab_colour_p1..4`, `lab_colour_background`, `lab_colour_stage`, `labskipscene` | Lab view | Lab view |
| `arcade_hitboxes`, `arcade_projectile_hitboxes`, `arcade_hitboxes_behind`, `arcade_combo_counter`, `arcade_camera_2d`, `arcade_backdrop`, `arcade_background_rgb`, `arcade_grid_rgb` | Arcade look | Arcade look |
| `borderart`, `borderedge` | Side-bar art | Border art |
| `crt_filter`, `crt_curve` | Built-in CRT | CRT look |
| `shaderpreset` | RetroArch preset | Shader presets |
| `customtextures`, `prefetchtextures`, `texpackoff <folder>`, `dumptextures` | Texture packs and dumping | Texture packs |
| `videobackgrounds` | Looping menu videos | Video backgrounds |
| `latencytest`, `lagtestnext` | Display latency test, adapter lag test | Diagnostics |
| `flickerscan`, `showvram`, `advancedvideo`, `advancedoverlays` | Glitch detector, VRAM readout, Advanced sections open | Diagnostics / UI |
| `music`, `musicpacks`, `volume` | Music level, custom music, master volume | Audio / Jukebox |
| `audio_mode`, `audio_asio_driver`, `audio_asio_buffer`, `audio_device` | Output mode and device | Audio (not applicable) |
| `audio_buffer_ms` | Software audio queue | Audio |
| `audio_early_keyon`, `mute_voices` | Faster sound effects, mute voices | Gameplay options |
| `menusounds` | Settings UI sounds | UI |
| `autoopenoverlay`, `backgroundinput` | Open settings at start; read pads unfocused | UI / Controllers |
| `gamelanguage` | Game language | Gameplay options |
| `unlock_all`, `results_screen`, `zelda_as_sheik` | Unlock everything, results screen, Zelda as Sheik | Gameplay options |
| `shield_colorp1..4`, `shield_color_online` | Shield colours | Gameplay options / Online |
| `offline_delay`, `offline_delay_frames` | Offline input delay | Gameplay options |
| `onlinedelay`, `onlinedelayauto` | Online input delay | Online |
| `early_inputs2` (`early_inputs` is the old, ignored key) | Early inputs | Online |
| `quickchat`, `directpause`, `blockcode1..3` | Quick chat, pause rule, skip list | Online |
| `portcolour1`, `portcolour2` | Online port colours | Online |
| `online_training_after`, `online_return_training`, `keep_search`, `search_banner` | Training after online, play while searching | Online |
| `warmup_panel_style`, `warmup_button`, `warmup_turn_instant`, `warmup_watch`, `warmup_pip_size`, `warmup_pip_corner`, `warmup_pip_cx`, `warmup_pip_cy`, `warmup_pip_cw`, `room_watch`, `spectate_view` | Warm-up room and arena views | Online |
| `slippi_spectator`, `slippi_spectator_port` | Spectator server | Online |
| `netoverlay`, `showping` | Network overlay, ping | Online |
| `discordon`, `discord_app_id` | Discord presence | Online (not applicable) |
| `freeze_fd`, `stadium_frozen`, `fodreflections`, `cssbackgroundfree` | Stage options | Gameplay options |
| `cpu_20xx`, `cpu_tech`, `cpu_getup`, `cpu_di`, `cpu_sdi`, `cpu_notaunt`, `cpu_lcancel`, `cpu_nojab`, `cpu_notransform` | 20XX CPUs and CPU training options | Gameplay options |
| `lcancel_flash_mode`, `lcancel_success_flash`, `lcancelindicator` (old), `autolcancel` | L-cancel flash and Auto L-cancel | Gameplay options |
| `stockhudscale`, `damagehudscale`, `palstockicons`, `playertags`, `noscreenshake`, `screenshake`, `hide_ko_explosions`, `hide_hud_emblems`, `training_hud` | HUD and camera options | Gameplay options |
| `community_codes` | Community codes mask (hex) | Gameplay options |
| `geckocode <name>`, `geckochosen` | Enabled pasted codes | Gecko codes |
| `te_options`, `te_options2`, `te_menu_music` | 20XX TE words and menu song | 20XX TE |
| `mod_profile`, `mod_te_enabled`, `mod_tmce_enabled`, `mod_enabled <key>`, `mods_in_direct`, `hp_settings` | Mod profile, packs on/off, Hack Pack settings | Mods |
| `practicecpu`, `practicecpuslot1..3`, `practicestyle`, `practiceopens`, `matchmakinghint`, `proaistronger` | Practice CPU, menu look, hint, stronger pro AI | Training |
| `rumble` | Controller rumble | Controllers |
| `port0..3`, `portname0..3`, `activeprofile<tab>` | Port routing and active profile | Controllers |
| `key_<Action>`, `pad<i>_<Action>`, `ds4<i>_<Action>`, `gc<i>_<Action>`, `swpro<i>_<Action>`, `hid<i>_<Action>`, `*_level`, `pad<i>_invert` | Bindings and analog levels | Controllers |
| `stick_modifier_percent`, `deadzone_<family>_main|_c`, `radius_<family>_main|_c`, `snapback_size_<family>_x|y`, `snapback_ms_<family>_x|y`, `snapback_<family>_x|y` (old), `trigger_<family>_l|r`, `trigger_<family>_l_threshold|r_threshold` | Stick and trigger tuning per controller family | Controllers |
| `swpro_gc_picture` | GameCube picture for Switch pads in the editor | Controllers |
| `clean_replays` | Hide overlays during replays | Replays |
| `playernicknames`, `settingsreminder`, `skin_online_notice`, `performance`, `showfps`, `showfps_yougame2` | Overlays | UI |
| `inputoverlay`, `inputoverlayvalues`, `inputoverlaystick`, `inputoverlayports`, `inputoverlayhideborder`, `inputoverlayrect0..3` | Controller input overlay | UI |
| `legacymenu`, `legacymenustyle`, `overlaystyle`, `overlaypalette0..6`, `settingstransparency`, `menucustom`, `menucolor` | Settings appearance | UI |
| `onlineframeprotection`, `editdevice*`, `online_back_to_training`, `truewsfit` | Old keys, read and ignored | none |

Not traced to a key: the arena "Room look", "Arena button" and "Hide the room panels" controls.

Other files beside the settings file: `settings-view.txt` (last page), `GeckoCodes.ini`, `ControllerProfiles/`,
`PhobGCC/port<N>.ini` and `PhobGCC/Setups/`, `Dlss5Profiles/`, `Mods/` (`Discs`, `Saves`, `Files`, `Profiles`,
`links.txt`), `CosmeticMods/`, `MusicPacks/`, `TexturePacks/`, `Load/Textures/GALE01`, `Shaders/`, `Borders/`,
`VideoBackgrounds/`, `Lab/`, `ProAI/`, `sound-mode.txt` in the card folder, and the launcher's `launcher.ini`
(`iso`, `activeversion`, `warmcache`, `notesseen`, `dlss5asked`, `replayasplayed`, `savereplays`,
`compressreplays`, `replaydir`, `replaysort`, `replayhideshort`, `cpubuild`, `engine`, `vanilla`,
`vanillaowncode`, `language`, `lobbyopen`, `lobbyiso`, `lobbyisoname`, `accent`, `yougame_env`,
`yougame_extension`, `yougame_app`, `yougame_session`).

## Appendix B. Command-line flags of the game executable

Parsed in `MC` (shared by both upstream engines). T = test harness or diagnostic only (classified from the
code comments by a reading pass).

| Flag(s) | What it controls | Feature |
|---|---|---|
| `--iso`, `-e` | Disc image | Disc |
| `--sys-dir`, `--sysgen-data-dir` | Slippi system files and where rebuilt ones go | Slippi system files |
| `--settings-path`, `--load-settings` (T) | Settings file | Settings file |
| `--card-dir` | Memory card folder | Saves |
| `--replay-dir`, `--compress-replays`, `--record-native` | Replay output | Replay recording |
| `--lab-dir`, `--lab-view` | Lab view assets and switch | Lab view |
| `--log-file`, `--quiet` (T), `--version` | Logging | Diagnostics |
| `--shader-cache`, `--prewarm-only` | Pipeline cache | Precompile |
| `--threaded-renderer` | Render thread | Frame-rate unlock |
| `--fps`, `--frame-mode` | Frame cap and sub-frame mode | Frame-rate unlock |
| `--scale`, `--msaa`, `--ssaa`, `--anisotropy`, `--sharpness`, `--ssao` | Image quality | Display |
| `--window`, `--fullscreen`, `--aspect`, `--vsync`, `--backend` | Window, aspect, V-Sync, renderer | Display |
| `--widescreen`, `--true-widescreen[=...]` | Slippi widescreen code; renderer widescreen | Gameplay options |
| `--dlss`, `--dlss-mode`, `--dlss5`, `--dlss-jitter-sign` (T), `--frame-generation[=...]`, `--reflex` | Upscalers, frame generation, Reflex | Display (not applicable) |
| `--crt`, `--border`, `--shader`, `--retro`, `--look`, `--lightweight`, `--effects` | Looks and filters | Looks |
| `--custom-textures`, `--dump-textures` | Texture packs | Texture packs |
| `--pc-settings`, `--pc-settings-open`, `--settings-window`, `--settings-standby`, `--settings-tab` | Settings panel | UI |
| `--language`, `--no-screen-shake`, `--pal-stock-icons` | Game options | Gameplay options |
| `--auto-lcancel`, `--lcancel-indicator`, `--lcancel-log` (T) | L-cancel tools | Gameplay options |
| `--gecko-codes` (T) | Code file | Gecko codes |
| `--volume`, `--music`, `--music-volume`, `--no-music`, `--audio-dump` (T), `--list-audio-devices` | Audio | Audio |
| `--vanilla-only`, `--vanilla-game` (T) | Retail-only session; code-free game | Gameplay options |
| `--mod-profile`, `--mod-dir`, `--mod-iso`, `--mod-gci`, `--mod-opponent-unverified`, `--no-mod-autodetect`, `--scan-mods` (T), `--mod-base-iso`, `--look-iso` | Mods | Mods |
| `--te-options`, `--te-options2`, `--cpu-training` (T) | TE words, CPU training word | 20XX TE / CPU options |
| `--import-cosmetic`, `--select-cosmetic`, `--deselect-cosmetic`, `--enable-project-effects`, `--restore-vanilla-cosmetics`, `--cosmetic-status`, `--scan-disc-skins` (T) | Cosmetic imports (run and exit) | Cosmetics |
| `--replay`, `-i`, `-b`, `--trace`, `--as-experienced` | Replay playback | Replays |
| `--spectate-view`, `--spectate-owner` | Arena viewer | Arena |
| `--slippi-menus on|off` | Online menus | Slippi netplay |
| `--user-dir`, `--netplay-port`, `--chat`, `--online-delay`, `--online-mode`, `--allow-matchmaking` (T) | Account folder, port, chat, delay, mode | Online |
| `--lobby-direct`, `--lobby-character`, `--lobby-status-file` | Launcher lobby hand-off | Lobby |
| `--ranked-rules` | YouGame ranked rules package | YouGame Ranked |
| `--p2p-port`, `--p2p-peer`, `--p2p-slot`, `--p2p-chars`, `--p2p-stage`, `--p2p-seed`, `--p2p-delay`, `--p2p-identity`, `--p2p-expect`, `--p2p-result`, `--p2p-names`, `--p2p-connect-seconds`, `--p2p-games` | mu_net session at boot | mu_net P2P |
| `--local-peer` (T), `--online-test` (T), `--test-stage` (T), `--arena-test` (T), `--squad-test-invalid-lineup` (T), `--squad-test-net-delay` (T) | Online test harness | Online |
| `--match`, `--boot-match` (T), `--script` (T), `--rng-seed` (T), `--time-base` (T), `--frames` (T), `--fast` (T), `--headless` (T), `--hidden` (T), `--resume` | Scripted runs | Scripted runs |
| `--state-digest` (T), `--state-trace` (T), `--trace-calls` (T), `--trace-func` (T), `--input-log` (T), `--frame-times` (T), `--capture`, `--capture-frame`, `--capture-every`, `--capture-burst`, `--capture-sim-frame`, `--dump`, `--dump-frame` (all T), `--flicker-scan`, `--pin-phase` (T), `--hang-watch` (T), `--profile` (T), `--profile-render` (T), `--card-self-test` (T) | Diagnostics | Diagnostics |
| `--lag-test` | Adapter lag test | Training tools |
| `--ram-translator`, `--no-ram-translator` | Static Recomp only | not applicable (other engine) |

Upstream's Linux minimal host (`port/linux/main.cpp`), the one our runner is built from, has its own short list:
`--iso`, `--game-library`, `--fps 60..1000`, `--no-gc-adapter`, `--vanilla-game`, `--log-file`, `--card-dir`,
`--input-script`, `--state-digest`, `--frames`, `--headless`, `--fast`, `--trace-state`, `--rng-seed`,
`--time-base`, `--capture`, and four self-tests.
