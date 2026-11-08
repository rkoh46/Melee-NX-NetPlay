# Building Melee-NX-NetPlay

Everything cross-compiles inside Docker; the host needs Git, Docker, Bash and Python 3.11+. The
generated game is private to your build folder (`build/`, ignored) and is never committed.

## 1. Sources and the generated game

```bash
git clone https://github.com/rkoh46/Melee-NX-NetPlay && cd Melee-NX-NetPlay
bash builder/fetch-deps.sh melee            # pinned doldecomp/melee (symbol names only)
bash builder/generate-game.sh /path/to/GALE01.iso
```

`generate-game.sh` extracts `main.dol` from your disc (the SHA-1 must be the vanilla NTSC 1.02
image), applies Slippi's pinned code list and writes 144 C++ translation units of recompiled game to
`build/game/generated/guest` together with a provenance manifest. Re-running after an edit under
`port/` is required: the manifest records the runtime sources, and the build refuses a stale one.

## 2. Toolchain image

```bash
bash builder/docker.sh image
```

devkitA64 GCC compiles C, Clang 19 compiles C++ (the recompiled game needs `-ffp-model=strict`
and exceptions), and the portlibs the Slippi services use (curl over mbedTLS, zlib) are installed.

## 3. Headless build (phase 1)

```bash
bash builder/docker.sh game all             # MELEE_NX_FRONTEND=headless is the default
```

Output: `build/switch-headless/port/melee-nx-netplay.nro`. Copy it to
`sdmc:/switch/melee-nx-netplay/` with your disc as `GALE01.iso` beside it, then launch with full
application memory (hold R while opening a game). The console shows the frame counter; the log is
`Logs/latest.log` in the same folder, with `[frame N] ... sim: X ms/frame (worst Y)` every 60 frames.
Optional keys in `launcher.ini` next to the NRO: `frames=` (stop after N), `fast=1` (no pacing),
`script=` (an input script from `port/scripts/`, for a scripted match), `iso=`.

## 4. Renderer build (phase 2, when it lands)

The Vulkan path needs Mesa's NVK built for the Switch. Nothing prebuilt exists; build it once from
the pinned fork (Docker, about an hour, pinned commit in `builder/fetch-deps.sh`):

```bash
bash builder/fetch-deps.sh mesa
cd ref/mesa-switch && bash build-switch.sh && cd ../..
export MESA_NVK_ROOT="$PWD/ref/mesa-switch"
```

Then the graphics dependencies and the game:

```bash
bash builder/fetch-deps.sh                  # melee-pc (for Aurora), Dawn, SDL
bash builder/docker.sh graphics all         # patches, Dawn, SDL (1-3 hours)
MELEE_NX_FRONTEND=aurora bash builder/docker.sh game all
```

`builder/docker.sh sdk-image` packages the toolchain with the Mesa archives so later machines need
no Mesa checkout, as in Melee-NX.

## Settings

| Variable | Default | Purpose |
|---|---|---|
| `MELEE_NX_FRONTEND` | `headless` | `headless` or `aurora` |
| `MELEE_DOCKER_IMAGE` | `melee-nx-netplay-build:latest` | Toolchain or SDK image |
| `MELEE_BUILD_JOBS` | `4` | Parallel compile jobs (the guest is 144 large units; more is better) |
| `MESA_NVK_ROOT` | unset | Built Mesa tree for the aurora frontend |
| `MELEE_SDL_VARIANT` | `dusklight` | SDL backend pair, as in Melee-NX |

## Troubleshooting

| Symptom | Check |
|---|---|
| `bash: builder/build-game.sh: No such file or directory` from the container | Docker cannot see the checkout: with Colima, only the volumes in `~/.colima/default/colima.yaml` `mounts:` are shared. Add your home folder there (and restart Colima) or keep the checkout on a shared volume. |
| `Generated game missing` | Run `builder/generate-game.sh` on the host first |
| `--dol must be the verified vanilla Melee NTSC 1.02 image` | Wrong disc or region; the SHA-1 is in `tools/port_source_pins.json` |
| `Slippi Sys file inventory differs` | `port/slippi_sys` was changed; restore it (`tools/slippi_sys_manifest.json` lists it) |
| Launched in applet mode warning on the console | Hold R while opening a game so the homebrew gets application memory |
| `Mesa/NVK is missing` | Only the aurora frontend needs it; set `MESA_NVK_ROOT` or use an SDK image |
