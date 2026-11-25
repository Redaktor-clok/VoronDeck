# SPDX-License-Identifier: GPL-3.0-or-later
FROM debian:bookworm-slim@sha256:e67eeead736ff7bbe258ef088bff683635c561b1fcfefad0806c1df4ab49ce0e
ENV DEBIAN_FRONTEND=noninteractive
RUN echo 'Acquire::Retries "3";' > /etc/apt/apt.conf.d/80-retries && \
    dpkg --add-architecture arm64 && apt-get update && \
    apt-get install -y --no-install-recommends \
      build-essential crossbuild-essential-arm64 git pkg-config ca-certificates \
      make cmake python3 file autoconf automake libtool flex bison ccache \
      linux-libc-dev:amd64 linux-libc-dev:arm64 libc6-dev:arm64 libstdc++-12-dev:arm64 \
      libdrm-dev:arm64 libinput-dev:arm64 libudev-dev:arm64 libssl-dev:arm64 \
      libsystemd-dev:arm64 zlib1g-dev:arm64 libgbm-dev:arm64 libgles2-mesa-dev:arm64 \
      libegl1-mesa-dev:arm64 libusb-1.0-0-dev:arm64 libbluetooth-dev:arm64 libasound2-dev:arm64 && \
    rm -rf /var/lib/apt/lists/*
ENV CROSS_COMPILE=aarch64-linux-gnu- CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ \
    AR=aarch64-linux-gnu-gcc-ar STRIP=aarch64-linux-gnu-strip RANLIB=aarch64-linux-gnu-gcc-ranlib \
    LD=aarch64-linux-gnu-ld PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig PKG_CONFIG_SYSROOT_DIR=/
WORKDIR /src
RUN aarch64-linux-gnu-gcc --version && aarch64-linux-gnu-g++ --version
