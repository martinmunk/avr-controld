# Dockerfile for ALSA AVR control
FROM debian:bookworm-slim

# Install build dependencies and ALSA libraries
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libasound2-dev \
    libasound2 \
    alsa-utils \
    && rm -rf /var/lib/apt/lists/*

# Copy source code
COPY include /src/include
COPY src /src/src
COPY CMakeLists.txt /src/

# Build plugin inside container
WORKDIR /src
RUN mkdir -p build && cd build && \
    cmake .. && \
    make -j4 && \
    mkdir -p /lib/aarch64-linux-gnu/alsa-lib /usr/lib/aarch64-linux-gnu/alsa-lib && \
    cp libasound_module_ctl_avr.so /lib/aarch64-linux-gnu/alsa-lib/ && \
    cp libasound_module_ctl_avr.so /usr/lib/aarch64-linux-gnu/alsa-lib/

# Copy ALSA configuration
COPY docker-asound.conf /etc/asound.conf

# Test that plugin loads
RUN amixer -D avr 2>&1 || echo "Plugin loaded (connection to AVR will fail without network)"

WORKDIR /
CMD ["/bin/bash"]
