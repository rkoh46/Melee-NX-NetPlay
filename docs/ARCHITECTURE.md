# Architecture

Melee-NX-NetPlay is two proven stacks joined at the renderer and the platform layer.

```text
player's main.dol + Slippi code list ──recomp.py──▶ 144 C++ units (guest)      [Dashdance]
guest ──▶ ppc::Context, HLE of the GameCube SDK, Slippi EXI device, GX decoder    [Dashdance]
GX decoder ──▶ gx::Frame ──gx_aurora_bridge──▶ Aurora GX FIFO                      [Dashdance]
Aurora GX ──▶ Dawn WebGPU ──▶ Vulkan ──▶ Mesa NVK ──▶ libnx NWindow                [Melee-NX]
SDL3 (Dusklight backend) for pads, audio and the window; libnx for services     [Melee-NX]
```

## The game: recompiled, not rebuilt from source

`port/recomp/recomp.py` reads the player's `main.dol` and the decomp's symbol table, applies
Slippi's Gecko code list (`port/slippi_sys`, loaded at `0x8065CC80` like Slippi Dolphin) and
emits one C++ function per guest function. Loads and stores go through byte-swapping helpers,
floating point through `ppc::fmadd`-style helpers compiled with `-ffp-model=strict
-ffp-contract=off`, and the FPCR is set with `mrs/msr` on AArch64 (`runtime/ppc/numeric.*`). This is
what keeps a Switch peer frame-identical to a Dolphin peer during rollback; Dashdance already
matches Slippi Dolphin on Apple's AArch64 cores, and the Cortex-A57 follows the same IEEE rules.

114 guest functions are replaced by host code (`port/recomp/hle_list.txt`): disc reads, pads,
audio (AX ucode), memory cards, OS threads and the Slippi EXI device. HLE hooks are matched by
symbol name; a name missing from the list is silently recompiled instead of hooked.

Guest RAM is a plain 24 MiB `calloc` plus 16 MiB of ARAM; addresses are masked into it (`ppc.h`,
`fast()`), with no mmap or fixed virtual address, which suits newlib. The dispatch table for
indirect calls is 48 MiB of function pointers. Online, the savestate pool for 7 rollback frames is
about 57 MiB. With the ~70 MB of recompiled code this needs the full application memory, hence
title override at launch.

## Switch platform layer (`switch/`)

From Melee-NX: the toolchain file (`SwitchGCC.cmake`: devkitA64 GCC for C, Clang 19 for C++, PIC
multilib, explicit startup objects and link group because Clang ignores `switch.specs`), the
Docker recipes, and the Dawn/SDL/Aurora patches that make the graphics stack run on Horizon.
`switch/src/switch_threads.c` synchronises Clang's `tpidr_el0` TLS with libnx on every thread,
gives pthreads 8 MiB stacks, reaps detached threads (libnx has no `pthread_detach`) and turns
`exit/abort/_exit` into a flush-then-`svcExitProcess`.

The simulation runs on its own libnx thread with a 32 MiB stack (`port/app/main_switch.cpp`): the
guest call depth is capped at 20000 and the loader's main stack is not sized for it.

## Phase 1: headless

`MELEE_NX_FRONTEND=headless` links Dashdance's headless window/input/audio stubs: the game boots,
runs a neutral or scripted controller, decodes GX into frames that are dropped, and logs
`sim: X ms/frame` every 60 frames. This is the measurement that decides the overclock story before
any pixel is drawn. Paths live under `sdmc:/switch/melee-nx-netplay/`; Slippi's Sys folder is in
the NRO's RomFS (`romfs:/Sys`).

## Phase 2: renderer and input

Dashdance's Aurora backend (`gx_aurora.cpp`, `gx_aurora_bridge.cpp`) re-encodes each decoded draw as
GX wire bytes into Aurora's FIFO. On the Mac it is a diagnostic; here it becomes the renderer, with
Melee-NX's Aurora patches (Vulkan-only backend selection, NWindow surface, uber-shaders, cache
recovery, GX CPU-side state suppression). Work items:

- a Switch frontend replacing `metal_frontend_aurora.cpp`: `aurora_initialize` with
  `BACKEND_VULKAN`, no Cocoa checks;
- `reset_epoch()` on every rollback load (`gx::mark_discontinuity` is declared but unused) so
  reversed frame sequences stop throwing `CoverageError`;
- overlay text (menu, HUD, sign-in) without CoreText: a glyph atlas rasterised with
  `stb_truetype` or Aurora's ImGui pass;
- SDL3 input through the Dusklight backend; the official GameCube adapter appears as a standard
  pad, with analog triggers from Melee-NX's SDL patch; Dashdance's controller mapping
  (`input_config.*`) carries over. The system owns the adapter (usb:hs refuses it, so the 1000 Hz
  reader in `gc_adapter_usbhs.cpp` only runs when it is free) and updates each GameCube pad 200
  times a second. The game thread calls `SDL_UpdateJoysticks()` itself before every `PADRead`
  (otherwise the render thread's event pump refreshed the pads, up to a frame late), and a
  GameCube pad's sticks bypass the generic gamepad conversion: `sdl-dusklight-gc-sticks.patch`
  scales the system's calibrated +-32767 to a real controller's ~+-100 at the gate (`stick_range`,
  80..120) without a dead zone, which Melee applies itself.

## Phase 3: online

The Slippi host half is portable: ENet 1.3.13 over BSD sockets (IPv4), matchmaking at
`mm.slippi.gg:43113`, HTTPS to Slippi's GraphQL and Firebase endpoints for login, reports and the
rank card. On the Switch, sockets come from libnx (`socketInitializeDefault`) and HTTPS from
devkitPro's curl over mbedTLS (`runtime/hle/slippi_http_curl.cpp`, the one HTTP implementation for
every caller). Shift-JIS player names need an iconv replacement (`slippi_text.cpp`). `user.json`
(connect code, play key) lives in `sdmc:/switch/melee-nx-netplay/User/Slippi`; the sign-in
screen drives `slippi_login.*` exactly as Dashdance's dashboard does. The online input delay is a
player setting (`online_delay`: `auto`, 2 frames for the first game of a set, then 2..5 from the round trip of the game's own pad
acknowledgements in the previous game (ENet's roundTripTime starts at 500 ms and is useless right after
connecting), or a
fixed 1..9) and is never hardcoded.

## What is deliberately not here

Melee-NX's melee-pc game build, its disc extractor and data gate (Dashdance's HLE reads the ISO
directly), Dashdance's Metal renderer, Cocoa/UIKit dashboards, Windows D3D12 path and Discord
presence. Melee-NX's own architecture notes are kept as
[MELEE_NX_ARCHITECTURE.md](MELEE_NX_ARCHITECTURE.md) for the toolchain and linker constraints,
which apply unchanged.
