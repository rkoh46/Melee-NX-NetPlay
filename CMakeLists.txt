# Melee-NX-NetPlay: Dashdance's recompiled Melee runtime and Slippi netplay on
# Nintendo Switch homebrew. Configure through builder/ (Docker, devkitA64 GCC for
# C, Clang for C++) with -DCMAKE_TOOLCHAIN_FILE=switch/cmake/SwitchGCC.cmake.
# See BUILDING.md; the generated game must exist first (tools/bootstrap_port.py).
cmake_minimum_required(VERSION 3.25)
project(MeleeNXNetPlay LANGUAGES C CXX ASM)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "Use switch/cmake/SwitchGCC.cmake: devkitA64 GCC compiles C, Clang compiles C++")
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_package(Threads REQUIRED)

set(MELEE_NX_FRONTEND "headless" CACHE STRING "headless (console log only, measures the simulation) or aurora (Dawn/NVK renderer)")
set_property(CACHE MELEE_NX_FRONTEND PROPERTY STRINGS headless aurora)

# ---- Inputs the player supplies or the fetcher pins (never committed) ----
set(MELEE_DECOMP_ROOT "${PROJECT_SOURCE_DIR}/ref/melee" CACHE PATH "Pinned doldecomp/melee checkout (build input)")
set(MELEE_PORT_GENERATED_DIR "${PROJECT_SOURCE_DIR}/build/generated/guest" CACHE PATH "Generated game C++ (tools/bootstrap_port.py)")
set(MELEE_PORT_INPUT_MANIFEST "${PROJECT_SOURCE_DIR}/build/port-inputs.json" CACHE FILEPATH "Provenance manifest written by the bootstrap")
foreach(fobj_input fobj.c fobj.h spline.c)
  if(NOT EXISTS "${MELEE_DECOMP_ROOT}/src/sysdolphin/baselib/${fobj_input}")
    message(FATAL_ERROR "Missing ${MELEE_DECOMP_ROOT}/src/sysdolphin/baselib/${fobj_input}; run builder/fetch-deps.sh")
  endif()
endforeach()
if(NOT EXISTS "${MELEE_PORT_GENERATED_DIR}/guest_sources.cmake")
  message(FATAL_ERROR "Generated game missing at ${MELEE_PORT_GENERATED_DIR}; run tools/bootstrap_port.py --stage generate (see BUILDING.md)")
endif()
if(NOT EXISTS "${MELEE_PORT_INPUT_MANIFEST}")
  message(FATAL_ERROR "Missing ${MELEE_PORT_INPUT_MANIFEST}; run tools/bootstrap_port.py --stage generate")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MELEE_PORT_INPUT_MANIFEST}")
file(SHA256 "${MELEE_PORT_INPUT_MANIFEST}" MELEE_PORT_INPUT_MANIFEST_SHA256)
file(STRINGS "${PROJECT_SOURCE_DIR}/VERSION" MELEE_PORT_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/VERSION")   # a version bump reconfigures

# ---- Runtime link group (after project(); the devkitPro platform file overwrites an earlier value) ----
# Everything cyclic between the C/C++ runtime, libnx and the portlibs resolves in one rescan group;
# crtend/crtn close the sections crti/crtbegin open (see switch/cmake/SwitchGCC.cmake).
set(MELEE_NX_PORTLIBS "-lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz")
if(MELEE_NX_FRONTEND STREQUAL "aurora")
  include("${PROJECT_SOURCE_DIR}/switch/cmake/Graphics.cmake")
  string(APPEND MELEE_NX_PORTLIBS " ${MELEE_NVK_ARCHIVES} -ljpeg -lexpat")
endif()
# libnx precedes libc so its socket-aware fcntl (O_NONBLOCK on sockets, which ENet
# and curl need) is the one pulled in, not newlib's syscall stub.
set(CMAKE_CXX_STANDARD_LIBRARIES
  "-Wl,--start-group -lstdc++ -lsupc++ -lpthread -lnx -lsysbase -lc -lm -lgcc -lunwind ${MELEE_NX_PORTLIBS} -Wl,--end-group ${MELEE_LIBGCC_DIR}/crtend.o ${MELEE_LIBGCC_DIR}/crtn.o"
  CACHE STRING "" FORCE)

# ---- Shared compile options: exact GameCube float semantics in the recompiled code ----
add_library(port_build_options INTERFACE)
target_compile_features(port_build_options INTERFACE cxx_std_17)
# -fPIE after the toolchain's -fPIC: the NRO is one position-independent executable, so globals
# (guest RAM base, dispatch table, CPU context) can be addressed PC-relative instead of through
# the GOT on every access.
target_compile_options(port_build_options INTERFACE -fPIE
  $<$<COMPILE_LANGUAGE:CXX>:-fno-fast-math -ffp-model=strict -ffp-contract=off -fexceptions>)
target_compile_definitions(port_build_options INTERFACE __SWITCH__=1 MELEE_NX=1)

# ---- Host animation interpreter generated from the decomp ----
set(HOST_FOBJ "${PROJECT_BINARY_DIR}/generated/FObjHost.cpp")
add_custom_command(OUTPUT "${HOST_FOBJ}"
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/generate_fobj_host.py" "${HOST_FOBJ}" --decomp-root "${MELEE_DECOMP_ROOT}"
  DEPENDS tools/generate_fobj_host.py tools/port_source_pins.json
          "${MELEE_DECOMP_ROOT}/src/sysdolphin/baselib/fobj.c" "${MELEE_DECOMP_ROOT}/src/sysdolphin/baselib/fobj.h"
          "${MELEE_DECOMP_ROOT}/src/sysdolphin/baselib/spline.c"
  VERBATIM)
add_library(native_animation STATIC "${HOST_FOBJ}")
target_include_directories(native_animation PUBLIC native)
target_compile_features(native_animation PUBLIC cxx_std_14)
target_link_libraries(native_animation PUBLIC port_build_options)

add_subdirectory(port)
