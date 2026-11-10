# The recompiled game and the host runtime, built for the Switch. The source
# lists are explicit on purpose: nothing Apple, Win32 or D3D12 enters through a
# glob, and the online transports join only when the frontend asks for them.
include("${MELEE_PORT_GENERATED_DIR}/guest_sources.cmake")
set(PORT_GEN "${MELEE_PORT_GENERATED_DIR}")

# ---- Guest: the 144 generated translation units of the recompiled game ----
add_library(guest STATIC ${GUEST_SOURCES} "${PORT_GEN}/function_names.cpp" "${PORT_GEN}/gecko_data.cpp")
target_include_directories(guest PUBLIC "${PORT_GEN}" runtime/ppc)
target_include_directories(guest PRIVATE runtime/hle)
target_link_libraries(guest PUBLIC port_build_options)
target_compile_options(guest PRIVATE -Wno-unused-label -Wno-unused-variable -Os)
target_compile_definitions(guest PRIVATE NOMINMAX)

# ---- Native GX: the decompilation's matching GX source, compiled by GCC (scalar_storage_order) ----
# tools/generate_gx_native.py writes it (with gxn_hle.cpp, the guest-register wrappers) at generation.
set(GXN_DIR "${PORT_GEN}/../gxnative")
file(GLOB GXN_C_SOURCES "${GXN_DIR}/GX*.c")
add_library(gx_native STATIC ${GXN_C_SOURCES})
target_include_directories(gx_native PRIVATE "${GXN_DIR}" "${GXN_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/runtime/gx/gxnative"
  "${PROJECT_SOURCE_DIR}/ref/melee/include")
target_compile_options(gx_native PRIVATE -O2 -ffp-contract=off -fno-strict-aliasing -w)

# ---- ENet 1.3.13 (IPv4 peer transport for Slippi netplay) ----
add_library(port_enet STATIC third_party/enet/callbacks.c third_party/enet/compress.c third_party/enet/host.c
  third_party/enet/list.c third_party/enet/packet.c third_party/enet/peer.c third_party/enet/protocol.c
  third_party/enet/unix.c)
target_include_directories(port_enet PUBLIC third_party/enet/include)
# libnx's BSD socket layer: fcntl(O_NONBLOCK), poll, inet_pton/ntop and socklen_t
# exist; the GNU *_r resolver variants do not.
target_compile_definitions(port_enet PRIVATE HAS_FCNTL=1 HAS_POLL=1 HAS_INET_PTON=1 HAS_INET_NTOP=1
  HAS_SOCKLEN_T=1 HAS_MSGHDR_FLAGS=1)
# libnx declares htons/htonl in <arpa/inet.h>, which ENet's unix.h does not include.
target_compile_options(port_enet PRIVATE -include arpa/inet.h)

# ---- Portable runtime: CPU context, HLE of the GameCube SDK, Slippi, GX decoding ----
set(PORT_PORTABLE_RUNTIME_SOURCES
  runtime/ppc/ppc_runtime.cpp runtime/ppc/interp.cpp runtime/ppc/numeric.cpp
  runtime/hle/ax_ucode.cpp runtime/hle/exi_slippi.cpp
  runtime/hle/hle_card.cpp runtime/hle/hle_mtx.cpp runtime/hle/hle_dvd.cpp runtime/hle/dvd_read_worker.cpp runtime/hle/hle_os.cpp
  runtime/hle/hle_pad.cpp runtime/hle/pad_rumble.cpp runtime/hle/hle_stubs.cpp runtime/hle/jukebox.cpp
  runtime/hle/slippi_net.cpp runtime/hle/slippi_net_diag.cpp runtime/hle/slippi_online.cpp runtime/hle/slippi_offline.cpp
  runtime/hle/slippi_playback.cpp runtime/hle/slippi_report.cpp runtime/hle/slippi_login.cpp
  runtime/hle/slippi_http_curl.cpp runtime/hle/discord_rpc_none.cpp
  runtime/host/host.cpp runtime/host/host_log.cpp runtime/host/vcdiff.cpp runtime/host/input_script.cpp
  runtime/gx/gx_core.cpp runtime/gx/gx_texture.cpp runtime/gx/gx_shader.cpp
  runtime/gx/authored_pose.cpp runtime/gx/render_observer.cpp runtime/gx/subframe.cpp
  third_party/slippilib/SlippiGame.cpp)

set(PORT_SWITCH_SOURCES
  "${PROJECT_SOURCE_DIR}/switch/src/switch_threads.c" "${PROJECT_SOURCE_DIR}/switch/src/switch_clocks.cpp"
  "${PROJECT_SOURCE_DIR}/switch/src/switch_profile.cpp" "${PROJECT_SOURCE_DIR}/switch/src/log_switch.cpp")

if(MELEE_NX_FRONTEND STREQUAL "headless")
  set(PORT_FRONTEND_SOURCES runtime/host/input_headless.cpp runtime/host/window_headless.cpp runtime/host/audio_headless.cpp)
  set(PORT_ONLINE 0)
else()
  # Aurora owns the SDL3 window (libnx NWindow) and the Dawn/Vulkan device; the
  # decoded GX frames reach it through gx_aurora. Pads come from Aurora PAD over
  # SDL3 gamepads (Joy-Con, Pro Controller, the official GameCube adapter).
  set(PORT_FRONTEND_SOURCES
    runtime/host/input_sdl.cpp runtime/host/gc_adapter_usbhs.cpp runtime/host/audio_sdl.cpp
    runtime/host/window_metal.cpp runtime/host/metal_frontend.cpp runtime/host/switch_frontend_aurora.cpp
    runtime/host/game_menu.cpp runtime/host/input_config.cpp runtime/host/overlay_imgui.cpp runtime/host/switch_signin.cpp
    runtime/gx/gx_aurora.cpp runtime/gx/gx_aurora_bridge.cpp runtime/gx/async_backend.cpp)
  option(MELEE_NX_ONLINE "Build the Slippi online services (ENet netplay, matchmaking, login, reports) into the renderer frontend" ON)
  set(PORT_ONLINE ${MELEE_NX_ONLINE})
  if(PORT_ONLINE)
    list(APPEND PORT_FRONTEND_SOURCES runtime/hle/slippi_text.cpp)
  endif()
endif()

# Natively compiled decomp C (tools/gml): the lowered object and its guest-register wrappers.
set(GML_DIR "${PORT_GEN}/../gml")
set(GML_OBJECTS)
if(EXISTS "${GML_DIR}/gml_native.o")
  set(GML_OBJECTS "${GML_DIR}/gml_native.o")
endif()
add_library(runtime_switch STATIC ${PORT_PORTABLE_RUNTIME_SOURCES} ${PORT_FRONTEND_SOURCES} ${PORT_SWITCH_SOURCES}
  runtime/gx/gxnative/gxn_glue.cpp "${GXN_DIR}/gxn_hle.cpp"
  runtime/gml/gml_glue.cpp "${GML_DIR}/gml_hle.cpp" ${GML_OBJECTS})
target_include_directories(runtime_switch PUBLIC runtime/ppc runtime/host runtime/hle runtime/gx
  "${PORT_GEN}" "${PROJECT_SOURCE_DIR}/native" "${PROJECT_SOURCE_DIR}/switch/src" third_party)
# devkitPro portlibs (curl, mbedTLS, zlib) and libnx headers.
target_include_directories(runtime_switch SYSTEM PUBLIC "${DEVKITPRO}/portlibs/switch/include" "${DEVKITPRO}/libnx/include")
target_compile_definitions(runtime_switch PUBLIC NOMINMAX
  MELEE_PORT_INPUT_MANIFEST_PATH="${MELEE_PORT_INPUT_MANIFEST}"
  MELEE_PORT_INPUT_MANIFEST_SHA256="${MELEE_PORT_INPUT_MANIFEST_SHA256}")
target_compile_definitions(runtime_switch PRIVATE MELEE_PORT_VERSION="${MELEE_PORT_VERSION}")
if(NOT PORT_ONLINE)
  target_compile_definitions(runtime_switch PUBLIC MELEE_PORT_OFFLINE=1)
else()
  target_link_libraries(runtime_switch PUBLIC port_enet)
endif()
target_link_libraries(runtime_switch PUBLIC native_animation port_build_options Threads::Threads gx_native)
target_include_directories(runtime_switch PRIVATE runtime/gx/gxnative runtime/gml)
# Line tables only (no code change): console profiles of the render thread resolve to source lines.
target_compile_options(runtime_switch PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-gline-tables-only> $<$<COMPILE_LANGUAGE:C>:-g1>)

# ---- The executable and its NRO ----
add_executable(melee_nx_netplay app/main_switch.cpp)
target_compile_definitions(melee_nx_netplay PRIVATE MELEE_PORT_VERSION="${MELEE_PORT_VERSION}")
# Repeat the runtime for the ELF linker: the guest calls HLE functions and the
# host dispatch table refers back into the guest.
target_link_libraries(melee_nx_netplay PRIVATE runtime_switch guest runtime_switch)
target_link_options(melee_nx_netplay PRIVATE
  -fuse-ld=${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-ld
  -stdlib=libstdc++
  -L${MELEE_LIBCXX_PIC_DIR} -L${MELEE_LIBGCC_DIR} -L${MELEE_UNWIND_STUB_DIR}
  -L${DEVKITPRO}/libnx/lib -L${DEVKITPRO}/portlibs/switch/lib
  ${MELEE_LIBGCC_DIR}/crti.o ${MELEE_LIBGCC_DIR}/crtbegin.o
  -Wl,-T,${DEVKITPRO}/libnx/switch.ld -Wl,-pie -Wl,--no-dynamic-linker -Wl,-z,text -Wl,-z,now
  -Wl,--build-id=sha1 -Wl,--require-defined=main -nodefaultlibs -Wl,--gc-sections
  # As in Melee-NX: the Mesa archives and newlib overlap on a few libc symbols.
  -Wl,--allow-multiple-definition
  -Wl,--wrap=pthread_create -Wl,--wrap=pthread_detach -Wl,--wrap=pthread_join
  -Wl,--wrap=exit -Wl,--wrap=abort -Wl,--wrap=_exit)
set_target_properties(melee_nx_netplay PROPERTIES OUTPUT_NAME melee-nx-netplay)
if(MELEE_NX_FRONTEND STREQUAL "aurora")
  target_compile_definitions(runtime_switch PUBLIC MELEE_NX_AURORA=1)
  # gx_aurora.cpp writes Aurora's FIFO directly (lib/gx/fifo.hpp).
  target_include_directories(runtime_switch PRIVATE "${MELEE_AURORA_SOURCE}/lib")
  target_link_libraries(runtime_switch PUBLIC aurora::core aurora::gx aurora::vi aurora::os aurora::mtx aurora::pad
    absl::flat_hash_map absl::btree SDL3::SDL3-static)
  # Symbols Aurora's patched objects and ImGui call back into: executable objects
  # are always visible to the linker, unlike archive members behind them.
  target_sources(melee_nx_netplay PRIVATE "${PROJECT_SOURCE_DIR}/switch/src/perf_switch.cpp"
    "${PROJECT_SOURCE_DIR}/switch/src/switch_compat.cpp" "${PROJECT_SOURCE_DIR}/switch/src/libc_switch.c")
  if(MELEE_NVK_STUBS_SOURCE)
    target_sources(melee_nx_netplay PRIVATE "${MELEE_NVK_STUBS_SOURCE}")   # Rust/newlib ABI bridge for the NVK archives
  endif()
endif()

# RomFS carries Slippi's Sys folder (code list, code handler, GameFiles), read at
# romfs:/Sys. Nothing from the disc is in it.
set(PORT_ROMFS "${PROJECT_BINARY_DIR}/romfs")
file(MAKE_DIRECTORY "${PORT_ROMFS}/Sys")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/slippi_sys/" DESTINATION "${PORT_ROMFS}/Sys")
file(GLOB_RECURSE PORT_SYS_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/slippi_sys/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PORT_SYS_FILES})

nx_generate_nacp(melee-nx-netplay.nacp NAME "Melee-NX-NetPlay" AUTHOR "rkoh" VERSION "${MELEE_PORT_VERSION}")
nx_create_nro(melee_nx_netplay NACP melee-nx-netplay.nacp ICON "${PROJECT_SOURCE_DIR}/switch/icon.jpg" ROMFS "${PORT_ROMFS}")
