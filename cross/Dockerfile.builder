# syntax=docker/dockerfile:1
#
# Containerized cross-toolchain: produces the exact same static
# arm-linux-musleabihf binaries as docs/sbc-deployment.md's "Path B" /
# `wiki-ops.sh build cross`, without needing zig or vcpkg installed on the
# host — only Docker. This is a build-only image; its output is extracted
# via `docker create`+`docker cp` (see `wiki-ops.sh build cross-container`),
# never run as a container itself. Deliberately NOT using a buildx
# `--output` target, so this stays usable with plain `docker build` too,
# independent of the buildx/QEMU requirement the container-arm variant
# (the main Dockerfile, built with --platform) actually needs.
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git curl xz-utils zip unzip tar \
    pkg-config ca-certificates python3 \
    && rm -rf /var/lib/apt/lists/*

# zig bundles its own libc/libc++ for the target — no separate ARM
# cross-toolchain package needed beyond this one tarball.
ARG ZIG_VERSION=0.16.0
RUN curl -sSfL "https://ziglang.org/download/${ZIG_VERSION}/zig-linux-x86_64-${ZIG_VERSION}.tar.xz" \
      -o /tmp/zig.tar.xz \
    && tar -C /usr/local -xJf /tmp/zig.tar.xz \
    && ln -s "/usr/local/zig-linux-x86_64-${ZIG_VERSION}/zig" /usr/local/bin/zig \
    && rm /tmp/zig.tar.xz

WORKDIR /src

# vcpkg bootstrapped fresh inside the image, not vendored — same
# reasoning as the main Dockerfile and .gitignore's own comment on vcpkg/.
# FULL clone, not --depth 1 (see docs/architecture.md's "Build" section —
# manifest mode needs the exact pinned builtin-baseline commit reachable).
RUN git clone https://github.com/microsoft/vcpkg.git vcpkg \
    && ./vcpkg/bootstrap-vcpkg.sh -disableMetrics

COPY vcpkg.json .
# Native x64-linux drogon_ctl, built first: classic-mode cross builds
# reuse this host tool instead of cross-building the `ctl` code-generator
# (drogon[ctl] isn't supported cross — see docs/sbc-deployment.md's Path B).
RUN ./vcpkg/vcpkg install --triplet x64-linux

COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY src ./src
COPY tests ./tests
COPY cross ./cross

RUN cd vcpkg && ./vcpkg install --classic --triplet arm-musl \
      --overlay-triplets=../cross/arm-musl --overlay-ports=../cross/overlay-ports \
      --x-install-root=../vcpkg_installed_arm \
      drogon sqlite3[core,fts5,json1] libargon2 nlohmann-json md4c yaml-cpp \
      tomlplusplus catch2

ENV PKG_CONFIG_LIBDIR=/src/vcpkg_installed_arm/arm-musl/lib/pkgconfig:/src/vcpkg_installed_arm/arm-musl/share/pkgconfig
ENV PKG_CONFIG_SYSROOT_DIR=""

RUN cmake -S . -B build-arm -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cross/arm-musl/toolchain.cmake \
      -DCMAKE_PREFIX_PATH=/src/vcpkg_installed_arm/arm-musl \
      -DCMAKE_FIND_ROOT_PATH=/src/vcpkg_installed_arm/arm-musl \
      -DDROGON_CTL_COMMAND=/src/vcpkg_installed/x64-linux/tools/drogon/drogon_ctl \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    && cmake --build build-arm -j"$(nproc)"

RUN mkdir -p /out && cp build-arm/wiki-server build-arm/wiki-mcp build-arm/tests/unit_tests /out/
