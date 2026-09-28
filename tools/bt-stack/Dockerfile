# Build environment for compas-player's R1 Bluetooth base-image upgrade
# (BlueALSA 5, BlueZ 5.87 and their patches) -- their scripts, unmodified,
# run against pinned sources. amd64 because the Ingenic glibc toolchain
# (tobunto/ingenic-toolchain-v5.2) is x86-64 Linux only; trixie because
# GLib 2.84 needs a newer meson than bullseye/bookworm carry.
FROM --platform=linux/amd64 debian:trixie
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential autoconf automake libtool gettext autopoint \
        cmake meson ninja-build pkg-config git curl ca-certificates \
        xz-utils bzip2 file ripgrep qemu-user python3 python3-docutils patch \
        squashfs-tools libglib2.0-dev-bin bison flex \
    && rm -rf /var/lib/apt/lists/*
RUN git config --system --add safe.directory "*"
WORKDIR /build
