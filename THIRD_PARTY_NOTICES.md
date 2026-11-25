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
| [slippi-rust-extensions](https://github.com/project-slippi/slippi-rust-extensions) | C++ ports of the jukebox (`jukebox.cpp`), the matchmaking service client and STUN codec (`slippi_matchmaking_ws.*`, from `matchmaking/`), the game-file link resolver (`slippi_hsd_link.*`, from `hsd/` and `ffi/src/hsd.rs`), the match-quality tracker (`slippi_match_quality.*`, from `match-quality/`) and the rank thresholds, all at commit 934f373 | GPL-2.0, no version stated | Ships the GPLv2 text as `LICENSE`; section 9 of GPLv2 lets any published version be chosen. |

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
| [glslang](https://github.com/KhronosGroup/glslang) 15.1.0 (Khronos) | Fetched by `experiments/native-engine/fetch.sh` (release archive, SHA-256 pinned) and built by `frontend/build.sh glslang` into the Linux test build and the NRO: it compiles RetroArch shader presets' slang GLSL to SPIR-V on the device (`frontend/src/user_shader.cpp`); never committed | BSD-3-Clause, BSD-2-Clause, MIT and Apache-2.0 for the parts built (its `LICENSE.txt`, which lists them by part) |
| melee-unlocked's shader preset helpers | `experiments/native-engine/frontend/src/user_shader.cpp` ports the preset helpers of its `port/runtime/gx/user_shader.cpp` (the Shaders folder's listing, a single `.slang` as a preset of one pass, `first_missing_file` and `missing_include` with their messages) off Windows; the preset reader and the pass runtime are this project's (upstream runs presets through librashader, which is not used) | GPL-2.0-or-later, as part of melee-unlocked (its README, "License"), used here under GPL-3.0-or-later |
| [Tracy](https://github.com/wolfpld/tracy) | Optional profiler Aurora fetches; off by default | BSD-3-Clause |
| [zstd](https://github.com/facebook/zstd) | System or devkitPro portlib linked by the native-engine experiment for `.slpz` replays | BSD-3-Clause (or GPL-2.0) |
| [slpz](https://github.com/AlexanderHarrison/slpz) (Alexander Harrison) | The `.slpz` format, through melee-unlocked's C++ port (`port/app/replay_slpz.cpp`, GPL-2.0-or-later) compiled from the fetched tree by `experiments/native-engine/frontend/src/features/replays.cpp`; nothing copied | MIT or Apache-2.0 (the tool); GPL-2.0-or-later (the port) |
| melee-unlocked's Slippi spectator server | `experiments/native-engine/frontend/src/features/spectator.cpp` is adapted from its `port/runtime/hle/slippi_spectate.cpp`, a port of Slippi Dolphin's `SlippiSpectateServer` | GPL-2.0-or-later (both sources; header `SPDX-License-Identifier: GPL-2.0-or-later`), used here under GPL-3.0-or-later |
| melee-unlocked's peer lobby (Multiplayer Lobby) | The Switch app's Multiplayer Lobby (`experiments/native-engine/frontend/src/lobby.cpp`, `mu_home_lobby.inl`) compiles its `port/app/launcher_lobby_p2p.cpp` and `.h` and the English launcher text `launcher_lang_en.inl` from the fetched tree (build-time copies in `gen/lobby`, `frontend/patches/0142`); `src/lobby_shim/launcher_lang.h` is a stand-in for its `launcher_lang.h` written to the same calls; nothing of it is committed | GPL-2.0-or-later (headers `SPDX-License-Identifier: GPL-2.0-or-later`), used here under GPL-3.0-or-later |
| [jech/dht](https://github.com/jech/dht) (Juliusz Chroboczek) | The BitTorrent mainline DHT (BEP 5) under the Multiplayer Lobby: melee-unlocked's vendored copy, `port/third_party/dht/dht.c` and `dht.h`, compiled from the fetched tree into the Switch app's services program; not committed | MIT: its `LICENCE` and the header of `dht.c`: "Copyright (c) 2009-2011 by Juliusz Chroboczek / Permission is hereby granted, free of charge, to any person obtaining a copy of this software [...]" |
| [Monocypher](https://monocypher.org) (Loup Vaillant, Michael Savage, Fabio Scotoni) | The Multiplayer Lobby's signatures, key exchange and encryption (Ed25519, X25519, BLAKE2b, XChaCha20-Poly1305): melee-unlocked's vendored copy, `port/third_party/monocypher/monocypher.c`, `monocypher-ed25519.c` and their headers, compiled from the fetched tree; not committed | BSD-2-Clause or CC0-1.0, at the user's choice: `monocypher.c`'s header "SPDX-License-Identifier: BSD-2-Clause OR CC0-1.0" and its `LICENCE.md`: "Monocypher as a whole is dual-licensed." |
| melee-unlocked's settings panel and launcher | The Switch app's in-game panel and home screen (`experiments/native-engine/frontend/src/mu_panel.cpp`, `mu_home.cpp`) compile its own code, copied verbatim from the fetched tree at build time by `frontend/scripts/mu_ui_extract.py` (`port/runtime/gx/pc_settings_visuals.inl`, `settings_page_navigation.h`, `settings_imgui_input.h`, `settings_search.h`, functions and blocks of `pc_settings.cpp` and `pc_settings_shared.h`, the palette and layout constants of `port/app/launcher.cpp`, `launcher_theme.h`); nothing of it is committed | GPL-2.0-or-later (each file's header `SPDX-License-Identifier: GPL-2.0-or-later`), used here under GPL-3.0-or-later |
| melee-unlocked's launcher artwork and icon | `port/app/bailey_cut.bmp`, `melee_mark.bmp` (the home screen's rail) and `port/app/melee_unlocked.ico` (the NRO's icon, converted to JPEG by `frontend/build.sh`), embedded at build time from the fetched tree; not committed | GPL-2.0-or-later, as part of melee-unlocked (its README, "License": "GPL-2.0-or-later."; its NOTICE names no other terms for them) |
| [GD's Melee](https://github.com/Hero88go/melee-unlocked/blob/main/docs/third-party-ui-attribution.md) settings motion | `ui_sources/gd_melee/motion.h`, `text.inl`, `font_metrics.h` (the panel's slide-in keyframes; the GD font tables compile but are not drawn), through melee-unlocked's copies | GPL-2.0-or-later (melee-unlocked's NOTICE: "GD's Melee (GPL-2.0-or-later): settings motion and art") |
| [Source Sans 3](https://github.com/adobe-fonts/source-sans) Black and Bold (Adobe) | The panel's heading font and the home screen's text, from melee-unlocked's `port/runtime/gx/ui_sources/gd_melee/assets/kit/`, embedded at build time; not committed | SIL Open Font License 1.1: "Copyright 2010-2024 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'. [...] This Font Software is licensed under the SIL Open Font License, Version 1.1." (`SourceSans3-OFL.md` beside the fonts) |
| melee-unlocked's Practice menu and Fight a pro | `experiments/native-engine/frontend`: its `port/runtime/gx/practice_menu.cpp` and `port/runtime/hle/phillip_ai.cpp` compiled from the fetched tree with `frontend/patches/0112`, `0113` (build-time copies; nothing copied into this repository); `frontend/src/features/proai.cpp` follows its `practice_cpu.cpp`, `native_practice.cpp` and `source_host.cpp` (command 0xEA) | GPL-2.0-or-later (headers `SPDX-License-Identifier: GPL-2.0-or-later`), used here under GPL-3.0-or-later |
| [slippi-ai](https://github.com/vladfi1/slippi-ai) (Phillip, Vlad Firoiu) | The policy input and output packing (`onnx_policies.py`, format version 1) that melee-unlocked's `phillip_ai.cpp` follows; `experiments/native-engine/tools/proai_catalog.py` reads the models' own metadata the same way | MIT | Its `LICENSE`: "MIT License / Copyright (c) 2020 Vlad Firoiu". |
| [vladfi/phillip-models](https://huggingface.co/vladfi/phillip-models) (Hugging Face) | Phillip's published ONNX policies: never committed or shipped; the app downloads the ones the player asks for from the pinned URLs in `experiments/native-engine/frontend/src/proai/catalog.json` (SHA-256 checked), or the player copies them to `ProAI/` | MIT | The model card's metadata: "license: mit" (Hugging Face tag `license:mit`). |
| [libmelee](https://github.com/altf4/libmelee) | Randall's positions on Yoshi's Story, as the table melee-unlocked generated (`port/runtime/hle/phillip_randall.h`, from libmelee's `melee.stages.randall_position`), compiled from the fetched tree | LGPL-3.0 (GPL-3.0 compatible) | The generated header: "from libmelee's melee.stages.randall_position (LGPL-3.0, https://github.com/altf4/libmelee)". |
| [xdelta](https://github.com/jmacd/xdelta) 3.1.0 (Joshua P. MacDonald) | The DJW secondary decoder in `experiments/native-engine/frontend/src/modinstall/mi_vcdiff.cpp` is adapted from its `xdelta3/xdelta3-djw.h` and `xdelta3-second.h` (bit order, code-length coding, prefix tables, group selectors); the rest of the VCDIFF decoder is written from RFC 3284; no xdelta3 program is shipped or run | GPL-2.0-or-later: the files' headers at tag v3.1.0 read "either version 2 of the License, or (at your option) any later version"; used here under GPL-3.0-or-later |
| melee-unlocked's mod catalog and its rules | `experiments/native-engine/frontend/src/modinstall/mi_catalog.cpp` follows its `port/app/launcher_mod_catalog.cpp` (`parse`: field checks, the rule that automatic download needs a recorded licence or permission); its `mods-catalog-2.json` is read from the fetched tree at build time and built into the program, not committed | GPL-2.0-or-later (header `SPDX-License-Identifier: GPL-2.0-or-later`), used here under GPL-3.0-or-later |
| [LZMA SDK](https://www.7-zip.org/sdk.html) 25.01 (Igor Pavlov) | `C/LzmaDec.c`, `C/Lzma2Dec.c` and their headers, compiled by the mod installer (7z downloads, xdelta3's LZMA secondary compression) from the copy in the fetched melee-unlocked tree (`port/third_party/lzma`); not committed | Public domain: the files' headers read "Igor Pavlov : Public domain"; the tree's `lzma-sdk.txt` |
| zlib | Its raw inflate, for the mod installer's zip downloads: the system's library on Linux and macOS, devkitPro's portlib on the console (already linked, row above) | zlib |

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

### jech/dht

```
Copyright (c) 2009, 2010 by Juliusz Chroboczek

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
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### Monocypher (its BSD-2-Clause licence; it is also offered under CC0-1.0)

```
Copyright (c) 2017-2023, Loup Vaillant
Copyright (c) 2017-2019, Michael Savage
Copyright (c) 2017-2023, Fabio Scotoni
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the
   distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
