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
  : "${WIKI_DEPLOY_EMBEDDINGS_API_KEY_ENV:=}"
  : "${WIKI_DEPLOY_LLM_API_KEY_ENV:=}"
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
# SECTION: build — native / cross / container / cross-container / container-arm
# ============================================================

# platform_tag_suffix PLATFORM — "linux/arm/v7" -> "arm-v7", "linux/arm64"
# -> "arm64", "linux/amd64" -> "amd64". Used to tag the image so testing
# a non-default --platform never collides with the "arm" tag
# docker-compose.arm.yml/cmd_deploy_container expect for the real,
# documented armv7-on-a-Pi target.
platform_tag_suffix() {
  printf '%s' "$1" | sed -e 's#^linux/##' -e 's#/#-#'
}

cmd_build_native() {
  local skip_tests=$1
  run cmake -S . -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
  run cmake --build build -j"$(nproc)"
  if [ "$skip_tests" != 1 ]; then
    run ctest --test-dir build --output-on-failure
  fi
}

cmd_build_cross() {
  local triplet=$1 cross_dir=$2 skip_tests=$3
  local drogon_ctl="$SCRIPT_DIR/build/vcpkg_installed/x64-linux/tools/drogon/drogon_ctl"
  [ -x "$drogon_ctl" ] || die "native drogon_ctl not found at $drogon_ctl — run '$0 build native' first (classic-mode cross builds reuse the host's own drogon_ctl, see docs/sbc-deployment.md's Path B)"

  run bash -c "cd '$SCRIPT_DIR/vcpkg' && ./vcpkg install --classic --triplet '$triplet' \
    --overlay-triplets='$SCRIPT_DIR/$cross_dir' --overlay-ports='$SCRIPT_DIR/cross/overlay-ports' \
    --x-install-root='$SCRIPT_DIR/vcpkg_installed_arm' \
    drogon sqlite3[core,fts5,json1] libargon2 nlohmann-json md4c yaml-cpp tomlplusplus catch2"

  export PKG_CONFIG_LIBDIR="$SCRIPT_DIR/vcpkg_installed_arm/$triplet/lib/pkgconfig:$SCRIPT_DIR/vcpkg_installed_arm/$triplet/share/pkgconfig"
  export PKG_CONFIG_SYSROOT_DIR=""

  run cmake -S . -B build-arm -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$cross_dir/toolchain.cmake" \
    -DCMAKE_PREFIX_PATH="$SCRIPT_DIR/vcpkg_installed_arm/$triplet" \
    -DCMAKE_FIND_ROOT_PATH="$SCRIPT_DIR/vcpkg_installed_arm/$triplet" \
    -DDROGON_CTL_COMMAND="$drogon_ctl" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
  run cmake --build build-arm -j"$(nproc)"

  if [ "$skip_tests" != 1 ]; then
    log "cross build has no on-host test run — use 'wiki-ops.sh verify cross' (qemu-arm-static) instead"
  fi
}

cmd_build_container() {
  run docker compose build
}

cmd_build_cross_container() {
  local out_dir="$SCRIPT_DIR/build-arm-container"
  run docker build -f cross/Dockerfile.builder -t wiki-cross-builder .
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
  local platform=$1 tag
  if [ "$platform" = linux/arm/v7 ]; then
    # Matches docker-compose.arm.yml's own 'image: personal-wiki:arm' and
    # cmd_deploy_container's hardcoded tag for the documented armv7-on-a-Pi
    # target — keep this exact tag for the default platform so
    # `deploy --variant=container-arm` still finds the image it expects.
    tag="personal-wiki:arm"
  else
    tag="personal-wiki:$(platform_tag_suffix "$platform")"
    log "non-default platform ($platform) — tagged $tag, not personal-wiki:arm; deploy's container-arm path won't pick this up automatically"
  fi
  run docker buildx build --platform "$platform" -t "$tag" --load .
  log "built $tag"
}

# ============================================================
# SECTION: verify — qemu for bare-ARM binaries, docker run for images
# ============================================================

cmd_verify_cross() {
  local build_dir=$1 qemu_cpu=$2
  command -v qemu-arm-static >/dev/null 2>&1 || die "qemu-arm-static not found — install qemu-user-static"
  local unit_tests_bin="$build_dir/tests/unit_tests"
  [ -x "$unit_tests_bin" ] || unit_tests_bin="$build_dir/unit_tests"
  [ -x "$unit_tests_bin" ] || die "no unit_tests binary found under $build_dir"

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

  log "running unit_tests under qemu-arm-static — must report all cases passed, not just exit 0"
  qemu-arm-static "${qemu_cpu_args[@]}" "$unit_tests_bin"
  log "running wiki-server --create-admin under qemu-arm-static as a real smoke test"
  qemu-arm-static "${qemu_cpu_args[@]}" "$build_dir/wiki-server" --create-admin
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
  docker buildx inspect >/dev/null 2>&1 || die "no buildx builder available — 'docker buildx create --use' first"
  local tag="personal-wiki:$(platform_tag_suffix "$platform")-verify"
  docker build --platform "$platform" -t "$tag" --load .
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
  local with_backup_timer=$1 install_root=$2

  remote_sh "sudo cp '$install_root/share/wiki/systemd/wiki.service' /etc/systemd/system/ && sudo systemctl daemon-reload"

  local is_active
  is_active=$(remote_sh_capture "systemctl is-active wiki.service 2>/dev/null || true")
  if [ "$is_active" = active ]; then
    confirm_or_die "wiki.service is already running on $( [ -n "${WIKI_DEPLOY_HOST:-}" ] && echo "$WIKI_DEPLOY_HOST" || echo "this machine" ) — restart it now?"
    remote_sh "sudo systemctl restart wiki.service"
  else
    remote_sh "sudo systemctl enable --now wiki.service"
  fi
  remote_sh "sleep 1; sudo systemctl is-active wiki.service; curl -s -o /dev/null -w 'healthz: %{http_code}\n' http://127.0.0.1:8080/healthz"

  if [ "$with_backup_timer" = 1 ]; then
    remote_sh "sudo cp '$install_root'/share/wiki/systemd/wiki-backup.{service,timer} /etc/systemd/system/ && sudo mkdir -p /etc/opt/wiki"
    local has_env
    has_env=$(remote_sh_capture "[ -f /etc/opt/wiki/wiki-backup.env ] && echo yes || echo no")
    if [ "$has_env" = no ]; then
      remote_sh "sudo cp '$install_root/share/wiki/systemd/wiki-backup.env.example' /etc/opt/wiki/wiki-backup.env"
      log "wrote /etc/opt/wiki/wiki-backup.env from the example — edit BACKUP_DIR (a DIFFERENT disk/mount than the vault) before trusting this"
    fi
    remote_sh "sudo systemctl daemon-reload && sudo systemctl enable --now wiki-backup.timer"
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
  local listen_addr port threads vault_path mcp_scope base_path

  resolve listen_addr "" "" "127.0.0.1" "[server].listen_addr (keep on loopback behind a reverse proxy)"
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
}

# ============================================================
# SECTION: first-time admin account (real interactive TTY, never scripted)
# ============================================================

cmd_setup_admin() {
  local install_root=$1
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
  local install_root=$1
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
# the dev machine's own `strip` can't read a cross ARM ELF).
deploy_swap_tree() {
  local staging=$1 install_root=$2 strip_bins=$3
  local user=$WIKI_DEPLOY_SERVICE_USER

  ensure_remote_user "$user" "$install_root"

  local remote_staging="/tmp/wiki-ops-deploy-$$"
  remote_sh "rm -rf '$remote_staging'"
  copy_to_target "$staging" "$remote_staging"

  local strip_cmd=""
  if [ "$strip_bins" = 1 ]; then
    strip_cmd="sudo strip --strip-all '$remote_staging/bin/wiki-server' '$remote_staging/bin/wiki-mcp' &&"
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
sudo chown -R '$user':'$user' '$install_root'; \
rm -rf '$remote_staging'"
}

cmd_deploy_container() {
  local variant=$1 install_root=$2 host_port=$3
  local extra_compose=()
  local image_tag=personal-wiki:latest
  if [ "$variant" = container-arm ]; then
    extra_compose=(-f docker-compose.arm.yml)
    image_tag=personal-wiki:arm
  fi

  if [ -z "${WIKI_DEPLOY_HOST:-}" ]; then
    if [ -n "$host_port" ]; then
      export WIKI_HOST_PORT="$host_port"
      log "WIKI_HOST_PORT=$host_port exported for docker compose (container's own internal port stays 8080)"
    fi
    run docker compose -f docker-compose.yml "${extra_compose[@]}" up -d
    return
  fi

  local tmp_image remote_image
  tmp_image=$(mktemp /tmp/wiki-ops-image-XXXXXX.tar)
  run bash -c "docker save '$image_tag' -o '$tmp_image'"
  remote_image="/tmp/$(basename "$tmp_image")"
  copy_to_target "$tmp_image" "$remote_image"
  remote_sh "docker load -i '$remote_image' && rm -f '$remote_image'"
  rm -f "$tmp_image"

  copy_to_target docker-compose.yml "$install_root/docker-compose.yml"
  if [ "$variant" = container-arm ]; then
    copy_to_target docker-compose.arm.yml "$install_root/docker-compose.arm.yml"
  fi
  local compose_flags="-f docker-compose.yml"
  [ "$variant" = container-arm ] && compose_flags="-f docker-compose.yml -f docker-compose.arm.yml"
  local port_env=""
  [ -n "$host_port" ] && port_env="WIKI_HOST_PORT='$host_port' "
  remote_sh "mkdir -p '$install_root/vault_data' && cd '$install_root' && ${port_env}docker compose $compose_flags up -d"
}

cmd_deploy() {
  local flag_target=$1 flag_variant=$2 first_time=$3 with_backup_timer=$4 static_only=$5 skip_verify=$6 qemu_cpu=$7 port_flag=$8

  resolve_optional WIKI_DEPLOY_HOST "$flag_target" "${WIKI_DEPLOY_HOST:-}" "" "Deploy target (user@host; empty = local install)"
  resolve WIKI_DEPLOY_VARIANT "$flag_variant" "${WIKI_DEPLOY_VARIANT:-}" "cross" "Build variant to deploy (native|cross|container|cross-container|container-arm)"
  resolve WIKI_DEPLOY_INSTALL_ROOT "" "${WIKI_DEPLOY_INSTALL_ROOT:-}" "/opt/wiki" "Install root"
  resolve WIKI_DEPLOY_SERVICE_USER "" "${WIKI_DEPLOY_SERVICE_USER:-}" "wiki" "Service user"
  resolve WIKI_DEPLOY_SSH_PORT "" "${WIKI_DEPLOY_SSH_PORT:-}" "22" "SSH port"

  if [ "$static_only" = 1 ]; then
    cmd_static_redeploy "$WIKI_DEPLOY_INSTALL_ROOT"
    return
  fi

  case "$WIKI_DEPLOY_VARIANT" in
    native|cross|cross-container) ;;
    container|container-arm)
      resolve_optional WIKI_DEPLOY_PORT "$port_flag" "${WIKI_DEPLOY_PORT:-}" "" "Host-side port (blank = compose default, 8080)"
      cmd_deploy_container "$WIKI_DEPLOY_VARIANT" "$WIKI_DEPLOY_INSTALL_ROOT" "$WIKI_DEPLOY_PORT"
      cmd_systemd_install "$with_backup_timer" "$WIKI_DEPLOY_INSTALL_ROOT"
      return
      ;;
    *) die "deploy: unknown variant '$WIKI_DEPLOY_VARIANT'" ;;
  esac

  local build_dir strip_bins=0
  case "$WIKI_DEPLOY_VARIANT" in
    native) build_dir=build ;;
    cross) build_dir=build-arm; strip_bins=1 ;;
    cross-container) build_dir=build-arm-container; strip_bins=1 ;;
  esac
  [ -d "$build_dir" ] || die "no $build_dir/ found — run 'build $WIKI_DEPLOY_VARIANT' first"

  if [ "$skip_verify" != 1 ] && [ "$strip_bins" = 1 ]; then
    cmd_verify_cross "$build_dir" "$qemu_cpu"
  fi

  local staging
  staging=$(mktemp -d /tmp/wiki-ops-stage-XXXXXX)
  if [ "$WIKI_DEPLOY_VARIANT" = native ]; then
    run cmake --install "$build_dir" --prefix "$staging"
  else
    stage_bare_binary_tree "$build_dir" "$staging"
  fi

  deploy_swap_tree "$staging" "$WIKI_DEPLOY_INSTALL_ROOT" "$strip_bins"
  rm -rf "$staging"

  if [ "$first_time" = 1 ]; then
    cmd_configure_toml "$WIKI_DEPLOY_INSTALL_ROOT" "$port_flag"
  fi

  cmd_systemd_install "$with_backup_timer" "$WIKI_DEPLOY_INSTALL_ROOT"

  if [ "$first_time" = 1 ]; then
    cmd_setup_admin "$WIKI_DEPLOY_INSTALL_ROOT"
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

  build   native|cross|container|cross-container|container-arm [--skip-tests]
          [--triplet=NAME] [--cross-dir=PATH] [--platform=linux/arm/v7] [--dry-run]
  verify  cross|container|cross-container|container-arm
          [--platform=linux/arm/v7] [--qemu-cpu=NAME]
  deploy  [--target=HOST] [--variant=V] [--first-time] [--with-backup-timer]
          [--static-only] [--skip-verify] [--qemu-cpu=NAME] [--port=N]
          [--yes] [--dry-run]
  static-redeploy [--target=HOST] [--dry-run]
  setup-admin [--target=HOST]
  systemd install [--target=HOST] [--with-backup-timer] [--yes] [--dry-run]
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
FLAG_TRIPLET="arm-musl"
FLAG_CROSS_DIR="cross/arm-musl"
FLAG_PLATFORM=""
FLAG_QEMU_CPU=""
FLAG_PORT=""
FLAG_SKIP_TESTS=0
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
      case "$sub" in
        native) cmd_build_native "$FLAG_SKIP_TESTS" ;;
        cross) cmd_build_cross "$FLAG_TRIPLET" "$FLAG_CROSS_DIR" "$FLAG_SKIP_TESTS" ;;
        container) cmd_build_container ;;
        cross-container) cmd_build_cross_container ;;
        container-arm) cmd_build_container_arm "${FLAG_PLATFORM:-linux/arm/v7}" ;;
        *) die "build: variant must be one of native|cross|container|cross-container|container-arm" ;;
      esac
      ;;

    verify)
      local sub=${1:-}
      [ $# -gt 0 ] && shift
      parse_flags "$@"
      case "$sub" in
        cross) cmd_verify_cross build-arm "$FLAG_QEMU_CPU" ;;
        cross-container) cmd_verify_cross build-arm-container "$FLAG_QEMU_CPU" ;;
        container) cmd_verify_container ;;
        container-arm) cmd_verify_container_arm "${FLAG_PLATFORM:-linux/arm/v7}" ;;
        *) die "verify: variant must be one of cross|cross-container|container|container-arm" ;;
      esac
      ;;

    deploy)
      parse_flags "$@"
      load_deploy_config
      cmd_deploy "$FLAG_TARGET" "$FLAG_VARIANT" "$FLAG_FIRST_TIME" "$FLAG_WITH_BACKUP_TIMER" "$FLAG_STATIC_ONLY" "$FLAG_SKIP_VERIFY" "$FLAG_QEMU_CPU" "$FLAG_PORT"
      ;;

    static-redeploy)
      parse_flags "$@"
      load_deploy_config
      resolve_optional WIKI_DEPLOY_HOST "$FLAG_TARGET" "${WIKI_DEPLOY_HOST:-}" "" "Deploy target (user@host; empty = local install)"
      resolve WIKI_DEPLOY_INSTALL_ROOT "" "${WIKI_DEPLOY_INSTALL_ROOT:-}" "/opt/wiki" "Install root"
      resolve WIKI_DEPLOY_SERVICE_USER "" "${WIKI_DEPLOY_SERVICE_USER:-}" "wiki" "Service user"
      resolve WIKI_DEPLOY_SSH_PORT "" "${WIKI_DEPLOY_SSH_PORT:-}" "22" "SSH port"
      cmd_static_redeploy "$WIKI_DEPLOY_INSTALL_ROOT"
      ;;

    setup-admin)
      parse_flags "$@"
      load_deploy_config
      resolve_optional WIKI_DEPLOY_HOST "$FLAG_TARGET" "${WIKI_DEPLOY_HOST:-}" "" "Deploy target (user@host; empty = local install)"
      resolve WIKI_DEPLOY_INSTALL_ROOT "" "${WIKI_DEPLOY_INSTALL_ROOT:-}" "/opt/wiki" "Install root"
      resolve WIKI_DEPLOY_SERVICE_USER "" "${WIKI_DEPLOY_SERVICE_USER:-}" "wiki" "Service user"
      resolve WIKI_DEPLOY_SSH_PORT "" "${WIKI_DEPLOY_SSH_PORT:-}" "22" "SSH port"
      cmd_setup_admin "$WIKI_DEPLOY_INSTALL_ROOT"
      ;;

    systemd)
      local sub=${1:-}
      [ $# -gt 0 ] && shift
      parse_flags "$@"
      load_deploy_config
      resolve_optional WIKI_DEPLOY_HOST "$FLAG_TARGET" "${WIKI_DEPLOY_HOST:-}" "" "Deploy target (user@host; empty = local install)"
      resolve WIKI_DEPLOY_INSTALL_ROOT "" "${WIKI_DEPLOY_INSTALL_ROOT:-}" "/opt/wiki" "Install root"
      resolve WIKI_DEPLOY_SSH_PORT "" "${WIKI_DEPLOY_SSH_PORT:-}" "22" "SSH port"
      case "$sub" in
        install) cmd_systemd_install "$FLAG_WITH_BACKUP_TIMER" "$WIKI_DEPLOY_INSTALL_ROOT" ;;
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
