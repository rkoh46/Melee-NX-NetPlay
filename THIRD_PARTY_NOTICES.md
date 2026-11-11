# Licences and third-party notices

Melee-NX-NetPlay is distributed under the GNU General Public License, version 3 or (at your option) any later version
(GPL-3.0-or-later); the text is in [LICENSE](LICENSE). It joins two GPL projects: the game runtime and Slippi netplay
of Dashdance (GPL-2.0-or-later, whose files keep their own SPDX headers) and the Switch platform layer of Melee-NX
(GPL-3.0-or-later). GPL-2.0-or-later code may be distributed under GPL-3.0, so the combination is GPL-3.0-or-later.
This page lists what the project is built from, the licence each part carries, and the evidence, so the licensing can
be checked rather than taken on trust. It is a factual record, not legal advice.

## The two parents

| Source | What this project uses | Licence | Evidence |
|---|---|---|---|
| [Dashdance](https://github.com/TheAndersMadsen/dashdance) (via [rkoh46/dashdance](https://github.com/rkoh46/dashdance), branch `dashdance2`) | `port/`: the recompiler (`port/recomp`), the host runtime and GameCube SDK HLE (`port/runtime`), the Slippi EXI device, netplay, matchmaking, login, reports and savestates (`port/runtime/hle/slippi_*`, `exi_slippi.*`), the GX decoder and Aurora backend (`port/runtime/gx`), `tools/bootstrap_port.py`, `tools/extract_dol.py`, the Slippi Sys files | GPL-2.0-or-later | Its `THIRD_PARTY_NOTICES.md`: "Dashdance is distributed under the GNU General Public License, version 2 or (at your option) any later version"; source headers `SPDX-License-Identifier: GPL-2.0-or-later`. |
| [Melee-NX](https://github.com/KawaiiBunga/Melee-NX) (KawaiiBunga) | `switch/`: the devkitA64/Clang toolchain file and Docker recipes, the Dawn, SDL and Aurora Switch patches, the Clang TLS and thread glue (`switch/src/switch_threads.c`, adapted from its `clang_tls_switch.c`), the Mesa/NVK link group, the SQLite VFS and log/perf helpers; `builder/`: the Docker wrapper, dependency fetcher and graphics build | GPL-3.0-or-later | Its README: "Port code in `switch/` and `builder/` is GPL-3.0-or-later, matching melee-pc." |

Melee-NX's game sources (melee-pc, a build of the decomp) are **not** used: this project compiles Dashdance's
recompilation of the player's own `main.dol`. `ref/melee-pc` is fetched only because Melee-NX's Aurora patches target
the Aurora tree vendored there.

## Code Dashdance ports (carried over unchanged)

| Source | What is used | Licence | Evidence |
|---|---|---|---|
| [Hero88go/melee-unlocked](https://github.com/Hero88go/melee-unlocked) | Dashdance's base: the recompiler, runtime and HLE layers (pinned in `tools/port_source_pins.json`) | GPL-2.0-or-later | Its README, section "License": "GPL-2.0-or-later." |
| [Dolphin](https://github.com/dolphin-emu/dolphin) | Ports: GX register layouts and shader generation (`gx_regs.h`, `gx_shader.cpp`), AX audio (`ax_ucode.cpp`), memory-card layout | GPL-2.0-or-later | Ported files carry `SPDX-License-Identifier: GPL-2.0-or-later`. |
| [Slippi Dolphin](https://github.com/project-slippi/dolphin) | The playback Sys files in `port/slippi_sys_playback` (the Melee-NX Replays app) | GPL-2.0-or-later, per file | Same COPYING and SPDX scheme as Dolphin. |
| [Slippi Ishiiruka](https://github.com/project-slippi/Ishiiruka) | Ports: the Slippi EXI device, netplay, matchmaking, user record, savestates and playback; SlippiLib (`port/third_party/slippilib`); the Sys files in `port/slippi_sys` (from `Data/Sys`) | GPL-2.0-or-later | Its Readme: "licensed under the terms of the GNU General Public License, version 2 or later (GPLv2+)". |
| [slippi-rust-extensions](https://github.com/project-slippi/slippi-rust-extensions) | A C++ port of the jukebox (`jukebox.cpp`) | GPL-2.0, no version stated | Ships the GPLv2 text as `LICENSE`; section 9 of GPLv2 lets any published version be chosen. |

## Components under other licences

| Component | Where | Licence |
|---|---|---|
| [Aurora](https://github.com/encounter/aurora) | Fetched into `ref/melee-pc/extern/aurora` (999sian's fork, patched by `switch/patches/aurora-switch-*.patch`); the GX renderer, VI, PAD and OS layers | MIT |
| [Dawn](https://github.com/encounter/dawn) (Google's WebGPU implementation, encounter's fork) | Fetched into `ref/dawn`, patched by `switch/patches/dawn-*.patch` | BSD-3-Clause |
| [Mesa](https://gitlab.freedesktop.org/mesa/mesa) / [danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch) | The NVK Vulkan driver for the Tegra X1, built separately from the fork pinned in `builder/fetch-deps.sh` and linked as static archives | MIT (Mesa's `licenses/`) |
| [SDL 3](https://github.com/libsdl-org/SDL) | Fetched into `ref/SDL-dusklight`, patched with the Dusklight-NX Switch backend (`switch/patches/sdl-dusklight-*.patch`, from [souldbminerr/dusklight-nx](https://github.com/souldbminerr/dusklight-nx), GPL-3.0-or-later as part of that port) | zlib (SDL); GPL-3.0-or-later (the backend patch) |
| [libnx](https://github.com/switchbrew/libnx) and devkitA64 | The Switch SDK and toolchain, used from the `devkitpro/devkita64` image | ISC (libnx); GPL with runtime exception (GCC/newlib components) |
| [curl](https://curl.se), [mbedTLS](https://github.com/Mbed-TLS/mbedtls), zlib | devkitPro portlibs linked for the Slippi HTTPS services | curl licence (MIT-style); Apache-2.0 or GPL-2.0-or-later (mbedTLS); zlib |
| [ENet](https://github.com/lsalzman/enet) 1.3.13 | `port/third_party/enet` | MIT, see its `LICENSE` |
| [nlohmann/json](https://github.com/nlohmann/json) 3.4.0 | `port/third_party/nlohmann/json.hpp` | MIT, notice in the header |
| [hps_decode](https://github.com/DarylPinto/hps_decode) 0.3.0 | The HPS/DSP-ADPCM decoding that `jukebox.cpp` ports | MIT |
| [VirtualFriend](https://github.com/agg23/virtualfriend) | The touch layout in `window_sdl.cpp` (not built for the Switch, kept for the shared source) | MIT |
| [ControllerOverlays](https://github.com/datkat21/ControllerOverlays) | The GameCube controller artwork in `port/app/art/controller` | GPL-3.0 |
| [Tracy](https://github.com/wolfpld/tracy) | Optional profiler Aurora fetches; off by default | BSD-3-Clause |

## Not in this repository

- **doldecomp/melee.** The build reads a pinned checkout of [doldecomp/melee](https://github.com/doldecomp/melee) for
  symbol names and generates an animation interpreter from its `fobj.c` and `spline.c`. That repository publishes no
  licence. The generated code is created in your build folder and compiled into your own build; it is not committed.
- **Super Smash Bros. Melee.** Nintendo and HAL Laboratory own the game. This repository contains none of its code or
  data; every build is made from a disc image the player supplies, and no built NRO is published. The files in
  `port/slippi_sys` are Slippi's own code list, code handler and patches, as distributed in Slippi Ishiiruka.
- **Trademarks.** Slippi is a trademark of its authors. Nintendo Switch is a trademark of Nintendo. The icon in
  `switch/icon.jpg` is derived from Dashdance's mark as a placeholder and will be replaced.

## AI-assisted development

This project is written with AI coding assistants and its commits carry an AI co-author trailer; Dashdance's notices
discuss what that means for copyright (purely AI-generated material may not be protected; code derived from the GPL
sources above stays under the GPL either way). The same applies here.

## Notice texts

The MIT and zlib licences require their notices to be kept. ENet and nlohmann/json keep theirs in
`port/third_party`. The others, which are not otherwise present in this repository:

### VirtualFriend

```
The MIT License (MIT)

Copyright (c) 2024 Adam Gastineau

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### hps_decode

```
MIT License

Copyright (c) 2023 Daryl Pinto

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### Aurora

```
The MIT License

Copyright (c) 2022 Luke Street

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### SDL

```
Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>
  
This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:
  
1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required. 
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```
