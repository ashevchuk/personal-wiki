#!/usr/bin/env bash
# Unified build+deploy orchestration, replacing the manual copy-paste flow
# documented in docs/sbc-deployment.md. Every subcommand here is a thin
# wrapper around an exact command sequence from that doc (or, for
# cross-container, new infrastructure under cross/; container-arm reuses
# the main Dockerfile, parameterized by TARGETARCH/TARGETVARIANT) — this
# script does not reimplement build/deploy logic
# that already exists, it just enforces the order and removes the
# hand-typing.
#
# Flags always win; anything a flag doesn't supply falls back to
# deploy.local.env (see deploy.local.env.example), then an interactive
# prompt (with a shown default) when a real terminal is attached, then a
# hard default, then an error if none of those resolve it.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$SCRIPT_DIR"

DEPLOY_CONFIG="$SCRIPT_DIR/deploy.local.env"
DRY_RUN=0
ASSUME_YES=0

# ============================================================
# SECTION: logging / dry-run / confirmation
# ============================================================

log() { echo "wiki-ops: $*" >&2; }
die() { echo "wiki-ops: error: $*" >&2; exit 1; }

# Wraps every state-changing external command. Prints what it's about to
# run either way, so --dry-run output doubles as a readable transcript of
# what a real run would have done.
run() {
  printf 'wiki-ops: + %s\n' "$*" >&2
  if [ "$DRY_RUN" = 1 ]; then
    return 0
  fi
  "$@"
}

# confirm_or_die PROMPT — used before anything that restarts/overwrites a
# already-live target. --yes skips the prompt; no TTY and no --yes refuses
# outright rather than silently guessing either way.
confirm_or_die() {
  local prompt=$1
  if [ "$ASSUME_YES" = 1 ] || [ "$DRY_RUN" = 1 ]; then
    return 0
  fi
  if [ -t 0 ]; then
    local ans
    read -r -p "$prompt [y/N] " ans
    case "$ans" in
      [yY]|[yY][eE][sS]) return 0 ;;
      *) die "aborted by user" ;;
    esac
  else
    die "refusing to proceed non-interactively without --yes: $prompt"
  fi
}

# ============================================================
# SECTION: flag / config-file / prompt resolver
# ============================================================

# resolve OUTVAR FLAGVAL CFGVAL DEFAULT PROMPT
# Priority: flag > config file value > interactive prompt (default shown)
# > hard default > error (no TTY, nothing else resolved it).
resolve() {
  local __outvar=$1 flagval=$2 cfgval=$3 default=$4 prompt=$5
  local value
  if [ -n "$flagval" ]; then
    value=$flagval
  elif [ -n "$cfgval" ]; then
    value=$cfgval
  elif [ -t 0 ] && [ -t 1 ]; then
    read -r -p "$prompt [$default]: " value
    value=${value:-$default}
  elif [ -n "$default" ]; then
    value=$default
  else
    die "no value for $__outvar (no flag, no deploy.local.env entry, no default, and no TTY to ask)"
  fi
  printf -v "$__outvar" '%s' "$value"
}

# resolve_optional OUTVAR FLAGVAL CFGVAL DEFAULT PROMPT — same priority
# order as resolve(), but an empty DEFAULT is a legitimate final answer
# (e.g. WIKI_DEPLOY_HOST empty means "local install") rather than "no
# default was given", so this never errors out non-interactively.
resolve_optional() {
  local __outvar=$1 flagval=$2 cfgval=$3 default=$4 prompt=$5
  local value
  if [ -n "$flagval" ]; then
    value=$flagval
  elif [ -n "$cfgval" ]; then
    value=$cfgval
  elif [ -t 0 ] && [ -t 1 ]; then
    read -r -p "$prompt [$default]: " value
    value=${value:-$default}
  else
    value=$default
  fi
  printf -v "$__outvar" '%s' "$value"
}

# resolve_secret OUTVAR FLAGVAL PROMPT — never shows a default, never
# echoes input. Used only for values that must reach a target's wiki.env,
# never stored in deploy.local.env itself.
resolve_secret() {
  local __outvar=$1 flagval=$2 prompt=$3
  local value
  if [ -n "$flagval" ]; then
    value=$flagval
  elif [ -t 0 ]; then
    read -rs -p "$prompt: " value
    echo >&2
  else
    die "no value for $__outvar and no TTY to prompt for one"
  fi
  printf -v "$__outvar" '%s' "$value"
}

load_deploy_config() {
  : "${WIKI_DEPLOY_HOST:=}"
  : "${WIKI_DEPLOY_SSH_PORT:=22}"
  : "${WIKI_DEPLOY_INSTALL_ROOT:=/opt/wiki}"
  : "${WIKI_DEPLOY_VARIANT:=}"
  : "${WIKI_DEPLOY_SERVICE_USER:=wiki}"
  : "${WIKI_DEPLOY_DOMAIN:=}"
  : "${WIKI_DEPLOY_BASE_PATH:=}"
  : "${WIKI_DEPLOY_BACKUP_DIR:=}"
  : "${WIKI_DEPLOY_WITH_BACKUP_TIMER:=false}"
  : "${WIKI_DEPLOY_PORT:=}"
  : "${WIKI_DEPLOY_TRIPLET:=}"
  : "${WIKI_DEPLOY_EMBEDDINGS_API_KEY_ENV:=}"
  : "${WIKI_DEPLOY_LLM_API_KEY_ENV:=}"
  : "${WIKI_DEPLOY_TLS_ENABLED:=false}"
  : "${WIKI_DEPLOY_TLS_CERT_FILE:=}"
  : "${WIKI_DEPLOY_TLS_KEY_FILE:=}"
  : "${WIKI_DEPLOY_LISTEN_ADDR:=}"
  if [ -f "$DEPLOY_CONFIG" ]; then
    set -a
    # shellcheck disable=SC1090
    . "$DEPLOY_CONFIG"
    set +a
  fi
}

# ============================================================
# SECTION: local-vs-remote execution helpers
# ============================================================

# remote_sh SCRIPT — runs SCRIPT either over ssh (WIKI_DEPLOY_HOST set) or
# in a local bash -c, identical code path either way (see the plan: the
# atomic-swap logic must not fork into two parallel implementations).
remote_sh() {
  local script=$1
  printf 'wiki-ops: + (%s) %s\n' "${WIKI_DEPLOY_HOST:-local}" "$script" >&2
  if [ "$DRY_RUN" = 1 ]; then
    return 0
  fi
  if [ -n "${WIKI_DEPLOY_HOST:-}" ]; then
    ssh -p "$WIKI_DEPLOY_SSH_PORT" "$WIKI_DEPLOY_HOST" "set -euo pipefail; $script"
  else
    bash -c "set -euo pipefail; $script"
  fi
}

# remote_sh_secret SCRIPT SECRET — same as remote_sh, but pipes SECRET
# into the remote command's stdin instead of embedding it in SCRIPT.
# remote_sh's own trace line prints its whole SCRIPT argument unconditionally
# (even under --dry-run) — a secret VALUE must never be part of that
# string, or it leaks into this script's own stderr/logs every single
# run. SCRIPT is expected to do its own `read -r SOMEVAR` to consume it;
# nothing here parses or re-echoes the value itself.
remote_sh_secret() {
  local script=$1 secret=$2
  printf 'wiki-ops: + (%s) %s <value piped via stdin, not shown>\n' "${WIKI_DEPLOY_HOST:-local}" "$script" >&2
  if [ "$DRY_RUN" = 1 ]; then
    return 0
  fi
  if [ -n "${WIKI_DEPLOY_HOST:-}" ]; then
    ssh -p "$WIKI_DEPLOY_SSH_PORT" "$WIKI_DEPLOY_HOST" "set -euo pipefail; $script" <<< "$secret"
  else
    bash -c "set -euo pipefail; $script" <<< "$secret"
  fi
}

# Same as remote_sh but always actually runs (read-only queries — status
# checks, `is-active` probes — that --dry-run must still answer truthfully).
remote_sh_capture() {
  local script=$1
  if [ -n "${WIKI_DEPLOY_HOST:-}" ]; then
    ssh -p "$WIKI_DEPLOY_SSH_PORT" "$WIKI_DEPLOY_HOST" "$script"
  else
    bash -c "$script"
  fi
}

# copy_to_target LOCAL_PATH REMOTE_PATH
copy_to_target() {
  local local_path=$1 remote_path=$2
  if [ -n "${WIKI_DEPLOY_HOST:-}" ]; then
    run scp -P "$WIKI_DEPLOY_SSH_PORT" -r "$local_path" "$WIKI_DEPLOY_HOST:$remote_path"
    return
  fi
  # Local target: some callers build LOCAL_PATH and REMOTE_PATH from the
  # same mktemp name (e.g. static-redeploy's tarball, both staged under
  # /tmp) — `cp` a file onto itself errors "are the same file", so skip
  # the copy when they already resolve to the same path.
  if [ "$(readlink -f -- "$local_path" 2>/dev/null || printf '%s' "$local_path")" \
     = "$(readlink -f -- "$remote_path" 2>/dev/null || printf '%s' "$remote_path")" ]; then
    return
  fi
  run mkdir -p "$(dirname "$remote_path")"
  run cp -r "$local_path" "$remote_path"
}

# ============================================================
# SECTION: build — native / cross / container / cross-container /
# container-arm / container-native
# ============================================================

# platform_tag_suffix PLATFORM — "linux/arm/v7" -> "arm-v7", "linux/arm64"
# -> "arm64", "linux/amd64" -> "amd64". Used to tag the image so testing
# a non-default --platform never collides with the "arm" tag
# docker-compose.arm.yml/cmd_deploy_container expect for the real,
# documented armv7-on-a-Pi target.
platform_tag_suffix() {
  printf '%s' "$1" | sed -e 's#^linux/##' -e 's#/#-#'
}

# platform_build_args PLATFORM — echoes --build-arg flags for TARGETARCH
# (and TARGETVARIANT, if the platform string has one) derived from
# "linux/arm/v7" style strings. Confirmed live: buildx does NOT reliably
# auto-populate these from --platform alone in every builder/version
# combination — a real `build container-arm` run with
# --platform=linux/arm/v7 silently built as TARGETARCH=amd64 (the
# Dockerfile's own fallback default), wasting over 40 minutes of
# QEMU-emulated compute before failing on an architecture mismatch
# further in. Passing them explicitly removes the dependency on that
# auto-population working at all.
platform_build_args() {
  local platform=$1 rest arch variant
  rest="${platform#linux/}"
  arch="${rest%%/*}"
  variant=""
  [ "$rest" != "$arch" ] && variant="${rest#*/}"
  printf -- '--build-arg\nTARGETARCH=%s\n' "$arch"
  [ -n "$variant" ] && printf -- '--build-arg\nTARGETVARIANT=%s\n' "$variant"
}

# container_arm_tag PLATFORM — the image tag `build container-arm` produces
# for this platform, shared with `verify container-arm` so verify tests that
# exact image instead of running its own second `docker build` under a
# "-verify" tag. container-arm's build is the "very long" variant for any
# non-host --platform (see docs/wiki-ops.md) — paying that QEMU cost twice
# per verify defeats the point of build and verify being separate steps.
container_arm_tag() {
  local platform=$1
  if [ "$platform" = linux/arm/v7 ]; then
    # Matches docker-compose.arm.yml's own 'image: personal-wiki:arm' and
    # cmd_deploy_container's hardcoded tag for the documented armv7-on-a-Pi
    # target — keep this exact tag for the default platform so
    # `deploy --variant=container-arm` still finds the image it expects.
    printf 'personal-wiki:arm'
  else
    printf 'personal-wiki:%s' "$(platform_tag_suffix "$platform")"
  fi
}

# triplet_build_dir TRIPLET SUFFIX — "build-arm"/"build-arm-container" for the
# default triplet (arm-musl, preserving the long-documented directory name so
# nothing that already assumes it breaks), "build-$TRIPLET"/
# "build-$TRIPLET-container" for any other — so switching --triplet (e.g. to
# armv6-musl) never silently overwrites a different target's own build output
# in the same directory.
triplet_build_dir() {
  local triplet=$1 suffix=$2
  if [ "$triplet" = arm-musl ]; then
    printf 'build-arm%s' "$suffix"
  else
    printf 'build-%s%s' "$triplet" "$suffix"
  fi
}

# embeddings_cmake_args LOCAL_EMBEDDINGS CLOUD_EMBEDDINGS — the only two real
# WIKI_ENABLE_* flags that belong on a build/deploy command (CMakeLists.txt's
# third one, WIKI_ENABLE_FUZZING, is a dev-only tests/fuzz/ flag, needs Clang,
# and has no place here). Both OFF by default, matching CMakeLists.txt's own
# option() defaults — see docs/embeddings.md for what each actually requires.
embeddings_cmake_args() {
  local local_embeddings=$1 cloud_embeddings=$2 args=""
  [ "$local_embeddings" = 1 ] && args="$args -DWIKI_ENABLE_LOCAL_EMBEDDINGS=ON"
  [ "$cloud_embeddings" = 1 ] && args="$args -DWIKI_ENABLE_CLOUD_EMBEDDINGS=ON"
  printf '%s' "${args# }"
}

cmd_build_native() {
  local skip_tests=$1 local_embeddings=$2 cloud_embeddings=$3
  run cmake -S . -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    $(embeddings_cmake_args "$local_embeddings" "$cloud_embeddings")
  run cmake --build build -j"$(nproc)"
  if [ "$skip_tests" != 1 ]; then
    run ctest --test-dir build --output-on-failure
  fi
}

cmd_build_cross() {
  local triplet=$1 cross_dir=$2 skip_tests=$3 local_embeddings=$4 cloud_embeddings=$5
  local build_dir
  build_dir=$(triplet_build_dir "$triplet" "")
  local drogon_ctl="$SCRIPT_DIR/build/vcpkg_installed/x64-linux/tools/drogon/drogon_ctl"
  [ -x "$drogon_ctl" ] || die "native drogon_ctl not found at $drogon_ctl — run '$0 build native' first (classic-mode cross builds reuse the host's own drogon_ctl, see docs/sbc-deployment.md's Path B)"
  if [ "$local_embeddings" = 1 ]; then
    log "WIKI_ENABLE_LOCAL_EMBEDDINGS under zig/$triplet is UNVERIFIED — llama.cpp's own CMake has never been exercised against this cross-compilation path, see docs/wiki-ops.md"
  fi

  run bash -c "cd '$SCRIPT_DIR/vcpkg' && ./vcpkg install --classic --triplet '$triplet' \
    --overlay-triplets='$SCRIPT_DIR/$cross_dir' --overlay-ports='$SCRIPT_DIR/cross/overlay-ports' \
    --x-install-root='$SCRIPT_DIR/vcpkg_installed_arm' \
    drogon sqlite3[core,fts5,json1] argon2 nlohmann-json md4c yaml-cpp tomlplusplus catch2"

  export PKG_CONFIG_LIBDIR="$SCRIPT_DIR/vcpkg_installed_arm/$triplet/lib/pkgconfig:$SCRIPT_DIR/vcpkg_installed_arm/$triplet/share/pkgconfig"
  export PKG_CONFIG_SYSROOT_DIR=""

  run cmake -S . -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$cross_dir/toolchain.cmake" \
    -DCMAKE_PREFIX_PATH="$SCRIPT_DIR/vcpkg_installed_arm/$triplet" \
    -DCMAKE_FIND_ROOT_PATH="$SCRIPT_DIR/vcpkg_installed_arm/$triplet" \
    -DDROGON_CTL_COMMAND="$drogon_ctl" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    $(embeddings_cmake_args "$local_embeddings" "$cloud_embeddings")
  run cmake --build "$build_dir" -j"$(nproc)"

  if [ "$skip_tests" != 1 ]; then
    log "cross build has no on-host test run — use 'wiki-ops.sh verify cross --triplet=$triplet' (qemu-arm-static) instead"
  fi
  log "built into $build_dir/"
}

cmd_build_container() {
  local local_embeddings=$1 cloud_embeddings=$2
  local extra_args
  extra_args=$(embeddings_cmake_args "$local_embeddings" "$cloud_embeddings")
  if [ -n "$extra_args" ]; then
    run docker compose build --build-arg "WIKI_CMAKE_EXTRA_ARGS=$extra_args"
  else
    run docker compose build
  fi
}

cmd_build_cross_container() {
  local triplet=$1 local_embeddings=$2 cloud_embeddings=$3
  local out_dir
  out_dir=$(triplet_build_dir "$triplet" "-container")
  out_dir="$SCRIPT_DIR/$out_dir"
  if [ "$local_embeddings" = 1 ]; then
    log "WIKI_ENABLE_LOCAL_EMBEDDINGS under this zig toolchain is UNVERIFIED — see docs/wiki-ops.md"
  fi
  local extra_args build_args=(--build-arg "WIKI_CROSS_TRIPLET=$triplet")
  extra_args=$(embeddings_cmake_args "$local_embeddings" "$cloud_embeddings")
  if [ -n "$extra_args" ]; then
    run docker build -f cross/Dockerfile.builder "${build_args[@]}" --build-arg "WIKI_CMAKE_EXTRA_ARGS=$extra_args" -t wiki-cross-builder .
  else
    run docker build -f cross/Dockerfile.builder "${build_args[@]}" -t wiki-cross-builder .
  fi
  if [ "$DRY_RUN" = 1 ]; then
    log "(dry-run) would extract wiki-server/wiki-mcp/unit_tests from wiki-cross-builder into $out_dir/"
    return 0
  fi
  mkdir -p "$out_dir/tests"
  local cid
  cid=$(docker create wiki-cross-builder)
  docker cp "$cid:/out/wiki-server" "$out_dir/wiki-server"
  docker cp "$cid:/out/wiki-mcp" "$out_dir/wiki-mcp"
  docker cp "$cid:/out/unit_tests" "$out_dir/tests/unit_tests"
  docker rm "$cid" >/dev/null
  log "cross-container build artifacts in $out_dir/ (same shape as build-arm/ — verify with 'verify cross-container')"
}

cmd_build_container_arm() {
  local platform=$1 local_embeddings=$2 cloud_embeddings=$3
  local tag
  tag=$(container_arm_tag "$platform")
  [ "$platform" = linux/arm/v7 ] || log "non-default platform ($platform) — tagged $tag, not personal-wiki:arm; deploy's container-arm path won't pick this up automatically"
  local extra_args
  extra_args=$(embeddings_cmake_args "$local_embeddings" "$cloud_embeddings")
  local arch_args
  mapfile -t arch_args < <(platform_build_args "$platform")
  if [ -n "$extra_args" ]; then
    run docker buildx build --platform "$platform" "${arch_args[@]}" --build-arg "WIKI_CMAKE_EXTRA_ARGS=$extra_args" -t "$tag" --load .
  else
    run docker buildx build --platform "$platform" "${arch_args[@]}" -t "$tag" --load .
  fi
  log "built $tag"
}

# Builds just the main Dockerfile's "build" stage (stops before the
# runtime stage) and extracts /opt/wiki — already a complete
# cmake-install tree (CMakeLists.txt's install() rules already ran
# inside that stage, same as native's own `cmake --install`) — so the
# target never needs Docker at all. Same containerized-build idea as
# cross-container, but for a NATIVE (non-cross) build: no zig, and by
# default no --platform (the plain legacy builder, no buildx required,
# builds for whatever architecture this docker daemon's own host is —
# amd64 on a typical dev machine, matching Dockerfile's TARGETARCH=amd64
# default). An explicit --platform (needs buildx) can target a
# DIFFERENT architecture than this host's own, e.g. arm64-native-via-
# Docker with no zig and no musl — a real option alongside cross/
# cross-container for anything the main Dockerfile's TARGETARCH mapping
# already covers (amd64, arm64, armv7 — see Dockerfile's own header).
cmd_build_container_native() {
  local platform=$1 local_embeddings=$2 cloud_embeddings=$3
  local out_dir="$SCRIPT_DIR/build-container-native"
  local extra_args
  extra_args=$(embeddings_cmake_args "$local_embeddings" "$cloud_embeddings")
  local build_args=()
  [ -n "$extra_args" ] && build_args=(--build-arg "WIKI_CMAKE_EXTRA_ARGS=$extra_args")

  if [ -n "$platform" ]; then
    local arch_args
    mapfile -t arch_args < <(platform_build_args "$platform")
    run docker buildx build --target build --platform "$platform" "${arch_args[@]}" "${build_args[@]}" -t wiki-native-builder --load .
  else
    run docker build --target build "${build_args[@]}" -t wiki-native-builder .
  fi

  if [ "$DRY_RUN" = 1 ]; then
    log "(dry-run) would extract /opt/wiki from wiki-native-builder into $out_dir/"
    return 0
  fi
  rm -rf "$out_dir"
  mkdir -p "$out_dir"
  local cid
  cid=$(docker create wiki-native-builder)
  docker cp "$cid:/opt/wiki/." "$out_dir/"
  docker rm "$cid" >/dev/null
  log "container-native build artifacts in $out_dir/ — dynamically linked against the build image's own glibc (debian:bookworm-slim); a target with an OLDER glibc can fail with 'GLIBC_2.XX not found', the same class of problem cross/'s static musl path exists to avoid. Fine for a target running Debian bookworm+ (or anything with glibc >= that); use 'cross' instead for an old/unknown target."
}

# ============================================================
# SECTION: verify — qemu for bare-ARM binaries, docker run for images
# ============================================================

cmd_verify_cross() {
  local build_dir=$1 qemu_cpu=$2 skip_admin_smoke=$3
  local unit_tests_bin="$build_dir/tests/unit_tests"
  [ -x "$unit_tests_bin" ] || unit_tests_bin="$build_dir/unit_tests"
  [ -x "$unit_tests_bin" ] || die "no unit_tests binary found under $build_dir"

  # Picked from the binary's own ELF machine type, not the triplet name —
  # arm-musl/armv6-musl are both 32-bit ARMv6/v7 (qemu-arm-static), but
  # aarch64-musl is a different, 64-bit instruction set entirely
  # (qemu-aarch64-static); hardcoding qemu-arm-static unconditionally here
  # made `verify cross-container --triplet=aarch64-musl` fail outright
  # with "Invalid ELF image for this architecture" before this.
  local qemu_bin
  case "$(file -b "$unit_tests_bin")" in
    *"ARM aarch64"*) qemu_bin=qemu-aarch64-static ;;
    *", ARM, "*) qemu_bin=qemu-arm-static ;;
    *) die "don't know which qemu-user-static binary to use for $unit_tests_bin (unrecognized architecture: $(file -b "$unit_tests_bin"))" ;;
  esac
  command -v "$qemu_bin" >/dev/null 2>&1 || die "$qemu_bin not found — install qemu-user-static"

  local qemu_cpu_args=()
  if [ -n "$qemu_cpu" ]; then
    qemu_cpu_args=(-cpu "$qemu_cpu")
    # qemu-arm-static's DEFAULT cpu model, when -cpu isn't given, emulates
    # a broader instruction set than a real narrow/old core — confirmed:
    # the existing build-arm/ (cross/arm-musl, ARMv7) binary passes clean
    # under the bare default, then SIGILLs under -cpu arm1176 specifically
    # (same binary, same host). For cross/armv6-musl/ (ARM1176JZF-S —
    # Raspberry Pi 1/Zero/Zero W), pass --qemu-cpu=arm1176 here; the bare
    # default would give a false pass. `qemu-arm-static -cpu help` lists
    # every model name this qemu build supports if a different core ever
    # needs pinning.
    log "pinning qemu CPU model to '$qemu_cpu' — a pass here is only as trustworthy as that name actually matching the real target core"
  fi

  log "running unit_tests under $qemu_bin — must report all cases passed, not just exit 0"
  "$qemu_bin" "${qemu_cpu_args[@]}" "$unit_tests_bin"
  if [ "$skip_admin_smoke" = 1 ]; then
    log "skipping --create-admin smoke test — needs a real interactive TTY (password echo disabled), which 'update' never has"
  else
    log "running wiki-server --create-admin under $qemu_bin as a real smoke test"
    "$qemu_bin" "${qemu_cpu_args[@]}" "$build_dir/wiki-server" --create-admin
  fi
}

cmd_verify_container_native() {
  local build_dir=$1 skip_admin_smoke=$2
  [ -x "$build_dir/bin/wiki-server" ] || die "no wiki-server binary found under $build_dir/bin — run 'build container-native' first"
  # No qemu here — this is a native-architecture binary (unless --platform
  # was used to cross-build it with buildx, in which case it won't even
  # execute on this host at all; that combination has no local smoke test
  # available, only the full ctest run that already happened INSIDE the
  # docker build itself).
  if [ "$skip_admin_smoke" = 1 ]; then
    log "skipping --create-admin smoke test — needs a real interactive TTY (password echo disabled), which 'update' never has"
  else
    log "running wiki-server --create-admin directly (native binary, no emulation) as a real smoke test"
    "$build_dir/bin/wiki-server" --create-admin
  fi
}

cmd_verify_container() {
  docker compose config >/dev/null
  docker build -t personal-wiki:verify .
  local cid
  cid=$(docker run -d -p 18080:8080 personal-wiki:verify)
  sleep 2
  if curl -sf http://127.0.0.1:18080/healthz >/dev/null; then
    log "container verify: /healthz OK"
    docker rm -f "$cid" >/dev/null
  else
    docker rm -f "$cid" >/dev/null
    die "container verify: /healthz check failed"
  fi
}

cmd_verify_container_arm() {
  local platform=$1
  command -v docker >/dev/null || die "docker not found"
  local tag
  tag=$(container_arm_tag "$platform")
  docker image inspect "$tag" >/dev/null 2>&1 \
    || die "no image $tag found — run 'build container-arm --platform=$platform' first (verify never builds it itself, same as the cross/cross-container/container-native variants)"
  local cid
  cid=$(docker run -d --platform "$platform" -p 18081:8080 "$tag")
  sleep 5
  if curl -sf http://127.0.0.1:18081/healthz >/dev/null; then
    log "container-arm verify ($platform): /healthz OK (under QEMU emulation unless $platform is this host's own architecture)"
    docker rm -f "$cid" >/dev/null
  else
    docker rm -f "$cid" >/dev/null
    die "container-arm verify ($platform): /healthz check failed"
  fi
}

# ============================================================
# SECTION: deploy.local.env management
# ============================================================

cmd_config_init() {
  [ -f "$DEPLOY_CONFIG" ] && die "$DEPLOY_CONFIG already exists — edit it directly, or remove it first"
  [ -f "$SCRIPT_DIR/deploy.local.env.example" ] || die "missing deploy.local.env.example"
  git -C "$SCRIPT_DIR" check-ignore -q deploy.local.env \
    || die "deploy.local.env is not covered by .gitignore — refusing to create a file that could leak into git"
  cp "$SCRIPT_DIR/deploy.local.env.example" "$DEPLOY_CONFIG"
  log "created $DEPLOY_CONFIG — edit it now, or just re-run any subcommand and it'll prompt for whatever's still missing"
}

cmd_config_show() {
  [ -f "$DEPLOY_CONFIG" ] || die "no $DEPLOY_CONFIG yet — run '$0 config init' first"
  grep -vE '^\s*#|^\s*$' "$DEPLOY_CONFIG"
}

# ============================================================
# SECTION: nginx reverse-proxy config generation (writes a local file only
# — never touches a target's live nginx; TLS cert issuance stays manual)
# ============================================================

cmd_nginx_config() {
  local shape=$1 domain=$2 base_path=$3 out=$4
  local body
  case "$shape" in
    root)
      [ -n "$domain" ] || die "nginx-config root requires --domain=wiki.example.com"
      body=$(cat <<EOF
server {
    listen 443 ssl;
    server_name $domain;
    ssl_certificate     /etc/letsencrypt/live/$domain/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/$domain/privkey.pem;

    location / {
        client_max_body_size 0;  # unlimited; wiki-server's own 2 GiB request cap still applies
        proxy_pass http://127.0.0.1:8080/;
        proxy_set_header Host \$host;
        proxy_set_header X-Real-IP \$remote_addr;
        proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto \$scheme;
    }
}
EOF
)
      ;;
    subpath)
      [ -n "$base_path" ] || die "nginx-config subpath requires --base-path=/wiki"
      body=$(cat <<EOF
location = $base_path {
    return 301 $base_path/;
}

location $base_path/ {
    client_max_body_size 0;  # unlimited; wiki-server's own 2 GiB request cap still applies
    proxy_pass http://127.0.0.1:8080/;
    proxy_set_header Host \$host;
    proxy_set_header X-Real-IP \$remote_addr;
    proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto \$scheme;
}
EOF
)
      log "remember to also set [server].base_path = \"$base_path\" in config.toml and restart — closes the cold-start unstyled-page gap, see docs/sbc-deployment.md"
      ;;
    *) die "nginx-config: first argument must be 'root' or 'subpath'" ;;
  esac

  if [ -n "$out" ]; then
    printf '%s\n' "$body" > "$out"
    log "wrote $out — issue/attach a real TLS cert yourself (e.g. certbot) before using this; not this script's job"
  else
    printf '%s\n' "$body"
  fi
}

# ============================================================
# SECTION: systemd install / status (service + opt-in backup timer)
# ============================================================

cmd_systemd_install() {
  local with_backup_timer=$1 install_root=$2 variant=$3

  case "$variant" in
    container|container-arm)
      log "'$variant' is a Docker deployment — Docker's own restart: unless-stopped already owns the process lifecycle; skipping the wiki.service install (see docs/wiki-ops.md's 'Container variants' notes)"
      ;;
    *)
      # Confirmed live (2026-10-07): this used to overwrite an existing
      # target's wiki.service unconditionally, silently discarding a
      # customized EnvironmentFile= path (the target had one pointing at a
      # legacy location this repo no longer ships) — real outage, caused by
      # this one `cp` having no idea the target's unit differed from the
      # one it was about to ship. Back up whatever's there first regardless
      # (recoverable even if this check has a gap), and make a path
      # mismatch loud and confirmable instead of invisible.
      local existing_env_file new_env_file
      existing_env_file=$(remote_sh_capture "sed -n 's/^EnvironmentFile=-\\?//p' /etc/systemd/system/wiki.service 2>/dev/null || true")
      new_env_file=$(sed -n 's/^EnvironmentFile=-\?//p' systemd/wiki.service)
      if [ -n "$existing_env_file" ] && [ "$existing_env_file" != "$new_env_file" ]; then
        confirm_or_die "target's existing wiki.service reads env from '$existing_env_file', not this repo's '$new_env_file' — overwrite it anyway? (backed up first, but the running service's secrets may live at the old path)"
      fi
      # sed 's|/opt/wiki|$install_root|g', not a plain `cp`: the shipped
      # unit hardcodes /opt/wiki in ExecStart/WorkingDirectory/
      # ReadWritePaths (see systemd/wiki.service, and CMakeLists.txt's own
      # "Install layout — matches what systemd/wiki.service expects"
      # comment) — confirmed live, deploying to any OTHER install_root
      # silently installed a unit pointing at /opt/wiki regardless, which
      # on a box that happened to have something already at /opt/wiki from
      # an earlier, unrelated deploy ran THAT instead with no error at
      # all; on a box with nothing there it would have failed to start
      # outright. $install_root = /opt/wiki (the documented default) makes
      # this a no-op substitution, so nothing changes for that case.
      remote_sh "if [ -f /etc/systemd/system/wiki.service ]; then sudo cp /etc/systemd/system/wiki.service /etc/systemd/system/wiki.service.bak-\$(date +%Y%m%d-%H%M%S); fi; sed 's|/opt/wiki|$install_root|g' '$install_root/share/wiki/systemd/wiki.service' | sudo tee /etc/systemd/system/wiki.service >/dev/null && sudo systemctl daemon-reload"
      local is_active
      is_active=$(remote_sh_capture "systemctl is-active wiki.service 2>/dev/null || true")
      if [ "$is_active" = active ]; then
        confirm_or_die "wiki.service is already running on $( [ -n "${WIKI_DEPLOY_HOST:-}" ] && echo "$WIKI_DEPLOY_HOST" || echo "this machine" ) — restart it now?"
        remote_sh "sudo systemctl restart wiki.service"
      else
        remote_sh "sudo systemctl enable --now wiki.service"
      fi
      # Confirmed live (2026-10-07): `sleep 1` is nowhere near enough for a
      # binary that loads a local-embeddings model at startup, and a bare
      # `systemctl is-active` exits 3 for the normal "activating" transient
      # state — under remote_sh's own `set -e` preamble that killed this
      # whole health-check (and the deploy) before the curl line ever ran,
      # even though the service went on to start up fine seconds later.
      # Poll instead of guessing one fixed delay, and never let a
      # non-"active" status abort the script — report what's actually
      # there either way.
      # Port read from the target's own config.toml, not hardcoded 8080:
      # confirmed live, a deploy with --port=<anything else> left this
      # check curling the wrong port, always reporting "healthz: 000"
      # (connection refused) even when the service was actually healthy.
      local health_port
      health_port=$(remote_sh_capture "grep -m1 '^port = ' '$install_root/config.toml' 2>/dev/null | sed -E 's/^port = ([0-9]+).*/\\1/'" || true)
      [ -n "$health_port" ] || health_port=8080
      remote_sh "st=unknown; for i in 1 2 3 4 5 6 7 8 9 10; do sleep 2; st=\$(sudo systemctl is-active wiki.service 2>/dev/null || true); [ \"\$st\" = active ] && break; done; echo \"wiki.service: \$st\"; curl -s -o /dev/null -w 'healthz: %{http_code}\n' http://127.0.0.1:$health_port/healthz || true"
      ;;
  esac

  if [ "$with_backup_timer" = 1 ]; then
    cmd_backup_timer_install "$install_root"
  fi
}

# Ships wiki-backup.sh plus its systemd unit/timer/env fresh from THIS
# REPO's own systemd/ directory — never relies on anything a previous
# deploy step staged remotely, so it works identically whether
# install_root has a bin/+share/wiki/systemd/ tree already (native/cross/
# cross-container/container-native) or nothing at all yet (container/
# container-arm, which never stage one). VAULT_PATH defaults to
# $install_root/vault_data — the same path both a binary install's
# config.toml ([vault].path, relative to WorkingDirectory=$install_root)
# and a container deployment's host-side bind mount
# (docker-compose.yml's ./vault_data:/data/vault, same cwd) actually use.
cmd_backup_timer_install() {
  local install_root=$1
  ensure_remote_user "$WIKI_DEPLOY_SERVICE_USER" "$install_root"

  remote_sh "sudo mkdir -p '$install_root/bin'"
  copy_to_target systemd/wiki-backup.sh /tmp/wiki-backup.sh
  remote_sh "sudo mv /tmp/wiki-backup.sh '$install_root/bin/wiki-backup.sh' && sudo chown '$WIKI_DEPLOY_SERVICE_USER':'$WIKI_DEPLOY_SERVICE_USER' '$install_root/bin/wiki-backup.sh' && sudo chmod +x '$install_root/bin/wiki-backup.sh'"

  copy_to_target systemd/wiki-backup.service /tmp/wiki-backup.service
  copy_to_target systemd/wiki-backup.timer /tmp/wiki-backup.timer
  # Same /opt/wiki hardcoding as wiki.service (ExecStart, ReadOnlyPaths)
  # — see cmd_systemd_install's own comment on why a plain `mv` here is
  # wrong for any install_root other than the documented default.
  remote_sh "sed -i 's|/opt/wiki|$install_root|g' /tmp/wiki-backup.service && sudo mv /tmp/wiki-backup.service /etc/systemd/system/wiki-backup.service && sudo mv /tmp/wiki-backup.timer /etc/systemd/system/wiki-backup.timer"

  local has_env
  has_env=$(remote_sh_capture "[ -f /etc/opt/wiki/wiki-backup.env ] && echo yes || echo no")
  if [ "$has_env" = no ]; then
    remote_sh "sudo mkdir -p /etc/opt/wiki"
    copy_to_target systemd/wiki-backup.env.example /tmp/wiki-backup.env.example
    remote_sh "sudo mv /tmp/wiki-backup.env.example /etc/opt/wiki/wiki-backup.env && sudo sed -i 's|^VAULT_PATH=.*|VAULT_PATH=$install_root/vault_data|' /etc/opt/wiki/wiki-backup.env"
    local backup_dir
    resolve_optional backup_dir "" "${WIKI_DEPLOY_BACKUP_DIR:-}" "" "BACKUP_DIR (a DIFFERENT disk/mount than the vault; blank = edit it yourself later)"
    if [ -n "$backup_dir" ]; then
      remote_sh "sudo sed -i 's|^BACKUP_DIR=.*|BACKUP_DIR=$backup_dir|' /etc/opt/wiki/wiki-backup.env"
    else
      log "wrote /etc/opt/wiki/wiki-backup.env — VAULT_PATH defaulted to $install_root/vault_data; BACKUP_DIR left at its placeholder, edit it yourself (a DIFFERENT disk/mount than the vault)"
    fi
  fi

  remote_sh "sudo systemctl daemon-reload && sudo systemctl enable --now wiki-backup.timer"

  if [ "$install_root" != /opt/wiki ]; then
    log "WARNING: the shipped wiki-backup.service hardcodes ReadOnlyPaths=/opt/wiki/vault_data — install_root is '$install_root', not /opt/wiki, so this unit's own sandboxing won't actually permit reading the real vault path; pre-existing limitation of the shipped unit file itself, not specific to this deploy"
  fi
}

cmd_systemd_status() {
  remote_sh_capture "systemctl status wiki.service --no-pager; echo; systemctl list-timers wiki-backup.timer --no-pager 2>/dev/null || echo 'wiki-backup.timer not installed'"
}

# ============================================================
# SECTION: first-time config.toml fields + admin account
# ============================================================

# Edits only the small set of single-line scalar keys that appear
# exactly once, uncommented, in config.example.toml (see that file) —
# anchored sed substitutions, never a real TOML parser. The multi-line,
# commented-out [embeddings]/[llm] blocks are deliberately left alone;
# those need hand-editing on the target, same as docs/sbc-deployment.md
# already says. Only runs as part of `deploy --first-time`, against a
# config.toml that was just copied fresh from the example.
cmd_configure_toml() {
  local install_root=$1 port_flag=$2 cfg
  cfg="$install_root/config.toml"
  local listen_addr listen_addr_default port threads vault_path mcp_scope base_path
  local tls_enabled tls_cert_file tls_key_file

  # Asked BEFORE listen_addr: standalone TLS (src/server/CertWatcher, see
  # docs/sbc-deployment.md's "Standalone TLS" section) changes what a
  # SENSIBLE default for listen_addr even is (AppConfig::load() itself
  # doesn't validate this field either way — see its own comment on why).
  resolve_optional tls_enabled "" "${WIKI_DEPLOY_TLS_ENABLED:-}" "false" \
    "Standalone TLS — terminate TLS in wiki-server itself instead of behind a reverse proxy (true/false)"

  listen_addr_default="127.0.0.1"
  [ "$tls_enabled" = "true" ] && listen_addr_default="0.0.0.0"
  resolve listen_addr "" "${WIKI_DEPLOY_LISTEN_ADDR:-}" "$listen_addr_default" \
    "[server].listen_addr (loopback behind a reverse proxy, or a public/specific-interface address with standalone TLS above)"
  resolve port "$port_flag" "${WIKI_DEPLOY_PORT:-}" "8080" "[server].port"
  resolve threads "" "" "2" "[server].threads"
  resolve vault_path "" "" "./vault_data" "[vault].path"
  resolve mcp_scope "" "" "admin" "[mcp].scope (admin sees public+private; public for a remote transport)"
  resolve_optional base_path "" "${WIKI_DEPLOY_BASE_PATH:-}" "" "[server].base_path (optional, e.g. /wiki; blank = none — see docs/sbc-deployment.md's Reverse proxy section)"

  remote_sh "sudo sed -i -e 's|^listen_addr = .*|listen_addr = \"$listen_addr\"|' -e 's|^port = .*|port = $port|' -e 's|^threads = .*|threads = $threads|' '$cfg'"
  remote_sh "sudo sed -i 's|^path = .*|path = \"$vault_path\"|' '$cfg'"
  remote_sh "sudo sed -i 's|^scope = .*|scope = \"$mcp_scope\"|' '$cfg'"
  if [ -n "$base_path" ]; then
    remote_sh "sudo sed -i '/^\[server\]/a base_path = \"$base_path\"' '$cfg'"
  fi

  if [ "$tls_enabled" = "true" ]; then
    resolve tls_cert_file "" "${WIKI_DEPLOY_TLS_CERT_FILE:-}" "" \
      "[tls].cert_file (e.g. /etc/letsencrypt/live/example.com/fullchain.pem)"
    resolve tls_key_file "" "${WIKI_DEPLOY_TLS_KEY_FILE:-}" "" \
      "[tls].key_file (e.g. /etc/letsencrypt/live/example.com/privkey.pem)"
    remote_sh "sudo sed -i '/^\[tls\]/a enabled = true\ncert_file = \"$tls_cert_file\"\nkey_file = \"$tls_key_file\"' '$cfg'"
    log "standalone TLS configured — '$WIKI_DEPLOY_SERVICE_USER' needs READ access to the cert/key; certbot's default /etc/letsencrypt/{live,archive} is root-only. Run once (survives renewals): sudo setfacl -R -m u:$WIKI_DEPLOY_SERVICE_USER:rx /etc/letsencrypt/live /etc/letsencrypt/archive — see docs/sbc-deployment.md's 'Standalone TLS' section"
  fi
}

# ============================================================
# SECTION: secrets (/etc/opt/wiki/wiki.env) — real API key VALUES,
# never config.toml's api_key_env NAME (that's cmd_configure_toml's own
# [embeddings]/[llm] territory, still left to hand-edit — see
# docs/sbc-deployment.md). A value entered here NEVER touches
# deploy.local.env, a flag, or this script's own trace output — see
# resolve_secret()/remote_sh_secret()'s own comments for exactly how.
# ============================================================

cmd_configure_secrets() {
  local install_root=$1 variant=$2
  case "$variant" in
    container|container-arm)
      log "'$variant' is a Docker deployment — wiki.env has no meaning there; set the real key via docker-compose.yml's own environment:/env_file instead"
      return ;;
  esac

  # Both names may be unset (the common case: embeddings/llm provider is
  # "none", nothing needs a key), may be the same name (Anthropic/OpenAI
  # keys are interchangeable at this layer per wiki.env.example's own
  # comment — one key, one prompt, not two), or two different names.
  local names="${WIKI_DEPLOY_EMBEDDINGS_API_KEY_ENV:-} ${WIKI_DEPLOY_LLM_API_KEY_ENV:-}"
  local -A seen=()
  local name value

  for name in $names; do
    [ -z "$name" ] && continue
    [ -n "${seen[$name]:-}" ] && continue
    seen[$name]=1

    resolve_secret value "" "value for $name (written to /etc/opt/wiki/wiki.env on the target; leave blank to skip/leave unchanged)"
    if [ -z "$value" ]; then
      log "skipped $name — wiki.env left unchanged for this name"
      continue
    fi

    ensure_remote_user "$WIKI_DEPLOY_SERVICE_USER" "$install_root"
    remote_sh_secret \
      "sudo mkdir -p /etc/opt/wiki && sudo touch /etc/opt/wiki/wiki.env && \
read -r WIKI_SECRET_VALUE && \
sudo sed -i \"/^$name=/d\" /etc/opt/wiki/wiki.env && \
printf '%s=%s\n' '$name' \"\$WIKI_SECRET_VALUE\" | sudo tee -a /etc/opt/wiki/wiki.env >/dev/null && \
sudo chown '$WIKI_DEPLOY_SERVICE_USER':'$WIKI_DEPLOY_SERVICE_USER' /etc/opt/wiki/wiki.env && \
sudo chmod 600 /etc/opt/wiki/wiki.env" \
      "$value"
    log "wrote $name to /etc/opt/wiki/wiki.env (mode 600, owned by $WIKI_DEPLOY_SERVICE_USER) — make sure config.toml's matching api_key_env is actually set to '$name', that part is still yours to hand-edit"
  done
}

# ============================================================
# SECTION: first-time admin account (real interactive TTY, never scripted)
# ============================================================

cmd_setup_admin() {
  local install_root=$1 variant=$2
  case "$variant" in
    container|container-arm)
      die "setup-admin assumes a binary install — '$variant' runs via Docker; create the admin account with: docker compose exec wiki wiki-server --create-admin" ;;
  esac
  if [ -n "${WIKI_DEPLOY_HOST:-}" ]; then
    run ssh -t -p "$WIKI_DEPLOY_SSH_PORT" "$WIKI_DEPLOY_HOST" \
      "cd '$install_root' && sudo -u '$WIKI_DEPLOY_SERVICE_USER' ./bin/wiki-server --create-admin"
  else
    run bash -c "cd '$install_root' && sudo -u '$WIKI_DEPLOY_SERVICE_USER' ./bin/wiki-server --create-admin"
  fi
}

# ============================================================
# SECTION: static-assets-only fast path (docs/sbc-deployment.md's
# "Static-assets-only redeploy" section, verbatim)
# ============================================================

verify_static_md5() {
  local install_root=$1
  local local_md5 remote_md5
  local_md5=$(mktemp)
  remote_md5=$(mktemp)
  find static -type f -exec md5sum {} \; | sed 's|static/||' | sort > "$local_md5"
  remote_sh_capture "cd '$install_root/static' && find . -type f -exec md5sum {} \;" \
    | sed 's|  \./|  |' | sort > "$remote_md5"
  if diff -q "$local_md5" "$remote_md5" >/dev/null; then
    log "static-redeploy: md5 verification OK"
  else
    diff "$local_md5" "$remote_md5" || true
    rm -f "$local_md5" "$remote_md5"
    die "static-redeploy: md5 MISMATCH — see diff above"
  fi
  rm -f "$local_md5" "$remote_md5"
}

cmd_static_redeploy() {
  local install_root=$1 variant=$2
  case "$variant" in
    container|container-arm)
      die "static-redeploy assumes a binary install — '$variant' serves static/ from inside the image; rebuild ('build $variant') and 'deploy --variant=$variant' again instead" ;;
  esac
  local tmp_tar remote_tar
  tmp_tar=$(mktemp /tmp/wiki-static-XXXXXX.tar.gz)
  run tar -czf "$tmp_tar" -C static .
  remote_tar="/tmp/$(basename "$tmp_tar")"
  copy_to_target "$tmp_tar" "$remote_tar"

  remote_sh "rm -rf /tmp/static-new && mkdir -p /tmp/static-new && \
tar -xzf '$remote_tar' -C /tmp/static-new && \
sudo chown -R '$WIKI_DEPLOY_SERVICE_USER':'$WIKI_DEPLOY_SERVICE_USER' /tmp/static-new && \
cd '$install_root' && sudo rm -rf static.old && sudo mv static static.old && sudo mv /tmp/static-new static && \
sudo systemctl restart wiki.service && \
sudo rm -rf static.old '$remote_tar'"

  if [ "$DRY_RUN" != 1 ]; then
    verify_static_md5 "$install_root"
  fi
  rm -f "$tmp_tar"
}

# ============================================================
# SECTION: deploy — binary variants (native/cross/cross-container) via
# staged atomic swap; container variants via image save/load
# ============================================================

ensure_remote_user() {
  local user=$1 install_root=$2
  remote_sh "if ! id '$user' >/dev/null 2>&1; then sudo useradd --system --home-dir '$install_root' --shell /usr/sbin/nologin '$user'; fi"
}

# Stages a cmake-install-shaped tree for a variant that has no cmake
# --install of its own (cross/cross-container only produce the two
# binaries + static/ + the same auxiliary files cmake --install would
# have staged from the native build).
stage_bare_binary_tree() {
  local build_dir=$1 staging=$2
  mkdir -p "$staging/bin" "$staging/share/wiki/systemd"
  cp "$build_dir/wiki-server" "$build_dir/wiki-mcp" "$staging/bin/"
  cp -r static "$staging/static"
  cp config.example.toml "$staging/"
  cp systemd/wiki.service systemd/wiki.env.example "$staging/share/wiki/systemd/"
  cp systemd/wiki-backup.service systemd/wiki-backup.timer systemd/wiki-backup.env.example "$staging/share/wiki/systemd/"
  cp systemd/wiki-backup.sh "$staging/bin/"
  chmod +x "$staging/bin/wiki-backup.sh"
}

# deploy_swap_tree STAGING_DIR INSTALL_ROOT STRIP_BINS
# Reproduces docs/sbc-deployment.md's "Update" atomic-swap sequence:
# .bak-$STAMP the live bin/+static/, move the new ones into place, chown,
# strip on the TARGET (never the dev machine — see the doc's note on why
# the dev machine's own `strip` can't read a cross ARM ELF; a target
# missing `strip` entirely — confirmed on a minimal Armbian image — is
# handled explicitly, not silently, see strip_cmd below).
#
# Also creates vault_data here, before cmd_systemd_install ever starts
# the service — confirmed live on a fresh target: the shipped
# wiki.service's `ReadWritePaths=.../vault_data` makes systemd's own
# mount-namespace setup fail outright (code=226/NAMESPACE) if that
# directory doesn't already exist, and nothing else in this flow created
# it first. Without this, the service only came up by accident, because
# a concurrent --create-admin run (outside systemd's sandbox) happened
# to create it in the ~2s gap between two of systemd's own on-failure
# restarts — not a real guarantee on a fresh target.
deploy_swap_tree() {
  local staging=$1 install_root=$2 strip_bins=$3
  local user=$WIKI_DEPLOY_SERVICE_USER

  ensure_remote_user "$user" "$install_root"

  local remote_staging="/tmp/wiki-ops-deploy-$$"
  remote_sh "rm -rf '$remote_staging'"
  copy_to_target "$staging" "$remote_staging"

  local strip_cmd=""
  if [ "$strip_bins" = 1 ]; then
    # `&&`-gating this ahead of the rm -rf/mv below used to mean a target
    # with no `strip` installed (confirmed live: a minimal Armbian image
    # has none) silently skipped not just the strip but LOOKED like it
    # might skip the rm -rf too — set -e does NOT abort a bare `cmd1 &&
    # cmd2` statement when cmd1 fails (only the LAST command in an AND/OR
    # list can trigger -e; confirmed empirically, this is correct but
    # unintuitive bash semantics), so the rm -rf/mv after it always ran
    # regardless — but on an UPDATE (existing bin/ dir) a failed strip
    # skipping jumped straight to `mv` with no clean rm -rf first in
    # some orderings, risking a nested bin/bin/. Now a stand-alone
    # statement, never silently swallowed, and never gates anything after it.
    strip_cmd="if command -v strip >/dev/null 2>&1; then sudo strip --strip-all '$remote_staging/bin/wiki-server' '$remote_staging/bin/wiki-mcp'; else echo 'wiki-ops: strip not found on target -- shipping unstripped binaries' >&2; fi; "
  fi

  remote_sh "STAMP=\$(date +%Y%m%d-%H%M%S); \
sudo mkdir -p '$install_root'; \
if [ -d '$install_root/bin' ]; then sudo cp -r '$install_root/bin' \"$install_root/bin.bak-\$STAMP\"; fi; \
if [ -d '$install_root/static' ]; then sudo mv '$install_root/static' \"$install_root/static.bak-\$STAMP\"; fi; \
sudo chmod +x '$remote_staging/bin/wiki-server' '$remote_staging/bin/wiki-mcp' '$remote_staging/bin/wiki-backup.sh'; \
$strip_cmd \
sudo rm -rf '$install_root/bin'; \
sudo mv '$remote_staging/bin' '$install_root/bin'; \
sudo mv '$remote_staging/static' '$install_root/static'; \
[ -f '$install_root/config.toml' ] || sudo cp '$remote_staging/config.example.toml' '$install_root/config.toml'; \
sudo cp '$remote_staging/config.example.toml' '$install_root/config.example.toml'; \
sudo mkdir -p '$install_root/share/wiki/systemd'; \
sudo cp '$remote_staging'/share/wiki/systemd/* '$install_root/share/wiki/systemd/'; \
sudo mkdir -p '$install_root/vault_data'; \
sudo chown -R '$user':'$user' '$install_root'; \
rm -rf '$remote_staging'"
}

# cmd_deploy_container VARIANT INSTALL_ROOT HOST_PORT PLATFORM — PLATFORM is
# only consulted for container-arm (ignored, may be blank, for plain
# container) and MUST be the exact same --platform `build container-arm`
# was given: container_arm_tag() is how both `build`/`verify` already name
# a non-default-platform image, and this needs to pick up that SAME tag,
# not the hardcoded armv7 one, or docker-compose.arm.yml's own default
# 'image: personal-wiki:arm' silently runs whatever stale armv7 image
# happens to already be tagged that, which fails with a plain "exec format
# error" rather than any clearer signal — confirmed live deploying an
# arm64-built image to an aarch64 target with CONFIG_COMPAT unset (no
# 32-bit compat at all) before this was threaded through.
cmd_deploy_container() {
  local variant=$1 install_root=$2 host_port=$3 platform=$4
  local extra_compose=()
  local image_tag=personal-wiki:latest
  if [ "$variant" = container-arm ]; then
    extra_compose=(-f docker-compose.arm.yml)
    image_tag=$(container_arm_tag "${platform:-linux/arm/v7}")
  fi

  if [ -z "${WIKI_DEPLOY_HOST:-}" ]; then
    if [ -n "$host_port" ]; then
      export WIKI_HOST_PORT="$host_port"
      log "WIKI_HOST_PORT=$host_port exported for docker compose (container's own internal port stays 8080)"
    fi
    export WIKI_IMAGE_TAG="$image_tag"
    run docker compose -f docker-compose.yml "${extra_compose[@]}" up -d
    return
  fi

  local tmp_image remote_image
  tmp_image=$(mktemp /tmp/wiki-ops-image-XXXXXX.tar)
  run bash -c "docker save '$image_tag' -o '$tmp_image'"
  remote_image="/tmp/$(basename "$tmp_image")"
  copy_to_target "$tmp_image" "$remote_image"
  # sudo on docker load/compose below: unlike the native/cross paths,
  # this doesn't assume the target's docker group — only that the SSH
  # login user has (passwordless) sudo, the same single requirement
  # every other remote_sh call in this script already relies on.
  remote_sh "sudo docker load -i '$remote_image' && rm -f '$remote_image'"
  rm -f "$tmp_image"

  # Staged into /tmp then sudo-moved, not scp'd straight into
  # install_root: install_root (e.g. /opt/wiki) is root-owned on a fresh
  # target, same as every other deploy path's staging dance (see
  # deploy_swap_tree) — a non-root SSH login user's scp would otherwise
  # fail outright with "Permission denied" before docker ever runs.
  copy_to_target docker-compose.yml /tmp/wiki-docker-compose.yml
  remote_sh "sudo mkdir -p '$install_root' && sudo mv /tmp/wiki-docker-compose.yml '$install_root/docker-compose.yml'"
  if [ "$variant" = container-arm ]; then
    copy_to_target docker-compose.arm.yml /tmp/wiki-docker-compose.arm.yml
    remote_sh "sudo mv /tmp/wiki-docker-compose.arm.yml '$install_root/docker-compose.arm.yml'"
  fi
  local compose_flags="-f docker-compose.yml"
  [ "$variant" = container-arm ] && compose_flags="-f docker-compose.yml -f docker-compose.arm.yml"
  local env_prefix=""
  [ -n "$host_port" ] && env_prefix="WIKI_HOST_PORT='$host_port' "
  [ "$variant" = container-arm ] && env_prefix="${env_prefix}WIKI_IMAGE_TAG='$image_tag' "
  # chown -R 1000:1000, not just mkdir -p: the image runs as a fixed
  # uid/gid 1000 (Dockerfile, see docker-compose.yml's own comment on
  # why) — a freshly `mkdir -p`'d directory over ssh is root:root, which
  # a uid-1000 process can traverse but not write into, so the container
  # crash-looped on every first deploy to a brand new install_root with
  # "unable to open database file" before this — confirmed live, not a
  # hypothetical. -R also self-heals the other documented caveat
  # (docker-compose.yml: "if your host user is a different uid, chown -R
  # 1000:1000 vault_data") when this install_root previously held a
  # native/cross binary deployment's own vault_data, owned by whatever
  # uid that deployment's system user happened to get.
  # `sudo env VAR=val` (not `VAR=val sudo`), so the override survives
  # sudo's env_reset regardless of this target's own env_keep config —
  # a bare `VAR=val sudo cmd` sets the variable for sudo itself, not for
  # the command sudo then execs.
  remote_sh "sudo mkdir -p '$install_root/vault_data' && sudo chown -R 1000:1000 '$install_root/vault_data' && cd '$install_root' && sudo env ${env_prefix}docker compose $compose_flags up -d"
}

cmd_deploy() {
  local flag_target=$1 flag_variant=$2 first_time=$3 with_backup_timer=$4 static_only=$5 skip_verify=$6 qemu_cpu=$7 port_flag=$8 triplet_flag=$9
  local skip_admin_smoke=${10:-0}

  resolve_optional WIKI_DEPLOY_HOST "$flag_target" "${WIKI_DEPLOY_HOST:-}" "" "Deploy target (user@host; empty = local install)"
  resolve WIKI_DEPLOY_VARIANT "$flag_variant" "${WIKI_DEPLOY_VARIANT:-}" "cross" "Build variant to deploy (native|cross|container|cross-container|container-arm|container-native)"
  resolve WIKI_DEPLOY_INSTALL_ROOT "" "${WIKI_DEPLOY_INSTALL_ROOT:-}" "/opt/wiki" "Install root"
  resolve WIKI_DEPLOY_SERVICE_USER "" "${WIKI_DEPLOY_SERVICE_USER:-}" "wiki" "Service user"
  resolve WIKI_DEPLOY_SSH_PORT "" "${WIKI_DEPLOY_SSH_PORT:-}" "22" "SSH port"
  resolve WIKI_DEPLOY_TRIPLET "$triplet_flag" "${WIKI_DEPLOY_TRIPLET:-}" "arm-musl" "cross/cross-container triplet (ignored for every other variant)"

  if [ "$static_only" = 1 ]; then
    cmd_static_redeploy "$WIKI_DEPLOY_INSTALL_ROOT" "$WIKI_DEPLOY_VARIANT"
    return
  fi

  case "$WIKI_DEPLOY_VARIANT" in
    native|cross|cross-container|container-native) ;;
    container|container-arm)
      # Docker owns this process's lifecycle (docker-compose.yml's own
      # restart: unless-stopped) — there is no $install_root/share/wiki/
      # systemd/wiki.service staged anywhere for this variant (only
      # native/cross/cross-container/container-native's install step
      # produces that tree). cmd_systemd_install knows to skip the
      # wiki.service part for these two variants — but --with-backup-timer
      # still works, via cmd_backup_timer_install shipping wiki-backup.sh
      # fresh from this repo's own systemd/ directory, independent of
      # anything a container deploy would otherwise stage.
      resolve_optional WIKI_DEPLOY_PORT "$port_flag" "${WIKI_DEPLOY_PORT:-}" "" "Host-side port (blank = compose default, 8080)"
      # $FLAG_PLATFORM: same global dispatch_build already reads directly
      # (see its own comment) rather than threading it through as a
      # parameter. Must be the SAME platform `build container-arm` was
      # given, or this loads/runs the wrong image — see
      # cmd_deploy_container's own comment on exactly how that fails.
      cmd_deploy_container "$WIKI_DEPLOY_VARIANT" "$WIKI_DEPLOY_INSTALL_ROOT" "$WIKI_DEPLOY_PORT" "${FLAG_PLATFORM:-linux/arm/v7}"
      cmd_systemd_install "$with_backup_timer" "$WIKI_DEPLOY_INSTALL_ROOT" "$WIKI_DEPLOY_VARIANT"
      return
      ;;
    *) die "deploy: unknown variant '$WIKI_DEPLOY_VARIANT'" ;;
  esac

  local build_dir strip_bins=0 verify_kind=""
  case "$WIKI_DEPLOY_VARIANT" in
    native) build_dir=build ;;
    cross) build_dir=$(triplet_build_dir "$WIKI_DEPLOY_TRIPLET" ""); strip_bins=1; verify_kind=cross ;;
    cross-container) build_dir=$(triplet_build_dir "$WIKI_DEPLOY_TRIPLET" "-container"); strip_bins=1; verify_kind=cross ;;
    container-native) build_dir=build-container-native; strip_bins=1; verify_kind=container-native ;;
  esac
  [ -d "$build_dir" ] || die "no $build_dir/ found — run 'build $WIKI_DEPLOY_VARIANT' first"

  if [ "$skip_verify" != 1 ]; then
    case "$verify_kind" in
      cross) cmd_verify_cross "$build_dir" "$qemu_cpu" "$skip_admin_smoke" ;;
      container-native) cmd_verify_container_native "$build_dir" "$skip_admin_smoke" ;;
    esac
  fi

  local staging
  staging=$(mktemp -d /tmp/wiki-ops-stage-XXXXXX)
  case "$WIKI_DEPLOY_VARIANT" in
    native) run cmake --install "$build_dir" --prefix "$staging" ;;
    container-native) cp -r "$build_dir"/. "$staging"/ ;;
    *) stage_bare_binary_tree "$build_dir" "$staging" ;;
  esac

  deploy_swap_tree "$staging" "$WIKI_DEPLOY_INSTALL_ROOT" "$strip_bins"
  rm -rf "$staging"

  if [ "$first_time" = 1 ]; then
    cmd_configure_toml "$WIKI_DEPLOY_INSTALL_ROOT" "$port_flag"
    cmd_configure_secrets "$WIKI_DEPLOY_INSTALL_ROOT" "$WIKI_DEPLOY_VARIANT"
  fi

  cmd_systemd_install "$with_backup_timer" "$WIKI_DEPLOY_INSTALL_ROOT" "$WIKI_DEPLOY_VARIANT"

  if [ "$first_time" = 1 ]; then
    cmd_setup_admin "$WIKI_DEPLOY_INSTALL_ROOT" "$WIKI_DEPLOY_VARIANT"
  fi
}

# ============================================================
# SECTION: usage / dispatch
# ============================================================

usage() {
  cat <<EOF
wiki-ops.sh — build+deploy orchestration for this repo (see docs/sbc-deployment.md
for the manual steps this wraps; this script doesn't change what happens, just
removes the hand-typing and enforces the order).

  build   native|cross|container|cross-container|container-arm|container-native
          [--skip-tests] [--triplet=NAME] [--cross-dir=PATH]
          [--platform=P] [--local-embeddings] [--cloud-embeddings] [--dry-run]
          (--platform default: linux/arm/v7 for container-arm, this host's own
           architecture for container-native)
  verify  cross|container|cross-container|container-arm|container-native
          [--platform=linux/arm/v7] [--qemu-cpu=NAME]
  deploy  [--target=HOST] [--variant=V] [--first-time] [--with-backup-timer]
          [--static-only] [--skip-verify] [--qemu-cpu=NAME] [--port=N]
          [--yes] [--dry-run]
  update  [--target=HOST] [--variant=V] [--triplet=NAME] [--platform=P]
          [--static-only] [--with-backup-timer] [--skip-verify]
          [--qemu-cpu=NAME] [--port=N] [--yes] [--dry-run]
          (git pull, then build V, then a non-first-time deploy to an
           EXISTING deployment — config.toml/vault/admin account untouched;
           --static-only skips straight to static-redeploy instead)
  static-redeploy [--target=HOST] [--variant=V] [--dry-run]
  setup-admin [--target=HOST] [--variant=V]
  secrets [--target=HOST] [--variant=V]
          (prompts, echo disabled, for each of WIKI_DEPLOY_EMBEDDINGS_API_KEY_ENV /
           WIKI_DEPLOY_LLM_API_KEY_ENV's real VALUE and writes it to the target's
           /etc/opt/wiki/wiki.env — also runs automatically as part of
           deploy --first-time; re-run standalone any time to rotate a key)
  systemd install [--target=HOST] [--variant=V] [--with-backup-timer] [--yes] [--dry-run]
  systemd status  [--target=HOST]
  nginx-config root --domain=D [--out=FILE]
  nginx-config subpath --base-path=/wiki [--out=FILE]
  config  init|show
  help

Flags always win over deploy.local.env (see deploy.local.env.example);
anything neither supplies is asked interactively when a terminal is
attached, otherwise it's a hard error.
EOF
}

# parse_flags consumes --target=/--variant=/--yes/--dry-run/etc. from the
# remaining args into the FLAG_* globals below; an unrecognized flag is a
# hard error (die), not silently ignored.
FLAG_TARGET=""
FLAG_VARIANT=""
FLAG_DOMAIN=""
FLAG_BASE_PATH=""
FLAG_OUT=""
FLAG_TRIPLET=""
FLAG_CROSS_DIR=""
FLAG_PLATFORM=""
FLAG_QEMU_CPU=""
FLAG_PORT=""
FLAG_SKIP_TESTS=0
FLAG_LOCAL_EMBEDDINGS=0
FLAG_CLOUD_EMBEDDINGS=0
FLAG_FIRST_TIME=0
FLAG_WITH_BACKUP_TIMER=0
FLAG_STATIC_ONLY=0
FLAG_SKIP_VERIFY=0

parse_flags() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --target=*) FLAG_TARGET=${1#*=} ;;
      --variant=*) FLAG_VARIANT=${1#*=} ;;
      --domain=*) FLAG_DOMAIN=${1#*=} ;;
      --base-path=*) FLAG_BASE_PATH=${1#*=} ;;
      --out=*) FLAG_OUT=${1#*=} ;;
      --triplet=*) FLAG_TRIPLET=${1#*=} ;;
      --cross-dir=*) FLAG_CROSS_DIR=${1#*=} ;;
      --platform=*) FLAG_PLATFORM=${1#*=} ;;
      --qemu-cpu=*) FLAG_QEMU_CPU=${1#*=} ;;
      --port=*) FLAG_PORT=${1#*=} ;;
      --skip-tests) FLAG_SKIP_TESTS=1 ;;
      --local-embeddings) FLAG_LOCAL_EMBEDDINGS=1 ;;
      --cloud-embeddings) FLAG_CLOUD_EMBEDDINGS=1 ;;
      --first-time) FLAG_FIRST_TIME=1 ;;
      --with-backup-timer) FLAG_WITH_BACKUP_TIMER=1 ;;
      --static-only) FLAG_STATIC_ONLY=1 ;;
      --skip-verify) FLAG_SKIP_VERIFY=1 ;;
      --yes) ASSUME_YES=1 ;;
      --dry-run) DRY_RUN=1 ;;
      *) die "unrecognized flag: $1" ;;
    esac
    shift
  done
}

# dispatch_build VARIANT — runs the matching cmd_build_* using the current
# FLAG_* globals. Shared by the `build` and `update` subcommands so picking
# the right build function for a variant lives in exactly one place.
dispatch_build() {
  local sub=$1
  case "$sub" in
    native) cmd_build_native "$FLAG_SKIP_TESTS" "$FLAG_LOCAL_EMBEDDINGS" "$FLAG_CLOUD_EMBEDDINGS" ;;
    cross)
      local resolved_triplet="${FLAG_TRIPLET:-arm-musl}"
      cmd_build_cross "$resolved_triplet" "${FLAG_CROSS_DIR:-cross/$resolved_triplet}" "$FLAG_SKIP_TESTS" "$FLAG_LOCAL_EMBEDDINGS" "$FLAG_CLOUD_EMBEDDINGS"
      ;;
    container) cmd_build_container "$FLAG_LOCAL_EMBEDDINGS" "$FLAG_CLOUD_EMBEDDINGS" ;;
    cross-container) cmd_build_cross_container "${FLAG_TRIPLET:-arm-musl}" "$FLAG_LOCAL_EMBEDDINGS" "$FLAG_CLOUD_EMBEDDINGS" ;;
    container-arm) cmd_build_container_arm "${FLAG_PLATFORM:-linux/arm/v7}" "$FLAG_LOCAL_EMBEDDINGS" "$FLAG_CLOUD_EMBEDDINGS" ;;
    container-native) cmd_build_container_native "$FLAG_PLATFORM" "$FLAG_LOCAL_EMBEDDINGS" "$FLAG_CLOUD_EMBEDDINGS" ;;
    *) die "build: variant must be one of native|cross|container|cross-container|container-arm|container-native" ;;
  esac
}

# cmd_update VARIANT TARGET — `git pull` then the matching build and a
# non-first-time deploy, for picking up new commits on an existing
# deployment without re-typing the usual build+deploy pair by hand.
# --static-only skips the build+full-deploy entirely in favor of
# static-redeploy, for a pull that only touched static/.
cmd_update() {
  local variant=$1 target=$2 static_only=$3 with_backup_timer=$4 skip_verify=$5 qemu_cpu=$6 port=$7 triplet=$8
  run git pull
  if [ "$static_only" = 1 ]; then
    cmd_static_redeploy "${WIKI_DEPLOY_INSTALL_ROOT:-/opt/wiki}" "$variant"
  else
    dispatch_build "$variant"
    # Final 1: skip the --create-admin smoke test specifically — it needs a
    # real interactive TTY (password echo disabled), which update never has.
    # The rest of verify (unit_tests under qemu, or the ctest run already
    # inside container-native's own docker build) still runs normally.
    cmd_deploy "$target" "$variant" 0 "$with_backup_timer" 0 "$skip_verify" "$qemu_cpu" "$port" "$triplet" 1
  fi
}

main() {
  local cmd=${1:-help}
  [ $# -gt 0 ] && shift

  case "$cmd" in
    help|-h|--help) usage ;;

    build)
      local sub=${1:-}
      [ $# -gt 0 ] && shift
      parse_flags "$@"
      load_deploy_config
      dispatch_build "$sub"
      ;;

    update)
      parse_flags "$@"
      load_deploy_config
      # Resolved here (not left to cmd_deploy's own resolve) because the
      # --static-only branch calls cmd_static_redeploy directly, bypassing
      # cmd_deploy entirely — it still needs WIKI_DEPLOY_HOST/INSTALL_ROOT
      # set correctly, same as the dedicated static-redeploy subcommand.
      resolve_optional WIKI_DEPLOY_HOST "$FLAG_TARGET" "${WIKI_DEPLOY_HOST:-}" "" "Deploy target (user@host; empty = local install)"
      resolve WIKI_DEPLOY_INSTALL_ROOT "" "${WIKI_DEPLOY_INSTALL_ROOT:-}" "/opt/wiki" "Install root"
      resolve WIKI_DEPLOY_SERVICE_USER "" "${WIKI_DEPLOY_SERVICE_USER:-}" "wiki" "Service user"
      resolve WIKI_DEPLOY_SSH_PORT "" "${WIKI_DEPLOY_SSH_PORT:-}" "22" "SSH port"
      resolve WIKI_DEPLOY_VARIANT "$FLAG_VARIANT" "${WIKI_DEPLOY_VARIANT:-}" "cross" "Build variant to update (native|cross|container|cross-container|container-arm|container-native)"
      cmd_update "$WIKI_DEPLOY_VARIANT" "$FLAG_TARGET" "$FLAG_STATIC_ONLY" "$FLAG_WITH_BACKUP_TIMER" "$FLAG_SKIP_VERIFY" "$FLAG_QEMU_CPU" "$FLAG_PORT" "$FLAG_TRIPLET"
      ;;

    verify)
      local sub=${1:-}
      [ $# -gt 0 ] && shift
      parse_flags "$@"
      case "$sub" in
        cross) cmd_verify_cross "$(triplet_build_dir "${FLAG_TRIPLET:-arm-musl}" "")" "$FLAG_QEMU_CPU" 0 ;;
        cross-container) cmd_verify_cross "$(triplet_build_dir "${FLAG_TRIPLET:-arm-musl}" "-container")" "$FLAG_QEMU_CPU" 0 ;;
        container) cmd_verify_container ;;
        container-arm) cmd_verify_container_arm "${FLAG_PLATFORM:-linux/arm/v7}" ;;
        container-native) cmd_verify_container_native build-container-native 0 ;;
        *) die "verify: variant must be one of cross|cross-container|container|container-arm|container-native" ;;
      esac
      ;;

    deploy)
      parse_flags "$@"
      load_deploy_config
      cmd_deploy "$FLAG_TARGET" "$FLAG_VARIANT" "$FLAG_FIRST_TIME" "$FLAG_WITH_BACKUP_TIMER" "$FLAG_STATIC_ONLY" "$FLAG_SKIP_VERIFY" "$FLAG_QEMU_CPU" "$FLAG_PORT" "$FLAG_TRIPLET"
      ;;

    static-redeploy)
      parse_flags "$@"
      load_deploy_config
      resolve_optional WIKI_DEPLOY_HOST "$FLAG_TARGET" "${WIKI_DEPLOY_HOST:-}" "" "Deploy target (user@host; empty = local install)"
      resolve WIKI_DEPLOY_INSTALL_ROOT "" "${WIKI_DEPLOY_INSTALL_ROOT:-}" "/opt/wiki" "Install root"
      resolve WIKI_DEPLOY_SERVICE_USER "" "${WIKI_DEPLOY_SERVICE_USER:-}" "wiki" "Service user"
      resolve WIKI_DEPLOY_SSH_PORT "" "${WIKI_DEPLOY_SSH_PORT:-}" "22" "SSH port"
      resolve_optional WIKI_DEPLOY_VARIANT "$FLAG_VARIANT" "${WIKI_DEPLOY_VARIANT:-}" "" "Variant (blank if unknown — only checked to refuse container/container-arm)"
      cmd_static_redeploy "$WIKI_DEPLOY_INSTALL_ROOT" "$WIKI_DEPLOY_VARIANT"
      ;;

    setup-admin)
      parse_flags "$@"
      load_deploy_config
      resolve_optional WIKI_DEPLOY_HOST "$FLAG_TARGET" "${WIKI_DEPLOY_HOST:-}" "" "Deploy target (user@host; empty = local install)"
      resolve WIKI_DEPLOY_INSTALL_ROOT "" "${WIKI_DEPLOY_INSTALL_ROOT:-}" "/opt/wiki" "Install root"
      resolve WIKI_DEPLOY_SERVICE_USER "" "${WIKI_DEPLOY_SERVICE_USER:-}" "wiki" "Service user"
      resolve WIKI_DEPLOY_SSH_PORT "" "${WIKI_DEPLOY_SSH_PORT:-}" "22" "SSH port"
      resolve_optional WIKI_DEPLOY_VARIANT "$FLAG_VARIANT" "${WIKI_DEPLOY_VARIANT:-}" "" "Variant (blank if unknown — only checked to refuse container/container-arm)"
      cmd_setup_admin "$WIKI_DEPLOY_INSTALL_ROOT" "$WIKI_DEPLOY_VARIANT"
      ;;

    secrets)
      parse_flags "$@"
      load_deploy_config
      resolve_optional WIKI_DEPLOY_HOST "$FLAG_TARGET" "${WIKI_DEPLOY_HOST:-}" "" "Deploy target (user@host; empty = local install)"
      resolve WIKI_DEPLOY_INSTALL_ROOT "" "${WIKI_DEPLOY_INSTALL_ROOT:-}" "/opt/wiki" "Install root"
      resolve WIKI_DEPLOY_SERVICE_USER "" "${WIKI_DEPLOY_SERVICE_USER:-}" "wiki" "Service user"
      resolve WIKI_DEPLOY_SSH_PORT "" "${WIKI_DEPLOY_SSH_PORT:-}" "22" "SSH port"
      resolve_optional WIKI_DEPLOY_VARIANT "$FLAG_VARIANT" "${WIKI_DEPLOY_VARIANT:-}" "" "Variant (blank if unknown — only checked to refuse container/container-arm)"
      cmd_configure_secrets "$WIKI_DEPLOY_INSTALL_ROOT" "$WIKI_DEPLOY_VARIANT"
      ;;

    systemd)
      local sub=${1:-}
      [ $# -gt 0 ] && shift
      parse_flags "$@"
      load_deploy_config
      resolve_optional WIKI_DEPLOY_HOST "$FLAG_TARGET" "${WIKI_DEPLOY_HOST:-}" "" "Deploy target (user@host; empty = local install)"
      resolve WIKI_DEPLOY_INSTALL_ROOT "" "${WIKI_DEPLOY_INSTALL_ROOT:-}" "/opt/wiki" "Install root"
      resolve WIKI_DEPLOY_SSH_PORT "" "${WIKI_DEPLOY_SSH_PORT:-}" "22" "SSH port"
      resolve_optional WIKI_DEPLOY_VARIANT "$FLAG_VARIANT" "${WIKI_DEPLOY_VARIANT:-}" "" "Variant (blank if unknown — only checked to skip the wiki.service part for container/container-arm)"
      case "$sub" in
        install) cmd_systemd_install "$FLAG_WITH_BACKUP_TIMER" "$WIKI_DEPLOY_INSTALL_ROOT" "$WIKI_DEPLOY_VARIANT" ;;
        status) cmd_systemd_status ;;
        *) die "systemd: subcommand must be 'install' or 'status'" ;;
      esac
      ;;

    nginx-config)
      local shape=${1:-}
      [ $# -gt 0 ] && shift
      parse_flags "$@"
      cmd_nginx_config "$shape" "$FLAG_DOMAIN" "$FLAG_BASE_PATH" "$FLAG_OUT"
      ;;

    config)
      local sub=${1:-}
      case "$sub" in
        init) cmd_config_init ;;
        show) cmd_config_show ;;
        *) die "config: subcommand must be 'init' or 'show'" ;;
      esac
      ;;

    "") usage ;;
    *) die "unknown command '$cmd' — run '$0 help'" ;;
  esac
}

main "$@"
