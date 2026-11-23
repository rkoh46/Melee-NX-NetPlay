# aarch64 Linux image for the off-console picture test: the native-engine GCC 15.2 image plus an X server
# without a screen (Xvfb), Mesa's software Vulkan driver (lavapipe) and what Dawn, SDL3 and Aurora need to build.
#   docker build -t mu-arm-gfx -f linux-gfx.Dockerfile .
FROM mu-arm-gcc15
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      xvfb xauth x11-apps imagemagick mesa-vulkan-drivers libvulkan1 libvulkan-dev vulkan-tools \
      libx11-dev libx11-xcb-dev libxcb1-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev \
      libxss-dev libxkbcommon-dev libxtst-dev libxinerama-dev libfreetype-dev libpng-dev libzstd-dev gdb \
    && rm -rf /var/lib/apt/lists/*
