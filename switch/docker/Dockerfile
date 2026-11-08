# Reusable Switch toolchain: devkitA64 GCC for C, Clang for the C++ runtime, Dawn
# and the recompiled game. From Melee-NX (switch/docker/Dockerfile), plus the
# portlibs the Slippi services need (curl over mbedtls, zlib).

FROM devkitpro/devkita64:latest

ARG LLVM_VERSION=19
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl gnupg lsb-release software-properties-common \
        ninja-build cmake git python3 python3-pip pkg-config \
    && curl -fsSL https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh \
    && chmod +x /tmp/llvm.sh \
    && /tmp/llvm.sh ${LLVM_VERSION} \
    && ln -sf /usr/bin/clang-${LLVM_VERSION}   /usr/local/bin/clang \
    && ln -sf /usr/bin/clang++-${LLVM_VERSION} /usr/local/bin/clang++ \
    && rm -rf /var/lib/apt/lists/* /tmp/llvm.sh

# Slippi login, matchmaking reports and the dashboard use libcurl; Dawn supplies
# its own Python generator dependencies in its source tree.
RUN dkp-pacman -S --noconfirm --needed switch-curl switch-mbedtls switch-zlib switch-libjpeg-turbo switch-libexpat \
    && dkp-pacman -Scc --noconfirm

ENV DEVKITPRO=/opt/devkitpro
ENV PATH="/usr/local/bin:${PATH}"
