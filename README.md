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

## When Slippi releases an update

This build plays **Slippi 3.7.0**. Slippi's matchmaking only pairs players on its latest release, so
after each Slippi update online play stops until this app is on the same release. Offline play is
never affected.

- **The app tells you.** At launch it asks GitHub (one request, nothing about you is sent) whether Slippi
  has a newer release than the one it plays. If so, a screen says which before the game starts, instead
  of the error on the character select screen.
- **To update, run the installer again** on your computer with your disc, and copy the two new folders to
  the SD card. It fetches the newest Melee-NX-NetPlay and rebuilds both apps:

  ```bash
  curl -fsSL https://raw.githubusercontent.com/rkoh46/Melee-NX-NetPlay/main/install.sh | bash -s -- /path/to/GALE01.iso
  ```

- **If Melee-NX-NetPlay itself has not caught up yet**, add `--update-slippi` to the end of that line (or run
  `bash tools/update-slippi.sh /path/to/GALE01.iso` in the folder). It compares Slippi's new release with
  the one this app implements. When only Slippi's game-side code and files changed, it fetches them and
  builds an app for the new release on the spot. When Slippi changed how its programs talk to each other
  (matchmaking, the netplay messages, the game's link to the emulator), it changes nothing and says so:
  that kind of release needs new code here first, and online play has to wait for it. Slippi 3.7.0 was
  one of those. Not every Slippi release can be picked up automatically, and the app is never told to
  claim a release it does not implement, because that would desync against other players.

The app cannot update itself on the console: it is built from your disc, so there is nothing to download.

## Updating from your own computer automatically

If a computer at home is always on (a Linux box, a home server, a Mac), it can do the rebuilding for you
and the console can fetch the result itself. Nothing is published anywhere: the built app goes only from
that computer to your console, over your home network.

On the computer, once (details in [BUILDING.md](BUILDING.md#an-always-on-build-computer)):

1. Run the installer there as above, so it has the source, the tools and your disc.
2. Let it check every hour: `tools/update-host.py /path/to/GALE01.iso` fetches the newest
   Melee-NX-NetPlay, looks for a new Slippi release, rebuilds when something changed and puts the result
   in a folder.
3. Let it offer that folder to the console: `tools/update-serve.py --bind <its home-network address> --port 8080`.

On the console, once: open `sdmc:/switch/melee-nx-netplay/launcher.ini` and add the computer's address:

```ini
update_host=http://192.168.1.20:8080
```

From then on, when you start the app and the computer has something newer, a screen says
**Update available** with the version: **A** installs it, **B** leaves it for later. Installing downloads
the new app (and the Replays app, if it is on the card) beside the old one, checks every byte against the
checksum the computer published, and only then swaps it in and asks you to start the app again. If the
download is interrupted or does not check out, nothing is changed and you keep playing the version you
have. The previous version is kept beside the app as `melee-nx-netplay.nro.bak`.

What it cannot do: when a Slippi release changes how Slippi's programs talk to each other, the computer
cannot build an app for it until Melee-NX-NetPlay itself has been updated. The console then says so
("it needs a Melee-NX-NetPlay update first") and the update arrives by itself once that work is published.

Keep it at home. The files the computer serves contain the game built from your disc, without a password
and without encryption: use the computer's home-network address, never a public one, and do not forward
the port on your router. The console trusts whatever answers at the address you typed, so the checksum
protects against a broken download, not against someone else on your network pretending to be your
computer. The setting is off unless you add it, and `offline=1` skips the check.

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
| 3 | Slippi online: ENet over libnx sockets, curl for login and reports, sign-in screen, online delay setting | plays Slippi 3.7.0: netplay, matchmaking over Slippi's WebSocket service with STUN, login, reports, ranked match-quality checks, game files linked from the disc, a sign-in screen, automatic input delay. Input suggestions (3.7's "16.7 ms less ping" messages) are received and ignored, not sent |
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
