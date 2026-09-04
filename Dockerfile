# syntax=docker/dockerfile:1
#
# Multi-stage build. This is NOT the recommended path for a weak/old ARM
# SBC (Raspberry Pi Zero/1, anything pre-Bookworm) — see docs/deployment.md
# and docs/sbc-deployment.md for that (native build or zig cross-compile to
# a fully static arm-linux-musleabihf binary, verified running on real
# armv7 Debian-9-stretch hardware, no container runtime required at all).
# This Dockerfile is for the "I want to try this on a normal x86_64/arm64
# machine right now" path: a desktop, a NAS, a cloud VM, or a Pi 4/5 on a
# 64-bit OS new enough to actually run a modern Docker Engine — Debian 9
# stretch's own glibc/kernel are too old for that in the first place.
#
# ---------------------------------------------------------------------------
# Stage 1: build. debian:bookworm-slim, not alpine — vcpkg's classic Linux
# triplet (x64-linux/arm64-linux) targets glibc; forcing musl here would
# mean fighting the exact same category of cross-toolchain friction
# docs/deployment.md's "Cross-compilation" section already describes for
# real musl targets, for zero benefit on a normal glibc host.
# ---------------------------------------------------------------------------
FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git curl zip unzip tar pkg-config \
    perl ca-certificates python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# vcpkg is bootstrapped fresh, not vendored (matches the rest of this repo
# — see .gitignore's own comment on why vcpkg/ is never committed). A
# FULL clone, not --depth 1 -- confirmed live that a shallow clone can
# miss vcpkg.json's pinned baseline commit entirely (manifest mode needs
# `git show <that commit>:versions/baseline.json`, which fails outright
# against a one-commit-deep history that commit doesn't happen to be
# — see docs/architecture.md's "Build" section for the full story). This
# broke a real build of this exact Dockerfile, not a hypothetical.
RUN git clone https://github.com/microsoft/vcpkg.git vcpkg \
    && ./vcpkg/bootstrap-vcpkg.sh -disableMetrics

# Only vcpkg.json first — a Docker layer-caching trick, not an accident.
# `vcpkg install` (manifest mode) reads vcpkg.json and populates
# vcpkg_installed/ WITHOUT needing CMakeLists.txt or any source file
# present at all. Building Drogon+OpenSSL+trantor from scratch is
# genuinely slow (docs/sbc-deployment.md: "expect this to take
# substantially longer than on a desktop" — and that's ON a desktop-class
# machine); copying the rest of the source AFTER this step means editing
# a .cpp file never invalidates this layer, only editing vcpkg.json does.
COPY vcpkg.json .
RUN ./vcpkg/vcpkg install --triplet x64-linux

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
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    && cmake --build build -j"$(nproc)"

# Same discipline as every other deployment path in this repo — don't
# trust a build that compiled clean, run the actual test suite
# (security_e2e.py included) before it goes anywhere near a runtime image.
RUN ctest --test-dir build --output-on-failure

RUN cmake --install build --prefix /opt/wiki

# ---------------------------------------------------------------------------
# Stage 2: runtime. vcpkg's Linux triplets are static by default, so the
# built binaries carry almost nothing dynamic beyond libc/libstdc++ (see
# CLAUDE.md's own ldd-verified note on this) — the runtime image needs
# barely more than glibc itself, not a second copy of the whole build
# toolchain.
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
