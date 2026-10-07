# syntax=docker/dockerfile:1
#
# Multi-stage build. This is NOT the recommended path for a weak/old ARM
# SBC (Raspberry Pi Zero/1, anything pre-Bookworm) — see
# docs/sbc-deployment.md for that (native build or zig cross-compile to a
# fully static arm-linux-musleabihf binary, verified running on real
# armv7 Debian-9-stretch hardware, no container runtime required at all;
# cross/Dockerfile.builder containerizes that same zig toolchain, a
# completely different artifact shape from this file). This Dockerfile is
# for the "I want to try this on a normal machine right now" path: a
# desktop, a NAS, a cloud VM, or a Pi 4/5 on a 64-bit OS new enough to
# actually run a modern Docker Engine — Debian 9 stretch's own
# glibc/kernel are too old for that in the first place.
#
# One Dockerfile for every architecture Docker/buildx can target (amd64,
# arm64, armv7 so far — see the TARGETARCH case below), not a separate
# file per arch: `docker build .` (no --platform, legacy builder included
# — TARGETARCH defaults to amd64 below for exactly that case) behaves
# identically to before; `docker buildx build --platform linux/arm/v7 .`
# cross-builds under QEMU emulation, buildx substituting TARGETARCH/
# TARGETVARIANT automatically. debian:bookworm-slim is itself a
# multi-arch manifest list — buildx resolves the right per-platform base
# image layer with no change needed here, same as any other FROM line.
#
# ---------------------------------------------------------------------------
# Stage 1: build. debian:bookworm-slim, not alpine — vcpkg's classic Linux
# triplets (x64-linux/arm64-linux/arm-linux) target glibc; forcing musl
# here would mean fighting the exact same category of cross-toolchain
# friction docs/sbc-deployment.md's "Cross-compilation" section already
# describes for real musl targets, for zero benefit on a normal glibc host.
# ---------------------------------------------------------------------------
FROM debian:bookworm-slim AS build

# Defaulted here (not left to buildx alone) so a plain `docker build .`
# with no buildx component at all — the legacy builder, still the
# default on some Docker installs — keeps working unchanged: TARGETARCH
# is only auto-populated by BuildKit/buildx, never by the legacy builder.
ARG TARGETARCH=amd64
ARG TARGETVARIANT=
# Extra -D... CMake cache flags, e.g. "-DWIKI_ENABLE_CLOUD_EMBEDDINGS=ON" — passed
# through from `docker build --build-arg WIKI_CMAKE_EXTRA_ARGS=...` (see
# tools/wiki-ops.sh's --local-embeddings/--cloud-embeddings build flags). Empty by
# default, word-splits to nothing in the RUN line below.
ARG WIKI_CMAKE_EXTRA_ARGS=

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git curl zip unzip tar pkg-config \
    perl ca-certificates python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Confirmed live under --platform linux/arm/v7: without this, vcpkg
# tries to download its own pre-built x86_64 helper tools (cmake, etc.)
# and, finding none for arm, falls back to compiling its OWN vcpkg-tool
# FROM SOURCE under QEMU emulation — over an hour of wasted compute
# before failing anyway with "Environment variable
# VCPKG_FORCE_SYSTEM_BINARIES must be set on arm...". Setting it
# unconditionally (not just when TARGETARCH is arm) is harmless on
# amd64/arm64 too: the apt-get above already installs cmake/ninja, so
# vcpkg has real system binaries to use there either way.
ENV VCPKG_FORCE_SYSTEM_BINARIES=1

# vcpkg is bootstrapped fresh, not vendored (matches the rest of this repo
# — see .gitignore's own comment on why vcpkg/ is never committed). A
# FULL clone, not --depth 1: a shallow clone can miss vcpkg.json's
# pinned baseline commit entirely (manifest mode needs
# `git show <that commit>:versions/baseline.json`, which fails outright
# against a one-commit-deep history that commit doesn't happen to be
# — see docs/architecture.md's "Build" section for the full story).
RUN git clone https://github.com/microsoft/vcpkg.git vcpkg \
    && ./vcpkg/bootstrap-vcpkg.sh -disableMetrics

# The one piece that actually varies per architecture: buildx's own
# TARGETARCH/TARGETVARIANT naming doesn't match vcpkg's triplet naming
# (e.g. "arm"+"v7" here is "arm-linux" to vcpkg). Fails loudly on an
# unmapped combination rather than silently picking the wrong triplet —
# ppc64le/s390x/riscv64 etc. aren't mapped because nothing in this repo
# targets them; add a case here, not a guess, if that ever changes.
RUN set -eu; \
    case "${TARGETARCH}:${TARGETVARIANT}" in \
      amd64:*) triplet=x64-linux ;; \
      arm64:*) triplet=arm64-linux ;; \
      arm:v7)  triplet=arm-linux ;; \
      *) echo "Dockerfile: no vcpkg triplet mapping for TARGETARCH=${TARGETARCH} TARGETVARIANT=${TARGETVARIANT}" >&2; exit 1 ;; \
    esac; \
    echo "$triplet" > /tmp/vcpkg-triplet

# Only vcpkg.json first — a Docker layer-caching trick, not an accident.
# `vcpkg install` (manifest mode) reads vcpkg.json and populates
# vcpkg_installed/ WITHOUT needing CMakeLists.txt or any source file
# present at all. Building Drogon+OpenSSL+trantor from scratch is
# genuinely slow (docs/sbc-deployment.md: "expect this to take
# substantially longer than on a desktop" — and that's ON a desktop-class
# machine); copying the rest of the source AFTER this step means editing
# a .cpp file never invalidates this layer, only editing vcpkg.json does.
COPY vcpkg.json .
RUN ./vcpkg/vcpkg install --triplet "$(cat /tmp/vcpkg-triplet)"

# Now the actual source. systemd/ is needed even though this image never
# runs under systemd -- CMakeLists.txt's own install() rules copy the
# unit files unconditionally (cmake --install below fails without it);
# they just never make it into the runtime stage's final COPY --from
# list further down, so nothing systemd-specific ends up in the image.
COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY src ./src
COPY tests ./tests
COPY static ./static
COPY systemd ./systemd
COPY config.example.toml ./

RUN cmake -S . -B build -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake \
      -DVCPKG_TARGET_TRIPLET="$(cat /tmp/vcpkg-triplet)" \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      ${WIKI_CMAKE_EXTRA_ARGS} \
    && cmake --build build -j"$(nproc)"

# Same discipline as every other deployment path in this repo — don't
# trust a build that compiled clean, run the actual test suite
# (security_e2e.py included) before it goes anywhere near a runtime
# image. Under QEMU emulation (cross-arch builds) this is slow but still
# cheap relative to the compile step above.
RUN ctest --test-dir build --output-on-failure

RUN cmake --install build --prefix /opt/wiki

# ---------------------------------------------------------------------------
# Stage 2: runtime. vcpkg's Linux triplets are static by default, so the
# built binaries carry almost nothing dynamic beyond libc/libstdc++ (see
# CLAUDE.md's own ldd-verified note on this) — the runtime image needs
# barely more than glibc itself, not a second copy of the whole build
# toolchain. No TARGETARCH handling needed here: everything below is
# architecture-agnostic (apt packages, the COPY'd artifacts, config).
# ---------------------------------------------------------------------------
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    # Fixed uid/gid 1000, not whatever `useradd --system` would pick on its
    # own (unpredictable across base-image versions) -- /data is meant to
    # be a HOST bind mount (docker-compose.yml), and a bind mount's write
    # permission is checked by raw uid, not by username, which doesn't
    # cross the mount boundary at all. 1000 matches the default first-user
    # uid on essentially every mainstream Linux distro, so `mkdir -p
    # vault_data` on the host "just works" in the common case; documented
    # in docker-compose.yml/README for the case where it doesn't.
    && groupadd --gid 1000 wiki \
    && useradd --system --uid 1000 --gid 1000 --create-home --home-dir /opt/wiki \
       --shell /usr/sbin/nologin wiki

WORKDIR /opt/wiki
COPY --from=build --chown=wiki:wiki /opt/wiki/bin ./bin
COPY --from=build --chown=wiki:wiki /opt/wiki/static ./static
COPY --from=build --chown=wiki:wiki /opt/wiki/config.example.toml ./
COPY --chown=wiki:wiki docker/config.docker.toml ./config.toml

# /data is the ONE thing meant to be volume-mounted (docker-compose.yml
# does this by default) — config.docker.toml points [vault].path/
# [index].db_path here. Mounting it keeps the vault's markdown files
# reachable directly on the host filesystem, unchanged from every other
# deployment path in this repo: the files on disk are the source of
# truth, never something meant to live only inside a container volume
# nobody outside Docker can browse or `git`/rsync/edit directly.
RUN mkdir -p /data/vault && chown -R wiki:wiki /data

VOLUME ["/data"]
EXPOSE 8080
USER wiki

HEALTHCHECK --interval=30s --timeout=3s --start-period=10s \
  CMD curl -f http://127.0.0.1:8080/healthz || exit 1

ENTRYPOINT ["/opt/wiki/bin/wiki-server"]
