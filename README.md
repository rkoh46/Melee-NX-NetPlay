# Melee-NX-NetPlay

Super Smash Bros. Melee with Slippi online play, as a native Nintendo Switch homebrew: the game's own
PowerPC code recompiled to C++ (Dashdance's runtime), running on Melee-NX's Switch platform layer.
The target is 60 fps on an overclocked Erista Switch, GameCube controllers through the official
adapter, and matchmaking against the same Slippi servers and Dolphin players as on a PC.

**Status: alpha, not playable yet.** The build pipeline and the headless simulation exist; the
renderer, input and online phases are listed below with what each needs. Nothing from the game is in
this repository and no built NRO is published: you build it from your own disc.

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

## What you need

- A Switch running [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere). Launch the NRO with
  full application memory (hold **R** while opening a game); the recompiled game is large.
- Your own NTSC-U 1.02 disc image (GALE01; the build verifies its `main.dol` SHA-1).
- A build host with Git, Docker, Bash and Python 3.11+. The first graphics build takes hours and
  tens of gigabytes; see [BUILDING.md](BUILDING.md).
- For online play: a Slippi account (connect code and play key), signed in on the console.

## Roadmap

| Phase | What | State |
|---|---|---|
| 1 | Headless simulation on the console: a scripted match, log to SD, measure ms/frame | NRO builds (71 MB); first hardware run pending |
| 2 | Aurora GX → Dawn → Vulkan/NVK renderer, SDL3 input and audio, GC adapter, overlay text | frontend, Aurora, Dawn and SDL compile for the Switch; the link waits on a Mesa/NVK build; overlay text not started |
| 3 | Slippi online: ENet over libnx sockets, curl for login and reports, sign-in screen, online delay setting | not started |
| 4 | Overclock profile, rollback cost tuning, release notes | not started |

## Layout

| Path | Contents |
|---|---|
| `port/recomp/` | The recompiler (PowerPC → C++) and the HLE hook list |
| `port/runtime/` | Guest CPU context and memory, GameCube SDK HLE, Slippi, GX decoding and the Aurora backend |
| `port/app/main_switch.cpp` | The libnx entry point |
| `port/slippi_sys/` | Slippi's Sys folder (code list, code handler, GameFiles), packed into the NRO's RomFS |
| `switch/` | Toolchain file, Docker recipes, the Dawn/SDL/Aurora Switch patches, Clang/libnx thread glue |
| `builder/` | `generate-game.sh` (host: disc → generated sources), `docker.sh`, `build-game.sh`, `build-graphics.sh`, `fetch-deps.sh` |
| `tools/` | `bootstrap_port.py`, `extract_dol.py`, pins and manifests |
| `docs/` | Architecture notes |

## Licence

GPL-3.0-or-later; see [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for
every component and its evidence. Dashdance's files keep their GPL-2.0-or-later headers. This is an
unofficial project with no relation to Nintendo, HAL Laboratory or the Slippi team.
