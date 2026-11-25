# mu-native-gfx: the native-engine runner with a picture. Configured by build.sh in three variants:
#   null    aarch64 Linux, no graphics device: the GX decoder and the renderer's frame check only
#   linux   aarch64 Linux with Aurora on Dawn/Vulkan (the test build; Mesa's software driver under Xvfb)
#   switch  Horizon NRO with Aurora on Dawn/Vulkan (Mesa NVK), linked like the app (port/CMakeLists.txt)
# melee-unlocked's sources are compiled from build/native-engine-gfx/mu and never copied into the repository.
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)
project(mu_native_gfx LANGUAGES C CXX ASM)

set(MU_FE_VARIANT "null" CACHE STRING "null, linux or switch")
option(MU_FE_FEATURES "Link the feature modules (src/features/, runner/features/; feature_hooks.h)" ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

get_filename_component(REPO "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(GFX "${REPO}/build/native-engine-gfx")
set(MU "${GFX}/mu")              # our copy of the fetched melee-unlocked tree, with patches/0101
set(RUN "${GFX}/runner")         # snapshot of experiments/native-engine/runner taken with the tree
set(GEN "${GFX}/gen")            # embedded.S, embedded_tables.cpp, the patched gx_core.cpp
set(FE "${CMAKE_CURRENT_LIST_DIR}/src")
set(GXSRC "${REPO}/port/runtime/gx")
set(APPSRC "${REPO}/port/runtime/host")
# The app's version (Melee-NX-NetPlay 0.4.0: the NACP, the self-update's comparison, the menu's title bar) and the
# Slippi release the netplay object implements (../build.sh NET_DEF: SLIPPI_PIN_VERSION 3.7.0).
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION" MU_APP_VERSION LIMIT_COUNT 1)
set(MU_SLIPPI_VERSION "3.7.0")
foreach(_needed "${MU}/port/linux/host_services.cpp" "${RUN}/runner_main.cpp" "${GEN}/embedded.S" "${GEN}/gx_core.cpp")
  if(NOT EXISTS "${_needed}")
    message(FATAL_ERROR "missing ${_needed}: run frontend/build.sh sync first")
  endif()
endforeach()

if(MU_FE_VARIANT STREQUAL "switch")
  # Dawn, SDL3, Aurora and the Mesa/NVK archives, exactly as the app gets them. libnx also before libc, so the
  # netplay object's ENet and curl get libnx's socket-aware fcntl (../README.md, the netplay hand-over). curl and
  # mbedTLS are for the services program; nothing else refers to them.
  # Aurora from build.sh sync's copy with patches/01xx-aurora-*.patch (Graphics.cmake keeps a value already cached).
  set(MELEE_AURORA_SOURCE "${GFX}/gen/aurora" CACHE PATH "Aurora source (Switch-patched)" FORCE)
  include("${REPO}/switch/cmake/Graphics.cmake")
  set(CMAKE_CXX_STANDARD_LIBRARIES
    "-Wl,--start-group -lstdc++ -lsupc++ -lpthread -lnx -lsysbase -lc -lm -lgcc -lunwind -lnx ${MELEE_NVK_ARCHIVES} -ljpeg -lexpat -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lzstd -lz -Wl,--end-group ${MELEE_LIBGCC_DIR}/crtend.o ${MELEE_LIBGCC_DIR}/crtn.o"
    CACHE STRING "" FORCE)
elseif(MU_FE_VARIANT STREQUAL "linux")
  # The same Dawn revision and Aurora source as the Switch build, for Linux (build.sh deps).
  set(_dawn_build "${GFX}/dawn-linux")
  add_library(dawn::webgpu_dawn STATIC IMPORTED GLOBAL)
  set_target_properties(dawn::webgpu_dawn PROPERTIES
    IMPORTED_LOCATION "${_dawn_build}/src/dawn/native/libwebgpu_dawn.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_dawn_build}/gen/include;${REPO}/ref/dawn/include"
    INTERFACE_LINK_LIBRARIES "X11;X11-xcb;xcb;dl;pthread")
  add_library(dawn::dawncpp_headers INTERFACE IMPORTED GLOBAL)
  set_target_properties(dawn::dawncpp_headers PROPERTIES INTERFACE_LINK_LIBRARIES dawn::webgpu_dawn)
  foreach(_name flat_hash_map btree)
    add_library(absl::${_name} INTERFACE IMPORTED GLOBAL)
    set_target_properties(absl::${_name} PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${REPO}/ref/dawn/third_party/abseil-cpp"
      INTERFACE_LINK_LIBRARIES dawn::webgpu_dawn)
  endforeach()
  set(Dawn_FOUND TRUE)
  set(AURORA_DAWN_PROVIDER system CACHE STRING "" FORCE)
  set(AURORA_SDL3_PROVIDER system CACHE STRING "" FORCE)
  set(AURORA_SDL3_LINKAGE static CACHE STRING "" FORCE)
  set(SDL3_DIR "${GFX}/sdl-linux" CACHE PATH "" FORCE)
  foreach(_off DVD CARD THP RMLUI)
    set(AURORA_ENABLE_${_off} OFF CACHE BOOL "" FORCE)
  endforeach()
  set(AURORA_ENABLE_GX ON CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF)
  set(TRACY_ENABLE OFF CACHE BOOL "" FORCE)
  # build.sh sync's copy of build/sdk/aurora with patches/01xx-aurora-*.patch applied.
  add_subdirectory("${GFX}/gen/aurora" aurora EXCLUDE_FROM_ALL)
  target_include_directories(aurora_gx PRIVATE "${REPO}/switch/src")
  target_include_directories(aurora_core PRIVATE "${REPO}/switch/src")
  set(MELEE_AURORA_SOURCE "${GFX}/gen/aurora")
elseif(NOT MU_FE_VARIANT STREQUAL "null")
  message(FATAL_ERROR "MU_FE_VARIANT must be null, linux or switch")
endif()

# ---- melee-unlocked's headless host services and the unchanged runner (as experiments/native-engine/build.sh builds them) ----
set(MU_HOST_SOURCES
  "${MU}/port/linux/host_services.cpp" "${MU}/port/linux/input_script.cpp"
  "${MU}/port/runtime/hle/ax_ucode.cpp" "${MU}/port/runtime/hle/native_savestate.cpp"
  "${RUN}/runner_main.cpp" "${RUN}/mu_elf.cpp" "${RUN}/rollback.cpp" "${RUN}/net_host.cpp" "${RUN}/hostprof.cpp" "${RUN}/mu_imports.c"
  "${GEN}/embedded_tables.cpp" "${GEN}/api_names.cpp" "${GEN}/embedded.S" "${FE}/play_main.cpp" "${FE}/mods.cpp")
if(MU_FE_VARIANT STREQUAL "switch")
  list(APPEND MU_HOST_SOURCES "${RUN}/fixedmap_horizon.c" "${RUN}/dirtytrack_horizon.c")
  # Its own target: the toolchain names objects without the source extension, and dirtytrack_horizon.c sits beside it.
  add_library(mu_host_entry OBJECT "${RUN}/dirtytrack_horizon.S")
  list(APPEND MU_HOST_SOURCES $<TARGET_OBJECTS:mu_host_entry>)
else()
  list(APPEND MU_HOST_SOURCES "${RUN}/fixedmap_linux.c" "${RUN}/dirtytrack_linux.c")
endif()
# Its own target: the Switch toolchain names objects without the source extension, and hostprof.cpp sits beside it.
add_library(mu_host_prof OBJECT "${RUN}/hostprof.S")
add_library(mu_host STATIC ${MU_HOST_SOURCES} $<TARGET_OBJECTS:mu_host_prof>)
set_target_properties(mu_host PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS ON)
target_include_directories(mu_host PRIVATE "${RUN}" "${MU}/port/linux" "${MU}/port/runtime/abi" "${MU}/port/runtime/hle"
  "${MU}/port/runtime/host" "${MU}/port/runtime/ppc" "${MU}/port/runtime/gx" "${MU}/native")
target_include_directories(mu_host PRIVATE "${FE}")
target_include_directories(mu_host PRIVATE "${REPO}/port/runtime/host")   # last: sha256.h for mods.cpp
target_compile_definitions(mu_host PRIVATE MU_FOREIGN_ELF MU_SQUAD=1 MELEE_NO_SLIPPI MELEE_PORT_VERSION="mu-native" MELEE_LINUX_CPU_BASELINE="AArch64")
target_compile_options(mu_host PRIVATE -O2 -g -Wall -fno-strict-aliasing $<$<COMPILE_LANGUAGE:CXX>:-ffp-contract=on>)
# The runner's main becomes mu_runner_main (frontend_main.cpp calls it).
# Its check of the process layout is answered by frontend_main.cpp, which has already made it with the whole command line.
set_source_files_properties("${RUN}/runner_main.cpp" PROPERTIES COMPILE_DEFINITIONS "main=mu_runner_main;fixedmap_startup=mu_runner_fixedmap_startup")
# embedded.S names its .incbin files relative to the repository root.
set_source_files_properties("${GEN}/embedded.S" PROPERTIES COMPILE_OPTIONS "-I${REPO}")
if(NOT MU_FE_VARIANT STREQUAL "switch")
  target_compile_definitions(mu_host PRIVATE MU_CARD_SIMPLE_COMMIT)
endif()

# ---- the mod installer (src/modinstall/: plain C++, also built for macOS by ../tools/mod_install_test.sh) ----
# The LZMA SDK's decoders (public domain) from the fetched tree's port/third_party/lzma; zlib from the system or devkitPro;
# nlohmann/json from this repository's port/third_party. The catalog is melee-unlocked's mods-catalog-2.json, built in.
set(MU_MOD_CATALOG "${REPO}/build/native-engine/mu/mods-catalog-2.json")
if(NOT EXISTS "${MU_MOD_CATALOG}")
  message(FATAL_ERROR "missing ${MU_MOD_CATALOG}: run ../fetch.sh first")
endif()
file(READ "${MU_MOD_CATALOG}" _catalog)
string(FIND "${_catalog}" ")mucatalog\"" _bad)
if(NOT _bad EQUAL -1)
  message(FATAL_ERROR "mods-catalog-2.json contains the raw-string delimiter")
endif()
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/gen_mods/mods_catalog_json.h" CONTENT
  "// Generated from melee-unlocked's mods-catalog-2.json (the fetched tree) by frontend/CMakeLists.txt.\n#pragma once\nstatic const char kModsCatalogJson[] = R\"mucatalog(@_catalog@)mucatalog\";\n" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MU_MOD_CATALOG}")
add_library(mu_modinstall STATIC
  "${FE}/modinstall/mi_hash.cpp" "${FE}/modinstall/mi_io.cpp" "${FE}/modinstall/mi_archive.cpp" "${FE}/modinstall/mi_lzma.cpp"
  "${FE}/modinstall/mi_vcdiff.cpp" "${FE}/modinstall/mi_catalog.cpp" "${FE}/modinstall/mod_install.cpp"
  "${MU}/port/third_party/lzma/LzmaDec.c" "${MU}/port/third_party/lzma/Lzma2Dec.c")
target_include_directories(mu_modinstall PRIVATE "${MU}/port/third_party/lzma" "${REPO}/port/third_party")
target_compile_options(mu_modinstall PRIVATE -O2 -g -Wall)
if(MU_FE_VARIANT STREQUAL "switch")
  target_include_directories(mu_modinstall SYSTEM PRIVATE "${DEVKITPRO}/portlibs/switch/include" "${DEVKITPRO}/libnx/include")
endif()

# ---- this repository's GX decoder and renderer bridge, on a small host shim ----
add_library(mu_gx STATIC
  "${GEN}/gx_core.cpp" "${GXSRC}/gx_texture.cpp" "${GEN}/gx_aurora_bridge.cpp" "${GEN}/subframe.cpp" "${GXSRC}/authored_pose.cpp" "${GXSRC}/gx_shader.cpp"
  "${FE}/shim/shim.cpp" "${FE}/frontend.cpp" "${FE}/play_io.cpp" "${FE}/settings.cpp" "${FE}/slippi_files.cpp"
  # This repository's VCDIFF decoder and Slippi link resolver (neither includes the app's host.h).
  "${REPO}/port/runtime/host/vcdiff.cpp" "${REPO}/port/runtime/hle/slippi_hsd_link.cpp")
# The shim's host.h and ppc.h come first; the real render_observer.h and fatal_boundary.h are found after them.
target_include_directories(mu_gx PRIVATE "${FE}/shim" "${FE}" "${GXSRC}" "${RUN}" "${REPO}/port/runtime/ppc" "${REPO}/port/runtime/host"
  "${MU}/port/linux" "${MU}/port/runtime/abi"
  "${MU}/port/runtime/host")   # last: only for hackpack_source.h (settings.cpp); every other name is found before it
target_include_directories(mu_gx PRIVATE "${REPO}/port/runtime/hle" "${REPO}/native")   # slippi_hsd_link.h, jukebox.h
target_include_directories(mu_gx PRIVATE "${GEN}")   # app/game_menu.h: the build-time copy with patches/0104
target_compile_options(mu_gx PRIVATE -O2 -g -Wall -fno-strict-aliasing)
# The app (app.cpp: folders, launcher.ini, controllers, HUD; app_menu.cpp: the in-game menu) and the feature hooks' calls.
target_sources(mu_gx PRIVATE "${FE}/app.cpp" "${FE}/software_keyboard.cpp")
# A mod disc's content fingerprint as melee-unlocked computes it (mod_fingerprint.cpp over its source_mod_overlay.cpp and
# layer_fingerprint.h from the fetched tree): the 2D-style m-ex build is known by it (mods.cpp).
target_sources(mu_gx PRIVATE "${FE}/mod_fingerprint.cpp" "${MU}/port/runtime/host/source_mod_overlay.cpp")
set_source_files_properties("${MU}/port/runtime/host/source_mod_overlay.cpp" PROPERTIES COMPILE_OPTIONS "-std=gnu++17")   # upstream's u8string is a std::string there
target_compile_definitions(mu_gx PRIVATE MU_APP_VERSION="${MU_APP_VERSION}" MU_SLIPPI_VERSION="${MU_SLIPPI_VERSION}")
if(MU_FE_VARIANT STREQUAL "null")
  target_compile_definitions(mu_gx PRIVATE MU_FE_AURORA=0)
else()
  # The renderer, and the app's controller input and sound output: unchanged sources of port/runtime/host,
  # compiled from the verbatim copies build.sh sync makes in ${GEN}/app (see there).
  target_sources(mu_gx PRIVATE "${GEN}/gx_aurora.cpp"   # with patches/0106
    "${GEN}/app/input_sdl.cpp" "${GEN}/app/input_script.cpp" "${GEN}/app/audio_sdl.cpp")
  # The app's in-game menu: melee-unlocked's settings panel (mu_panel.cpp, its drawing code from the fetched tree in
  # ${GEN}/mu_ui, scripts/mu_ui_extract.py) behind the app's menu interface (game_menu.h, the build-time copy with
  # patches/0104 and 0111; game_menu.cpp itself is no longer built). The HUD line through overlay_imgui.cpp, the
  # controller configuration (input_config.cpp) and the MetalFrontend the screens before the game take.
  target_sources(mu_gx PRIVATE "${FE}/app_menu.cpp" "${FE}/overlay_draw.cpp" "${FE}/mu_panel.cpp" "${GEN}/app/overlay_imgui.cpp"
    "${APPSRC}/input_config.cpp" "${APPSRC}/metal_frontend.cpp")
  set_source_files_properties("${FE}/mu_panel.cpp" PROPERTIES INCLUDE_DIRECTORIES "${GEN}/mu_ui")
  # melee-unlocked's character select easter egg, from the fetched tree (gen/mu_ui/egg) over two small stand-ins (src/egg_shim).
  target_sources(mu_gx PRIVATE "${FE}/egg.cpp")
  set_source_files_properties("${FE}/egg.cpp" PROPERTIES INCLUDE_DIRECTORIES "${FE}/egg_shim;${GEN}/mu_ui/egg")
  # The display wave (0.4.8): texture packs, ambient occlusion, the frame capture burst and the one-frame glitch detector
  # (gfx_render.cpp over frontend/patches/0124-0126), melee-unlocked's flicker_scan.cpp (build.sh sync's copy with patches/0127).
  target_sources(mu_gx PRIVATE "${FE}/gfx_render.cpp" "${GEN}/gfx/flicker_scan.cpp")
  target_include_directories(mu_gx PRIVATE "${GEN}/gfx")
  # melee-unlocked's Lab view (build.sh sync's copy in gen/lab, its host services renamed to src/lab_shim; earcut beside it).
  target_sources(mu_gx PRIVATE "${FE}/lab.cpp")
  set_source_files_properties("${FE}/lab.cpp" PROPERTIES INCLUDE_DIRECTORIES "${FE}/lab_shim;${GEN}/lab")
  # The controller setup beyond the button remap (controls_core.h, profiles and Dolphin import, its menu pages).
  target_sources(mu_gx PRIVATE "${FE}/controls.cpp" "${FE}/controls_files.cpp")
  # The mod installer's service (the app's folders, the built-in catalog, the services program's libcurl) and the
  # Mods screens of melee-unlocked's interface over it (mu_mods.cpp: the home screen's Mods page, the panel's MODS).
  target_sources(mu_gx PRIVATE "${FE}/mods_service.cpp" "${FE}/mu_mods.cpp")
  target_include_directories(mu_gx PRIVATE "${CMAKE_BINARY_DIR}/gen_mods")
  target_link_libraries(mu_gx PUBLIC mu_modinstall)
  # overlay_imgui.cpp's empty controller list stands aside for app.cpp's (the SDL gamepads Aurora opened).
  set_source_files_properties("${GEN}/app/overlay_imgui.cpp" PROPERTIES COMPILE_DEFINITIONS
    "window_list_controllers=overlay_unused_list_controllers;window_capture_input=overlay_unused_capture_input;window_test_rumble=overlay_unused_test_rumble")
  if(MU_FE_VARIANT STREQUAL "switch")
    target_sources(mu_gx PRIVATE "${GEN}/app/gc_adapter_usbhs.cpp")   # the GameCube adapter over usb:hs
    # Its poll is called through play_io.cpp, which stops it searching for an adapter every two seconds.
    set_source_files_properties("${GEN}/app/gc_adapter_usbhs.cpp" PROPERTIES COMPILE_DEFINITIONS "gcadapter_poll=gcadapter_poll_usb")
  else()
    target_sources(mu_gx PRIVATE "${GEN}/app/gc_adapter_none.cpp")
  endif()
  target_compile_definitions(mu_gx PRIVATE MU_FE_AURORA=1)
  target_include_directories(mu_gx PRIVATE "${MELEE_AURORA_SOURCE}/lib")   # gx_aurora.cpp writes Aurora's FIFO directly
  target_link_libraries(mu_gx PUBLIC aurora::core aurora::gx aurora::vi aurora::os aurora::mtx aurora::pad
    absl::flat_hash_map absl::btree SDL3::SDL3-static)
endif()

# The feature modules (feature_hooks.h): every .cpp of src/features/ and runner/features/, linked as objects so that
# play mode's weak references (feature_calls.h) find them. MU_FE_FEATURES=OFF leaves them out (play mode then runs
# without them).
# runner/features is taken from the working tree (build.sh sync copies the runner from a pinned commit).
get_filename_component(RUN_TREE "${CMAKE_CURRENT_LIST_DIR}/../runner" ABSOLUTE)
file(GLOB MU_FEATURE_SOURCES CONFIGURE_DEPENDS "${FE}/features/*.cpp" "${RUN_TREE}/features/*.cpp")
if(MU_FE_FEATURES AND MU_FEATURE_SOURCES)
  add_library(mu_features OBJECT ${MU_FEATURE_SOURCES})
  # PhobGCC settings on a stock controller: the report path (src/phob_adapter.cpp, patches/0123) and melee-unlocked's firmware
  # port, compiled from the fetched tree (host/phob.cpp, GPL-3.0-only).
  target_sources(mu_features PRIVATE "${FE}/phob_adapter.cpp" "${MU}/port/runtime/host/phob.cpp")
  set_target_properties(mu_features PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS ON)
  target_include_directories(mu_features PRIVATE "${FE}" "${FE}/features" "${RUN}" "${RUN_TREE}" "${RUN_TREE}/features" "${MU}/port/linux" "${MU}/port/runtime/abi" "${MU}/port/runtime/hle"
    "${MU}/port/runtime/host" "${MU}/port/runtime/ppc" "${MU}/port/runtime/gx" "${MU}/native")
  target_compile_definitions(mu_features PRIVATE MU_FOREIGN_ELF MU_SQUAD=1 MELEE_NO_SLIPPI MELEE_PORT_VERSION="mu-native")
  target_compile_options(mu_features PRIVATE -O2 -g -Wall -fno-strict-aliasing)
  target_include_directories(mu_features PRIVATE "${GEN}")   # native_recording_codes.h
  # replays.cpp compiles the fetched tree's port/app/replay_slpz.cpp, which includes "zstd/zstd.h" (upstream vendors zstd):
  # here that name leads to the system's zstd (Linux) or devkitPro's portlib (Switch), so header and library match.
  file(WRITE "${CMAKE_BINARY_DIR}/zstdshim/zstd/zstd.h" "#include <zstd.h>\n")
  target_include_directories(mu_features PRIVATE "${CMAKE_BINARY_DIR}/zstdshim")
  # replay_coach_glue.cpp compiles the fetched tree's replay_scan.cpp, which includes "slippilib/SlippiGame.h": that name
  # must lead to the fetched tree's copy (the one replays.cpp compiles), not to this repository's older port/third_party one.
  file(WRITE "${CMAKE_BINARY_DIR}/slpshim/slippilib/SlippiGame.h" "#include \"${MU}/port/third_party/slippilib/SlippiGame.h\"\n")
  target_include_directories(mu_features BEFORE PRIVATE "${CMAKE_BINARY_DIR}/slpshim")
  # spectator.cpp: the Slippi spectator server's own ENet (the netplay object's copy is local to that object) and
  # nlohmann's JSON, both this repository's port/third_party.
  set(_enet "${REPO}/port/third_party/enet")
  set(MU_ENET_SOURCES "${_enet}/callbacks.c" "${_enet}/compress.c" "${_enet}/host.c" "${_enet}/list.c" "${_enet}/packet.c"
    "${_enet}/peer.c" "${_enet}/protocol.c" "${_enet}/unix.c")
  target_sources(mu_features PRIVATE ${MU_ENET_SOURCES})
  set_source_files_properties(${MU_ENET_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "HAS_FCNTL=1;HAS_POLL=1;HAS_INET_PTON=1;HAS_INET_NTOP=1;HAS_SOCKLEN_T=1;HAS_MSGHDR_FLAGS=1"
    COMPILE_OPTIONS "-w;-include;arpa/inet.h")
  target_include_directories(mu_features PRIVATE "${_enet}/include" "${REPO}/port/third_party")
  if(MU_FE_VARIANT STREQUAL "switch")
    target_include_directories(mu_features SYSTEM PRIVATE "${DEVKITPRO}/portlibs/switch/include" "${DEVKITPRO}/libnx/include")
  endif()
  message(STATUS "feature modules: ${MU_FEATURE_SOURCES}")
else()
  message(STATUS "feature modules: none linked")
endif()

# Fight a pro (features/proai.cpp): the app's own inference engine (src/proai/onnx_lite.cpp; ort_lite.cpp gives it the
# onnxruntime-shaped calls upstream's code makes) and melee-unlocked's phillip_ai.cpp (build-time copy with patches/0112),
# compiled on src/proai/include's headers; the catalog (src/proai/catalog.json) embedded. The Practice menu
# (practice_menu.cpp with patches/0113, drawn by src/proai/practice_overlay.cpp) needs Aurora's ImGui: picture variants.
if(TARGET mu_features)
  file(READ "${FE}/proai/catalog.json" _proai_catalog)
  file(WRITE "${CMAKE_BINARY_DIR}/proai_catalog.cpp.new"
    "// Generated from src/proai/catalog.json (CMakeLists.txt).\nextern const char fe_proai_catalog_json[];\nconst char fe_proai_catalog_json[] = R\"PROAICATALOG(${_proai_catalog})PROAICATALOG\";\n")
  file(COPY_FILE "${CMAKE_BINARY_DIR}/proai_catalog.cpp.new" "${CMAKE_BINARY_DIR}/proai_catalog.cpp" ONLY_IF_DIFFERENT)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FE}/proai/catalog.json")
  add_library(mu_proai OBJECT "${FE}/proai/onnx_lite.cpp" "${FE}/proai/ort_lite.cpp" "${GEN}/proai/phillip_ai.cpp" "${CMAKE_BINARY_DIR}/proai_catalog.cpp")
  set_target_properties(mu_proai PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS ON)
  target_include_directories(mu_proai PRIVATE "${FE}/proai/include" "${FE}/proai" "${GEN}/proai" "${MU}/port/runtime/hle" "${MU}/port/runtime/abi"
    "${REPO}/port/third_party")
  target_compile_definitions(mu_proai PRIVATE MU_PROAI_FRONTEND=1)
  target_compile_options(mu_proai PRIVATE -O2 -g -Wall -fno-strict-aliasing)
  if(MU_FE_VARIANT STREQUAL "switch")
    target_include_directories(mu_proai SYSTEM PRIVATE "${DEVKITPRO}/portlibs/switch/include" "${DEVKITPRO}/libnx/include")
  endif()
  if(NOT MU_FE_VARIANT STREQUAL "null")
    add_library(mu_practice_menu OBJECT "${GEN}/proai/practice_menu.cpp" "${FE}/proai/practice_overlay.cpp"
      "${MU}/port/runtime/hle/native_practice_model.cpp")   # upstream's connect code rules (the Search page)
    set_target_properties(mu_practice_menu PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS ON)
    target_include_directories(mu_practice_menu PRIVATE "${FE}/proai/include" "${FE}/proai" "${GEN}/proai" "${MU}/port/runtime/hle"
      "${MU}/port/runtime/abi")
    target_compile_definitions(mu_practice_menu PRIVATE MU_PROAI_FRONTEND=1)
    target_compile_options(mu_practice_menu PRIVATE -O2 -g -Wall -fno-strict-aliasing -Wno-unused-function -Wno-unused-variable)
    target_link_libraries(mu_practice_menu PRIVATE aurora::core)
    if(MU_FE_VARIANT STREQUAL "switch")
      target_include_directories(mu_practice_menu SYSTEM PRIVATE "${DEVKITPRO}/portlibs/switch/include" "${DEVKITPRO}/libnx/include")
    endif()
  endif()
endif()

# The app's screens before the game, for the services program with a picture: the self-update from the player's build
# computer (self_update*.cpp) and the Slippi sign-in (switch_signin.cpp with patches/0105, slippi_login.cpp, HTTPS
# through libcurl). The netplay object has its own copies of the last two, local to it.
if(NOT MU_FE_VARIANT STREQUAL "null")
  add_library(mu_app_services OBJECT "${FE}/app_services.cpp" "${FE}/mu_home.cpp" "${FE}/mu_lang.cpp" "${FE}/diagnostics.cpp" "${GEN}/app/switch_signin.cpp"
    "${APPSRC}/self_update.cpp" "${APPSRC}/self_update_curl.cpp" "${REPO}/port/runtime/hle/slippi_login.cpp" "${REPO}/port/runtime/hle/slippi_http_curl.cpp"
    "${FE}/modinstall/mi_fetch_curl.cpp")   # the mod installer's downloads (mods_service.cpp finds it through a weak reference)
  target_include_directories(mu_app_services PRIVATE "${FE}/shim" "${FE}" "${GXSRC}" "${RUN}" "${REPO}/port/runtime/ppc" "${APPSRC}"
    "${REPO}/port/runtime/hle" "${REPO}/port/third_party" "${MU}/port/linux" "${MU}/port/runtime/abi" "${GEN}/mu_ui" "${MU}/port/app")
  target_compile_definitions(mu_app_services PRIVATE MU_FE_AURORA=1 MU_APP_VERSION="${MU_APP_VERSION}" MU_SLIPPI_VERSION="${MU_SLIPPI_VERSION}")
  target_compile_options(mu_app_services PRIVATE -O2 -g -Wall -fno-strict-aliasing)
  target_link_libraries(mu_app_services PRIVATE aurora::core aurora::gx aurora::pad SDL3::SDL3-static)
  if(MU_FE_VARIANT STREQUAL "switch")
    target_include_directories(mu_app_services SYSTEM PRIVATE "${DEVKITPRO}/portlibs/switch/include" "${DEVKITPRO}/libnx/include")
    target_include_directories(mu_app_services PRIVATE "${REPO}/switch/src")
  endif()
endif()

# The free DSP coefficient table (Dolphin's free DSP ROM table v0.2.1, GPL-2.0-or-later; melee-unlocked ships it as
# Sys/GC/dsp_coef.bin, docs/licenses/dsp-coefficients.txt), embedded from the fetched tree for features/options.cpp
# (settings.ini dsp_lowpass). Never a console dump: a player's own User/GC/dsp_coef.bin is read from the SD card.
set(_dsp_coef "${MU}/port/slippi_sys/GC/dsp_coef.bin")
set(_dsp_bytes "0")
if(EXISTS "${_dsp_coef}")
  file(READ "${_dsp_coef}" _dsp_hex HEX)
  string(REGEX REPLACE "(..)" "0x\\1," _dsp_bytes "${_dsp_hex}")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/dsp_coef_free.cpp.new"
  "// Generated from the fetched tree's port/slippi_sys/GC/dsp_coef.bin (CMakeLists.txt).\nextern const unsigned char fe_dsp_coef_free[];\nextern const unsigned fe_dsp_coef_free_size;\nconst unsigned char fe_dsp_coef_free[] = {${_dsp_bytes}};\nconst unsigned fe_dsp_coef_free_size = sizeof fe_dsp_coef_free;\n")
file(COPY_FILE "${CMAKE_BINARY_DIR}/dsp_coef_free.cpp.new" "${CMAKE_BINARY_DIR}/dsp_coef_free.cpp" ONLY_IF_DIFFERENT)

# The presenter (features/present: display-rate presentation, sub-frames): objects, so the weak references of the
# feature modules and frontend.cpp find it. Built with the renderer's include directories and definitions.
add_library(mu_present OBJECT "${FE}/features/present/present.cpp")
target_include_directories(mu_present PRIVATE $<TARGET_PROPERTY:mu_gx,INCLUDE_DIRECTORIES>)
target_compile_definitions(mu_present PRIVATE $<TARGET_PROPERTY:mu_gx,COMPILE_DEFINITIONS>)
target_compile_options(mu_present PRIVATE -O2 -g -Wall -fno-strict-aliasing)
if(NOT MU_FE_VARIANT STREQUAL "null")
  target_link_libraries(mu_present PRIVATE aurora::core aurora::gx)
endif()
# Use custom music (features/audio.cpp): melee-unlocked's jukebox (build.sh sync's copy with patches/0118, on the few host
# services src/jukebox_shim/ declares) and the player-file decoders (src/media_decode.cpp over minimp3, stb_vorbis, dr_wav
# and dr_flac, fetched by ../fetch.sh into build/native-engine/audio-decoders). Builds with sound output only.
set(MU_AUDIO_DECODERS "${REPO}/build/native-engine/audio-decoders")
if(NOT MU_FE_VARIANT STREQUAL "null" AND EXISTS "${GEN}/jukebox/jukebox.cpp" AND EXISTS "${MU_AUDIO_DECODERS}/minimp3_ex.h")
  add_library(mu_jukebox OBJECT "${GEN}/jukebox/jukebox.cpp" "${FE}/media_decode.cpp")
  set_target_properties(mu_jukebox PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS ON)   # upstream builds it as C++17 (u8string is a std::string there)
  target_include_directories(mu_jukebox BEFORE PRIVATE "${FE}/jukebox_shim")
  target_include_directories(mu_jukebox PRIVATE "${MU}/port/runtime/hle" "${MU}/port/linux" "${MU_AUDIO_DECODERS}")
  # Only audio_sinc.h is wanted from here; host.h and test_env.h are found in jukebox_shim first.
  target_include_directories(mu_jukebox PRIVATE "${MU}/port/runtime/host")
  target_compile_options(mu_jukebox PRIVATE -O2 -g -fno-strict-aliasing)
  set_source_files_properties("${FE}/media_decode.cpp" PROPERTIES COMPILE_OPTIONS "-w")   # the decoders' own warnings
  if(MU_FE_VARIANT STREQUAL "switch")
    target_include_directories(mu_jukebox SYSTEM PRIVATE "${DEVKITPRO}/portlibs/switch/include" "${DEVKITPRO}/libnx/include")
  endif()
else()
  message(STATUS "Use custom music: no jukebox in this variant (null, or build.sh sync / ../fetch.sh not run)")
endif()

# The player's Gecko codes (features/gecko.cpp): melee-unlocked's user_gecko.cpp, build.sh sync's copy with patches/0122, on
# src/gecko_shim/gecko_host_shim.h; its variable table (user_gecko_targets.h) and function tables from the fetched tree.
if(EXISTS "${GEN}/gecko/user_gecko.cpp")
  add_library(mu_gecko OBJECT "${GEN}/gecko/user_gecko.cpp")
  set_target_properties(mu_gecko PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS ON)
  target_include_directories(mu_gecko BEFORE PRIVATE "${FE}/gecko_shim")
  target_include_directories(mu_gecko PRIVATE "${MU}/port/runtime/host" "${REPO}/build/native-engine/mu/sourceport/game/shim")   # the function tables (the fetched tree)
  target_compile_definitions(mu_gecko PRIVATE MELEE_NO_SLIPPI MNX_NO_FUNCTION_PATCHES)
  target_compile_options(mu_gecko PRIVATE -O2 -g -fno-strict-aliasing)
  if(MU_FE_VARIANT STREQUAL "switch")
    target_include_directories(mu_gecko SYSTEM PRIVATE "${DEVKITPRO}/portlibs/switch/include" "${DEVKITPRO}/libnx/include")
  endif()
endif()

# RVZ and WIA discs (app.cpp app_disc_convert: a GALE01.rvz or .wia becomes GALE01.iso at start): melee-unlocked's disc_rvz.cpp and the
# bzip2 it vendors, compiled from the fetched tree; LZMA from mu_modinstall's LZMA SDK objects, Zstandard the system's or
# devkitPro's (the same one replays.cpp links). Builds with a picture only (the app).
if(NOT MU_FE_VARIANT STREQUAL "null" AND EXISTS "${MU}/port/app/disc_rvz.cpp")
  set(_tp "${MU}/port/third_party")
  add_library(mu_rvz OBJECT "${MU}/port/app/disc_rvz.cpp" "${_tp}/bzip2/blocksort.c" "${_tp}/bzip2/huffman.c" "${_tp}/bzip2/crctable.c"
    "${_tp}/bzip2/randtable.c" "${_tp}/bzip2/compress.c" "${_tp}/bzip2/decompress.c" "${_tp}/bzip2/bzlib.c")
  set_target_properties(mu_rvz PROPERTIES CXX_STANDARD 17)
  file(WRITE "${CMAKE_BINARY_DIR}/zstdshim/zstd/zstd.h" "#include <zstd.h>\n")
  target_include_directories(mu_rvz BEFORE PRIVATE "${CMAKE_BINARY_DIR}/zstdshim")
  target_include_directories(mu_rvz PRIVATE "${_tp}")
  target_compile_definitions(mu_rvz PRIVATE BZ_EXPORT)
  target_compile_options(mu_rvz PRIVATE -O2 $<$<COMPILE_LANGUAGE:C>:-w>)
  if(MU_FE_VARIANT STREQUAL "switch")
    target_include_directories(mu_rvz SYSTEM PRIVATE "${DEVKITPRO}/portlibs/switch/include" "${DEVKITPRO}/libnx/include")
  endif()
  target_compile_definitions(mu_gx PRIVATE MU_FE_RVZ=1)
  target_include_directories(mu_gx PRIVATE "${MU}/port/app")   # disc_rvz.h for app.cpp (app_disc_convert)
endif()

# The program, twice: with the netplay object of ../build.sh (copied by build.sh sync: everything behind net_host,
# its other symbols local), and with the services variant of that object when there is one.
function(mu_program target netplay_object suffix)
  add_executable(${target} "${FE}/frontend_main.cpp" "${FE}/log_queue.cpp" "${CMAKE_BINARY_DIR}/dsp_coef_free.cpp")
  if(suffix)
    target_sources(${target} PRIVATE "${FE}/services_check.cpp")   # the services program's start-up self-check
    if(TARGET mu_proai)
      target_sources(${target} PRIVATE "${FE}/proai/proai_fetch_curl.cpp")   # Fight a pro's downloads (libcurl)
      set_source_files_properties("${FE}/proai/proai_fetch_curl.cpp" PROPERTIES INCLUDE_DIRECTORIES "${REPO}/port/runtime/host")
    endif()
    if(TARGET mu_app_services)
      target_sources(${target} PRIVATE $<TARGET_OBJECTS:mu_app_services>)   # the app's update and sign-in screens
    endif()
  endif()
  target_sources(${target} PRIVATE $<TARGET_OBJECTS:mu_present>)
  if(TARGET mu_features)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:mu_features>)
  endif()
  if(TARGET mu_proai)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:mu_proai>)
  endif()
  if(TARGET mu_jukebox)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:mu_jukebox>)
  endif()
  if(TARGET mu_rvz)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:mu_rvz>)
  endif()
  if(TARGET mu_gecko AND TARGET mu_features)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:mu_gecko>)
  endif()
  if(TARGET mu_practice_menu)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:mu_practice_menu>)
  endif()
  target_include_directories(${target} PRIVATE "${FE}" "${RUN}")
  target_link_libraries(${target} PRIVATE mu_gx mu_host mu_gx "${netplay_object}")
  # Lines the netplay object prints with fprintf(stderr) go to the log queue while the game runs (log_queue.cpp).
  target_link_options(${target} PRIVATE -Wl,--wrap=fprintf)
  if(NOT MU_FE_VARIANT STREQUAL "null")
    # The render thread's time inside Aurora's two frame calls is measured through these (frontend.cpp).
    target_link_options(${target} PRIVATE -Wl,--wrap=aurora_begin_frame -Wl,--wrap=aurora_end_frame)
  endif()
  if(MU_FE_VARIANT STREQUAL "switch")
    # What Aurora's patched objects and the Clang/libnx runtime need from the app's Switch layer.
    target_sources(${target} PRIVATE "${REPO}/switch/src/switch_threads.c" "${REPO}/switch/src/perf_switch.cpp"
      "${REPO}/switch/src/libc_switch.c" "${REPO}/switch/src/switch_clocks.cpp" "${FE}/switch_glue.cpp")
    target_include_directories(${target} SYSTEM PRIVATE "${DEVKITPRO}/portlibs/switch/include" "${DEVKITPRO}/libnx/include")
    target_include_directories(${target} PRIVATE "${REPO}/switch/src")
    if(MELEE_NVK_STUBS_SOURCE)
      target_sources(${target} PRIVATE "${MELEE_NVK_STUBS_SOURCE}")   # Rust/newlib ABI bridge for the NVK archives
    endif()
    target_link_options(${target} PRIVATE
      -fuse-ld=${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-ld
      -stdlib=libstdc++
      -L${MELEE_LIBCXX_PIC_DIR} -L${MELEE_LIBGCC_DIR} -L${MELEE_UNWIND_STUB_DIR}
      -L${DEVKITPRO}/libnx/lib -L${DEVKITPRO}/portlibs/switch/lib
      ${MELEE_LIBGCC_DIR}/crti.o ${MELEE_LIBGCC_DIR}/crtbegin.o
      -Wl,-T,${DEVKITPRO}/libnx/switch.ld -Wl,-pie -Wl,--no-dynamic-linker -Wl,-z,text -Wl,-z,now
      -Wl,--build-id=sha1 -Wl,--require-defined=main -nodefaultlibs -Wl,--gc-sections
      -Wl,--allow-multiple-definition
      -Wl,--wrap=pthread_create -Wl,--wrap=pthread_detach -Wl,--wrap=pthread_join
      -Wl,--wrap=exit -Wl,--wrap=abort -Wl,--wrap=_exit
      # The runner's text console would take the display: see frontend_main.cpp.
      -Wl,--wrap=consoleInit -Wl,--wrap=consoleUpdate -Wl,--wrap=consoleExit)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME mu-native-gfx${suffix})
    # The services program is the app: melee-unlocked's own icon (build.sh sync converts its melee_unlocked.ico from the
    # fetched tree into gen/mu_ui/icon.jpg); the unbranded switch/icon.jpg otherwise.
    set(_icon "${REPO}/switch/icon.jpg")
    if(suffix AND EXISTS "${GEN}/mu_ui/icon.jpg")
      set(_icon "${GEN}/mu_ui/icon.jpg")
    endif()
    if(suffix)
      nx_generate_nacp(mu-native-gfx${suffix}.nacp NAME "Melee-NX-NetPlay" AUTHOR "Melee-NX-NetPlay" VERSION "${MU_APP_VERSION}")
    else()
      nx_generate_nacp(mu-native-gfx${suffix}.nacp NAME "mu-native-gfx" AUTHOR "Melee-NX-NetPlay" VERSION "${MU_APP_VERSION}")
    endif()
    nx_create_nro(${target} NACP mu-native-gfx${suffix}.nacp ICON "${_icon}")
  else()
    set_target_properties(${target} PROPERTIES OUTPUT_NAME mu-native-gfx-${MU_FE_VARIANT}${suffix})
    # A position-independent executable keeps the host away from the fixed game addresses.
    target_link_options(${target} PRIVATE -pie)
    target_link_libraries(${target} PRIVATE pthread zstd z m)
    if(suffix)
      target_link_libraries(${target} PRIVATE curl)
    endif()
  endif()
endfunction()

if(MU_FE_VARIANT STREQUAL "switch")
  target_include_directories(mu_gx SYSTEM PRIVATE "${DEVKITPRO}/portlibs/switch/include" "${DEVKITPRO}/libnx/include")
  target_include_directories(mu_host SYSTEM PRIVATE "${DEVKITPRO}/portlibs/switch/include" "${DEVKITPRO}/libnx/include")
  # The runner and the frontend use the app's clock module (switch_clocks.h).
  target_include_directories(mu_host PRIVATE "${REPO}/switch/src")
  target_include_directories(mu_gx PRIVATE "${REPO}/switch/src")
  set(MU_NET_DIR "${GFX}/net/obj-nx")
else()
  set(MU_NET_DIR "${GFX}/net/obj-linux")
endif()
# Sanitizer test builds (Linux only; build.sh with MU_SANITIZE=address|thread): this program's own code and the
# services netplay object (built with the same flag by build.sh into obj-linux-services-<asan|tsan>) are instrumented;
# Aurora, Dawn, SDL and the game image are not. The outputs get the suffix -asan or -tsan.
set(MU_FE_SANITIZE "" CACHE STRING "address or thread (Linux test builds only)")
set(MU_NET_SERVICES_DIR "${MU_NET_DIR}-services")
if(MU_FE_SANITIZE)
  if(MU_FE_VARIANT STREQUAL "switch")
    message(FATAL_ERROR "MU_FE_SANITIZE is for the Linux variants")
  endif()
  string(REPLACE "address" "asan" _san_suffix "${MU_FE_SANITIZE}")
  string(REPLACE "thread" "tsan" _san_suffix "${_san_suffix}")
  set(MU_NET_SERVICES_DIR "${MU_NET_DIR}-services-${_san_suffix}")
endif()
mu_program(mu_native_gfx "${MU_NET_DIR}/netplay-weak.o" "")
if(EXISTS "${MU_NET_SERVICES_DIR}/netplay-weak.o")
  mu_program(mu_native_gfx_services "${MU_NET_SERVICES_DIR}/netplay-weak.o" "-services")
else()
  message(STATUS "no services netplay object in ${MU_NET_SERVICES_DIR}: only the program without services is built")
endif()
if(MU_FE_SANITIZE)
  set(_san_flags -fsanitize=${MU_FE_SANITIZE} -fno-omit-frame-pointer)
  foreach(_t mu_host mu_gx mu_present mu_features mu_jukebox mu_gecko mu_app_services mu_native_gfx mu_native_gfx_services)
    if(TARGET ${_t})
      target_compile_options(${_t} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:${_san_flags}>)
    endif()
  endforeach()
  target_link_options(mu_native_gfx PRIVATE -fsanitize=${MU_FE_SANITIZE})
  set_target_properties(mu_native_gfx PROPERTIES OUTPUT_NAME mu-native-gfx-${MU_FE_VARIANT}-${_san_suffix})
  if(TARGET mu_native_gfx_services)
    target_link_options(mu_native_gfx_services PRIVATE -fsanitize=${MU_FE_SANITIZE})
    set_target_properties(mu_native_gfx_services PROPERTIES OUTPUT_NAME mu-native-gfx-${MU_FE_VARIANT}-services-${_san_suffix})
  endif()
endif()
