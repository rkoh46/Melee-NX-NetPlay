# mu-native-gfx: the native-engine runner with a picture. Configured by build.sh in three variants:
#   null    aarch64 Linux, no graphics device: the GX decoder and the renderer's frame check only
#   linux   aarch64 Linux with Aurora on Dawn/Vulkan (the test build; Mesa's software driver under Xvfb)
#   switch  Horizon NRO with Aurora on Dawn/Vulkan (Mesa NVK), linked like the app (port/CMakeLists.txt)
# melee-unlocked's sources are compiled from build/native-engine-gfx/mu and never copied into the repository.
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)
project(mu_native_gfx LANGUAGES C CXX ASM)

set(MU_FE_VARIANT "null" CACHE STRING "null, linux or switch")
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
foreach(_needed "${MU}/port/linux/host_services.cpp" "${RUN}/runner_main.cpp" "${GEN}/embedded.S" "${GEN}/gx_core.cpp")
  if(NOT EXISTS "${_needed}")
    message(FATAL_ERROR "missing ${_needed}: run frontend/build.sh sync first")
  endif()
endforeach()

if(MU_FE_VARIANT STREQUAL "switch")
  # Dawn, SDL3, Aurora and the Mesa/NVK archives, exactly as the app gets them.
  include("${REPO}/switch/cmake/Graphics.cmake")
  set(CMAKE_CXX_STANDARD_LIBRARIES
    "-Wl,--start-group -lstdc++ -lsupc++ -lpthread -lsysbase -lc -lm -lgcc -lunwind -lnx ${MELEE_NVK_ARCHIVES} -ljpeg -lexpat -lz -Wl,--end-group ${MELEE_LIBGCC_DIR}/crtend.o ${MELEE_LIBGCC_DIR}/crtn.o"
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
  add_subdirectory("${REPO}/build/sdk/aurora" aurora EXCLUDE_FROM_ALL)
  target_include_directories(aurora_gx PRIVATE "${REPO}/switch/src")
  target_include_directories(aurora_core PRIVATE "${REPO}/switch/src")
  set(MELEE_AURORA_SOURCE "${REPO}/build/sdk/aurora")
elseif(NOT MU_FE_VARIANT STREQUAL "null")
  message(FATAL_ERROR "MU_FE_VARIANT must be null, linux or switch")
endif()

# ---- melee-unlocked's headless host services and the unchanged runner (as experiments/native-engine/build.sh builds them) ----
set(MU_HOST_SOURCES
  "${MU}/port/linux/host_services.cpp" "${MU}/port/linux/input_script.cpp"
  "${MU}/port/runtime/hle/ax_ucode.cpp" "${MU}/port/runtime/hle/native_savestate.cpp"
  "${RUN}/runner_main.cpp" "${RUN}/mu_elf.cpp" "${RUN}/rollback.cpp" "${RUN}/mu_imports.c"
  "${GEN}/embedded_tables.cpp" "${GEN}/embedded.S")
if(MU_FE_VARIANT STREQUAL "switch")
  list(APPEND MU_HOST_SOURCES "${RUN}/fixedmap_horizon.c" "${RUN}/dirtytrack_horizon.c")
  # Its own target: the toolchain names objects without the source extension, and dirtytrack_horizon.c sits beside it.
  add_library(mu_host_entry OBJECT "${RUN}/dirtytrack_horizon.S")
  list(APPEND MU_HOST_SOURCES $<TARGET_OBJECTS:mu_host_entry>)
else()
  list(APPEND MU_HOST_SOURCES "${RUN}/fixedmap_linux.c" "${RUN}/dirtytrack_linux.c")
endif()
add_library(mu_host STATIC ${MU_HOST_SOURCES})
set_target_properties(mu_host PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS ON)
target_include_directories(mu_host PRIVATE "${RUN}" "${MU}/port/linux" "${MU}/port/runtime/abi" "${MU}/port/runtime/hle"
  "${MU}/port/runtime/host" "${MU}/port/runtime/ppc" "${MU}/port/runtime/gx" "${MU}/native")
target_compile_definitions(mu_host PRIVATE MU_FOREIGN_ELF MU_SQUAD=1 MELEE_NO_SLIPPI MELEE_PORT_VERSION="mu-native" MELEE_LINUX_CPU_BASELINE="AArch64")
target_compile_options(mu_host PRIVATE -O2 -g -Wall -fno-strict-aliasing $<$<COMPILE_LANGUAGE:CXX>:-ffp-contract=on>)
# The runner's main becomes mu_runner_main (frontend_main.cpp calls it).
set_source_files_properties("${RUN}/runner_main.cpp" PROPERTIES COMPILE_DEFINITIONS "main=mu_runner_main")
# embedded.S names its .incbin files relative to the repository root.
set_source_files_properties("${GEN}/embedded.S" PROPERTIES COMPILE_OPTIONS "-I${REPO}")
if(NOT MU_FE_VARIANT STREQUAL "switch")
  target_compile_definitions(mu_host PRIVATE MU_CARD_SIMPLE_COMMIT)
endif()

# ---- this repository's GX decoder and renderer bridge, on a small host shim ----
add_library(mu_gx STATIC
  "${GEN}/gx_core.cpp" "${GXSRC}/gx_texture.cpp" "${GXSRC}/gx_aurora_bridge.cpp" "${GXSRC}/async_backend.cpp"
  "${FE}/shim/shim.cpp" "${FE}/frontend.cpp")
# The shim's host.h and ppc.h come first; the real render_observer.h and fatal_boundary.h are found after them.
target_include_directories(mu_gx PRIVATE "${FE}/shim" "${FE}" "${GXSRC}" "${REPO}/port/runtime/ppc" "${REPO}/port/runtime/host"
  "${MU}/port/linux" "${MU}/port/runtime/abi")
target_compile_options(mu_gx PRIVATE -O2 -g -Wall -fno-strict-aliasing)
if(MU_FE_VARIANT STREQUAL "null")
  target_compile_definitions(mu_gx PRIVATE MU_FE_AURORA=0)
else()
  target_sources(mu_gx PRIVATE "${GXSRC}/gx_aurora.cpp")
  target_compile_definitions(mu_gx PRIVATE MU_FE_AURORA=1)
  target_include_directories(mu_gx PRIVATE "${MELEE_AURORA_SOURCE}/lib")   # gx_aurora.cpp writes Aurora's FIFO directly
  target_link_libraries(mu_gx PUBLIC aurora::core aurora::gx aurora::vi aurora::os aurora::mtx aurora::pad
    absl::flat_hash_map absl::btree SDL3::SDL3-static)
endif()

add_executable(mu_native_gfx "${FE}/frontend_main.cpp")
target_include_directories(mu_native_gfx PRIVATE "${FE}")
target_link_libraries(mu_native_gfx PRIVATE mu_gx mu_host mu_gx)

if(MU_FE_VARIANT STREQUAL "switch")
  target_include_directories(mu_gx SYSTEM PRIVATE "${DEVKITPRO}/portlibs/switch/include" "${DEVKITPRO}/libnx/include")
  target_include_directories(mu_host SYSTEM PRIVATE "${DEVKITPRO}/portlibs/switch/include" "${DEVKITPRO}/libnx/include")
  # What Aurora's patched objects and the Clang/libnx runtime need from the app's Switch layer.
  target_sources(mu_native_gfx PRIVATE "${REPO}/switch/src/switch_threads.c" "${REPO}/switch/src/perf_switch.cpp"
    "${REPO}/switch/src/libc_switch.c" "${FE}/switch_glue.cpp")
  target_include_directories(mu_native_gfx PRIVATE "${REPO}/switch/src")
  if(MELEE_NVK_STUBS_SOURCE)
    target_sources(mu_native_gfx PRIVATE "${MELEE_NVK_STUBS_SOURCE}")   # Rust/newlib ABI bridge for the NVK archives
  endif()
  target_link_options(mu_native_gfx PRIVATE
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
  set_target_properties(mu_native_gfx PROPERTIES OUTPUT_NAME mu-native-gfx)
  nx_generate_nacp(mu-native-gfx.nacp NAME "mu-native-gfx" AUTHOR "Melee-NX-NetPlay" VERSION "0.1")
  nx_create_nro(mu_native_gfx NACP mu-native-gfx.nacp ICON "${REPO}/switch/icon.jpg")
else()
  set_target_properties(mu_native_gfx PROPERTIES OUTPUT_NAME mu-native-gfx-${MU_FE_VARIANT})
  # A position-independent executable keeps the host away from the fixed game addresses.
  target_link_options(mu_native_gfx PRIVATE -pie)
  target_link_libraries(mu_native_gfx PRIVATE pthread z m)
endif()
