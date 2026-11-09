# Melee-NX-NetPlay

Super Smash Bros. Melee with Slippi online play, as a native Nintendo Switch homebrew: the game's own
PowerPC code recompiled to C++ (Dashdance's runtime), running on Melee-NX's Switch platform layer.
The target is 60 fps on an overclocked Erista Switch, GameCube controllers through the official
adapter, and matchmaking against the same Slippi servers and Dolphin players as on a PC.

**Status: alpha.** On an overclocked Erista Switch the game boots, renders and runs offline VS matches
at full speed (two level-9 CPUs, Fox against Marth on Battlefield: 14-15 ms of simulation per 16.7 ms
frame). Online play is built but untested on hardware. Nothing from the game is in this repository and
no built NRO is published: you build it from your own disc.

## Why this shape

Slippi is two halves. The host half (EXI device, ENet netplay, matchmaking, login, savestates,
replay writing) is ordinary C++ and lives in `port/runtime/hle/slippi_*`. The game half is Slippi's
Gecko code list: patches to the original PowerPC binary that add the online menus, the rollback
loop and the input injection. [Dashdance](https://github.com/TheAndersMadsen/dashdance) keeps both
halves by recompiling the real `main.dol` with that code list applied, so its netplay stays
frame-identical to a Slippi Dolphin peer. [Melee-NX](https://github.com/KawaiiBunga/Melee-NX) runs
the decomp's C source instead, which has no binary to patch. This project therefore takes the
game and netcode from Dashdance and the Switch toolchain, Dawn/Vulkan/NVK renderer path, SDL
backend and startup glue from Melee-NX. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Install

One command on a Mac or Linux PC with Docker builds the homebrew from your own disc (about 20 minutes,
most of it compiling the game) and tells you where to put the result:

```bash
curl -fsSL https://raw.githubusercontent.com/rkoh46/Melee-NX-NetPlay/main/install.sh | bash -s -- /path/to/GALE01.iso
```

It downloads a prebuilt, game-free SDK (the Switch GPU driver, Dawn, SDL3 and Aurora, 55 MB, from this
repository's releases), builds the compiler image, translates your disc and compiles it. The finished
folder goes to `sdmc:/switch/melee-nx-netplay/` on the SD card with your disc beside it as `GALE01.iso`.
No built NRO is published: it would contain the game.

## What you need

- A Switch running [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere). Launch the NRO with
  full application memory (hold **R** while opening a game); the recompiled game is large.
- Your own NTSC-U 1.02 disc image (GALE01; the build verifies its `main.dol` SHA-1).
- A build host with Git, Docker, Bash and Python 3.11+ for the installer. Building the SDK itself
  from source (Mesa, Dawn, SDL) is hours and tens of gigabytes; see [BUILDING.md](BUILDING.md).
- For online play: a Slippi account (connect code and play key), signed in on the console.

## Roadmap

| Phase | What | State |
|---|---|---|
| 1 | Headless simulation on the console: a scripted match, log to SD, measure ms/frame | done; scripted CPU matches and a sampling profiler drive the tuning |
| 2 | Aurora GX → Dawn → Vulkan/NVK renderer, SDL3 input and audio, GC adapter, overlay text | runs on hardware: menus, character and stage select, matches with shadows and EFB-copy effects; GX commands parsed on a worker thread; 60 fps in the CPU match test |
| 3 | Slippi online: ENet over libnx sockets, curl for login and reports, sign-in screen, online delay setting | built: netplay, matchmaking, login, reports, Shift-JIS names, a sign-in screen with the console keyboard, online delay in the menu; untested on hardware |
| 4 | Overclock profile, rollback cost tuning, release notes | clock request through clkrst (CPU 1785 / GPU 921 / memory 1600 MHz, `overclock=` and per-rail keys); rollback cost not yet measured |

## Layout

| Path | Contents |
|---|---|
| `port/recomp/` | The recompiler (PowerPC → C++) and the HLE hook list |
| `port/runtime/` | Guest CPU context and memory, GameCube SDK HLE, Slippi, GX decoding and the Aurora backend |
| `port/app/main_switch.cpp` | The libnx entry point |
| `port/slippi_sys/` | Slippi's Sys folder (code list, code handler, GameFiles), packed into the NRO's RomFS |
| `switch/` | Toolchain file, Docker recipes, the Dawn/SDL/Aurora Switch patches, Clang/libnx thread glue |
| `install.sh` | The one-command installer (clone, SDK download, compiler image, translate, compile, SD folder) |
| `builder/` | `generate-game.sh` (host: disc → generated sources), `docker.sh`, `build-game.sh`, `build-graphics.sh`, `fetch-deps.sh`, `package-sdk.sh` |
| `tools/` | `bootstrap_port.py`, `extract_dol.py`, pins and manifests |
| `docs/` | Architecture notes |

## Licence

GPL-3.0-or-later; see [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for
every component and its evidence. Dashdance's files keep their GPL-2.0-or-later headers. This is an
unofficial project with no relation to Nintendo, HAL Laboratory or the Slippi team.
