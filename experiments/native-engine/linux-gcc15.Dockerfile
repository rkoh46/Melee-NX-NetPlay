FROM gcc:15.2
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ca-certificates git cmake ninja-build python3 pkg-config libsdl2-dev libglew-dev libusb-1.0-0-dev libcurl4-openssl-dev libssl-dev zlib1g-dev patch time && rm -rf /var/lib/apt/lists/*
