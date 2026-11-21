# Building Melee-NX-NetPlay

Everything cross-compiles inside Docker; the host needs Git, Docker, Bash and Python 3.11+. The
generated game is private to your build folder (`build/`, ignored) and is never committed.

## The short way: install.sh

```bash
curl -fsSL https://raw.githubusercontent.com/rkoh46/Melee-NX-NetPlay/main/install.sh | bash -s -- /path/to/GALE01.iso
```

Steps 1-3 below, plus the prebuilt SDK from the `sdk-*` release instead of section 4's hours of Mesa,
Dawn and SDL builds, ending with the SD-card folder in `dist/sd/switch/melee-nx-netplay/`. The SDK is
made with `MESA_NVK_ROOT=... bash builder/package-sdk.sh <version>` after section 4 and contains no
game content. A checkout with `build/sdk/` unpacked uses it automatically (`MELEE_NX_SDK_DIR`
overrides the location). The rest of this page is the long way.

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

## 4. Renderer build (phase 2)

The Vulkan path needs Mesa's NVK built for the Switch. Nothing prebuilt exists; build it once from
the pinned fork (Docker, about an hour, pinned commit in `builder/fetch-deps.sh`):

```bash
bash builder/fetch-deps.sh mesa
cd ref/mesa-switch && bash build-switch.sh && cd ../..
export MESA_NVK_ROOT="$PWD/ref/mesa-switch"
```

The "LLVM gold plugin has failed to create LTO module" lines at the end are `ar` noting the LLVM
bitcode Rust embeds in its objects; the archives are complete. With Colima, keep `ref/mesa-switch`
on a shared volume (the fetcher clones it inside the checkout, which is fine when the checkout is).

Then the graphics dependencies and the game:

```bash
bash builder/fetch-deps.sh                  # melee-pc (for Aurora), Dawn, SDL
bash builder/docker.sh graphics all         # patches, Dawn, SDL (1-3 hours)
MELEE_NX_FRONTEND=aurora bash builder/docker.sh game all
```

Output: `build/switch-aurora/port/melee-nx-netplay.nro` (about 100 MB).

`builder/docker.sh sdk-image` packages the toolchain with the Mesa archives so later machines need
no Mesa checkout, as in Melee-NX.

The renderer build includes the Slippi online services (`MELEE_NX_ONLINE`, on by default). Until the
console has a sign-in screen, copy your Slippi identity from the Slippi Launcher
(`.../Slippi Launcher/netplay/User/Slippi/user.json`: connect code and play key) to
`sdmc:/switch/melee-nx-netplay/User/Slippi/user.json`. `launcher.ini` keys: `online_delay=` (`auto`, the
default: 2 frames for the first game against an opponent, then 2..5 from the ping measured in the previous game; or a fixed 1..9), `offline=1` to skip the sign-in and keep the services off, `volume=` (0..100), `hud=1`, `stick_range=` (GameCube stick value at the gate, 80..120, default 100 like a real controller), `trigger_range=` (light shield scale, 2..8),
`overclock=` (1: ask for Erista's top clocks, CPU 1785 / GPU 921 / memory 1600 MHz, tunable with
`clock_cpu_mhz=` `clock_gpu_mhz=` `clock_mem_mhz=`; 0: the system's clocks). The in-game menu is
L+R+Start held half a second. Without a saved identity the renderer shows the Slippi sign-in screen first.

## Following Slippi releases

`tools/slippi_pin.json` is the one place that names the Slippi release the port implements: the version it
reports (matchmaking and the report user agent), the matchmaking endpoint, the Slippi Dolphin tag and commit,
and the `slippi-rust-extensions` commit that tag uses. CMake reads it into the build
(`port/runtime/hle/slippi_version.h`); nothing else in the tree carries a version string.

```bash
python3 tools/update-slippi.py --check       # what the latest Slippi release would need; changes nothing
bash tools/update-slippi.sh /path/to/GALE01.iso   # adopt it if possible, then rebuild both apps
```

The tool diffs the upstream files listed under `host_protocol` in the pin between the pinned release and
the new one. If none changed, it copies the release's `Data/Sys` files (the inventory under `sys_files`)
into `port/slippi_sys`, rewrites `tools/slippi_sys_manifest.json` and the Slippi hashes in
`tools/port_source_pins.json`, and moves the pin; regeneration then validates every Gecko hook against the
HLE list as usual. If any changed, it stops with exit status 3 and lists them with their counterparts here.
Never edit `version` by hand to get past matchmaking: a build that reports a release whose protocol it
does not speak desyncs or fails against Dolphin players.

Porting a protocol release: bring the files below up to the new upstream, then run
`python3 tools/update-slippi.py --tag vX.Y.Z --ported` to adopt the Sys files and move the pin.

| Upstream (Slippi Dolphin, `Source/Core/Core/`) | Here (`port/runtime/hle/`) |
|---|---|
| `HW/EXI_DeviceSlippi.{h,cpp}`: commands, payload sizes, reply layouts, online frame flow | `exi_slippi.cpp` (table, recording, game files), `slippi_online.cpp` (online commands, savestates), `slippi_offline.cpp` (offline replies of the same sizes) |
| `Slippi/SlippiNetplay.{h,cpp}`, `Slippi/SlippiPad.h`, `NetPlayProto.h`: peer wire format | `slippi_net.{h,cpp}` (`NetplayClient`, `Pad`, `NetMsg`) |
| `Slippi/SlippiMatchmaking.{h,cpp}`: STUN on the netplay socket, the match result | `slippi_net.cpp` (`Matchmaking`), STUN servers in `slippi_version.h` |
| `Slippi/SlippiGameFileLoader.cpp` | `exi_slippi.cpp` (`build_game_file`, `resolve_links`) |
| `Slippi/SlippiSavestate.cpp` | `slippi_online.cpp` (`Savestate`) |
| `Slippi/SlippiGameReporter.cpp` | `slippi_report.cpp` |

| Upstream (`slippi-rust-extensions`) | Here |
|---|---|
| `matchmaking/` (queue service client, STUN codec) | `slippi_matchmaking_ws.{h,cpp}`; host test `tools/test_matchmaking_ws.cpp` |
| `hsd/` `archive.rs`, `link.rs`, `hash.rs`, `ffi/src/hsd.rs` (links from game files to disc assets) | `slippi_hsd_link.{h,cpp}`; host test `tools/test_hsd_link.cpp` |
| `match-quality/` (ranked termination) | `slippi_match_quality.{h,cpp}` |
| `user/` (rank thresholds), `game-reporter/` | `slippi_report.cpp`, `slippi_net.cpp` |
| `jukebox/` | `jukebox.cpp` |
| `input-suggestions/` | not ported: message 0x87 is accepted and ignored, the prediction block of the online-inputs reply repeats the latest real input |
| `ffi/includes/SlippiRustExtensions.h` | the whole host surface: a new function there is a new host duty |

The Replays app's code set (`port/slippi_sys_playback`) is not moved by the tool.

## An always-on build computer

The console can update itself from a computer of yours that stays on (`update_host=` in `launcher.ini`,
README "Updating from your own computer automatically"). The built NROs never leave your home network.

**What the computer needs** (Linux x86_64 or arm64, or a Mac):

- Git, Bash, curl, Python 3.11 or newer, `shasum` (part of Perl on Debian/Ubuntu; `perl-Digest-SHA` on Fedora),
  and Docker that your user may run without sudo (`sudo usermod -aG docker $USER`, then log in again; or
  rootless Docker). The toolchain image is built from `devkitpro/devkita64`, which is published for amd64 and arm64.
- About 10 GB of free disk: the toolchain image (about 4 GB), the build trees of both apps (about 2 GB with
  the SDK and the pinned decomp), Docker's build cache.
- Your disc image, NTSC-U 1.02.
- No Mesa, Dawn or SDL build: like `install.sh`, the update build uses the prebuilt game-free SDK from this
  repository's `sdk-*` release (downloaded once, 55 MB).
- A first build takes 20 to 40 minutes; an hourly run with nothing new takes seconds.

**Set it up:**

```bash
curl -fsSL https://raw.githubusercontent.com/rkoh46/Melee-NX-NetPlay/main/install.sh | bash -s -- /path/to/GALE01.iso
cd ~/Melee-NX-NetPlay
python3 tools/update-host.py /path/to/GALE01.iso            # one run: pull, follow Slippi, rebuild if needed, publish
python3 tools/update-serve.py --bind 192.168.1.20 --port 8080   # this computer's home-network address
```

`builder/systemd/` has user units for both (an hourly timer and the server); the comments at the top of
`melee-nx-update.service` say how to install them. With cron instead:
`17 * * * * cd ~/Melee-NX-NetPlay && python3 tools/update-host.py /path/to/GALE01.iso >> ~/melee-nx-update.log 2>&1`.

**One run of `update-host.py`:** `git pull --ff-only`; `tools/update-slippi.py` (a game-side-only Slippi release
is adopted locally, a protocol release is recorded as pending); if the commit or any local change differs
from what the published build was made from, `install.sh` rebuilds both apps from this checkout; then it
publishes. It prints one summary line and exits 0 (published or nothing to do), 3 (a Slippi release is
waiting for a port; the last good build stays published) or 1 (failure; the last good build stays
published). The build log is `build/update-host.log`.

**The published folder** (default `~/.local/share/melee-nx-netplay/updates/`, `--publish` to change): the two
NROs under names that carry the first 12 digits of their SHA-256, and `latest.json`. NROs are written to a
temporary name and renamed, the manifest last, so a manifest never names a half-written file; NROs the new
manifest no longer names are removed afterwards.

```json
{
  "schema": 1,
  "app_version": "0.3.8",
  "slippi_version": "3.7.0",
  "built": "2026-11-21T05:10:00Z",
  "commit": "<git commit of the checkout>",
  "files": {
    "game":    {"name": "melee-nx-netplay-0123456789ab.nro", "size": 74497982, "sha256": "<64 hex digits>"},
    "replays": {"name": "melee-nx-replays-ba9876543210.nro", "size": 72183875, "sha256": "<64 hex digits>"}
  },
  "pending": null,
  "inputs": "<fingerprint of the commit and local changes the build was made from>"
}
```

`pending` is `{"slippi": "3.8.0", "reason": "needs a port"}` while a Slippi release waits for a port.

**`update-serve.py`** answers GET and HEAD for `/latest.json` and the NRO names that manifest lists; every
other path is 404. It refuses to bind a public address. There is no login and no TLS: home network only,
no port forwarding. The NROs contain the game.

**On the console** (`port/runtime/host/self_update.*`, `port/app/main_switch.cpp`): the manifest is fetched on
a background thread at launch (1.5 s to connect; the launch waits 2 s at most). An update is offered when
the published app version is newer, or equal with a newer Slippi version. A download goes to
`<app>.nro.new`; after its size and SHA-256 match, a marker `<app>.nro.new.ok` is written. The swap is two
renames (installed to `.bak`, `.new` into place), attempted after the app has closed its RomFS, and again
before the RomFS is opened at every start of either app, so a swap that could not happen (the file still
open) or was cut off between the two renames is finished at the next start. A staged file without its
marker is deleted, never installed. Host tests: `tools/test_self_update.cpp`, `tools/test_update_host.py`.

## Running on the Switch from this computer

The Switch and this computer on the same Wi-Fi is the whole requirement. On the Switch (Atmosphère) open
the Homebrew Menu by holding R while launching a game, press Y so the netloader waits, then:

```bash
tools/nx-run.sh <switch-ip> aurora        # or headless
```

sends the NRO and streams its output here (every log line: clocks, frame timing, Slippi connection, the
reason for a crash), also saved under `build/nxlink-logs/`. The first run builds `tools/bin/nxlink`
(devkitPro's netloader client, one C file) natively. Settings, the disc and `Logs/` still live in
`sdmc:/switch/melee-nx-netplay/`; with sys-ftpd-light (or the ftpd homebrew) on the Switch, that folder
is reachable without taking the card out:

```bash
tools/nx-files.sh <switch-ip> iso /path/to/GALE01.iso   # once; ~1.4 GB over Wi-Fi
tools/nx-files.sh <switch-ip> push aurora               # NRO + launcher.ini onto the card
tools/nx-files.sh <switch-ip> pull                      # Logs/, crash and fatal reports -> build/switch-pull/
```

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
| The console shows "The software was closed because an error occurred" | Read `Logs/latest.log` (flushed every line) for the last thing the port did, then the newest file in `sdmc:/atmosphere/crash_reports/`: its PC and the module offsets resolve against `build/switch-<frontend>/port/melee-nx-netplay.elf` with `aarch64-none-elf-addr2line -e <elf> -f -C <offset>` inside the toolchain container (`bash builder/docker.sh shell`) |
