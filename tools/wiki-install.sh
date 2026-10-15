#!/bin/sh
# Standalone installer for a GitHub Release build of this wiki — runs
# directly ON the target device (a Raspberry Pi or any other Linux box)
# with nothing beyond curl/tar/sha256sum/sudo. No git checkout, no
# tools/wiki-ops.sh, no SSH from a dev machine.
#
# Deliberately duplicates a small slice of tools/wiki-ops.sh's own
# deploy_swap_tree/cmd_systemd_install logic rather than depending on it —
# the entire point of this script is to need nothing but itself; shipping
# wiki-ops.sh's ~1400 lines (with its git-repo-relative path assumptions)
# onto a fresh device would defeat that. See docs/sbc-deployment.md's
# "Path C" for the full picture, including the dev-machine equivalent
# (`wiki-ops.sh deploy --variant=release`).
#
# Read this script before running it as root/sudo — don't curl-pipe it
# blind. Usage:
#   ./wiki-install.sh [--version=vX.Y.Z] [--install-root=/opt/wiki]
#                      [--yes] [--skip-admin]

set -eu

REPO_URL="https://github.com/ashevchuk/personal-wiki"
VERSION=""
INSTALL_ROOT=/opt/wiki
ASSUME_YES=0
SKIP_ADMIN=0

log() { echo "wiki-install: $*" >&2; }
die() { echo "wiki-install: error: $*" >&2; exit 1; }

for arg in "$@"; do
  case "$arg" in
    --version=*) VERSION=${arg#*=} ;;
    --install-root=*) INSTALL_ROOT=${arg#*=} ;;
    --yes) ASSUME_YES=1 ;;
    --skip-admin) SKIP_ADMIN=1 ;;
    *) die "unrecognized flag: $arg" ;;
  esac
done

# 1. Architecture -> release triplet. uname -m, not /proc/cpuinfo — it
# already distinguishes armv6l (Pi 1/Zero/Zero W) from armv7l (Pi 2/3
# armhf) on every Raspbian/Debian build actually shipped for those
# boards. Anything unrecognized is a hard die, never a guess — see
# cross/*/toolchain.cmake for why armv6 vs armv7 vs aarch64 actually
# matters (an ARMv7 binary SIGILLs on a true ARMv6 core, a 32-bit binary
# can't even exec() on a CONFIG_COMPAT-less 64-bit kernel).
machine=$(uname -m)
case "$machine" in
  armv6l) triplet=armv6-musl ;;
  armv7l) triplet=arm-musl ;;
  aarch64 | arm64) triplet=aarch64-musl ;;
  x86_64) triplet=x64-musl ;;
  *) die "unsupported architecture '$machine' — no release build exists for it" ;;
esac
log "detected architecture $machine -> $triplet"

command -v curl >/dev/null 2>&1 || die "curl is required"
command -v sha256sum >/dev/null 2>&1 || die "sha256sum is required"
command -v sudo >/dev/null 2>&1 || die "sudo is required"

# 2. Resolve the release tag. Blank means "whatever GitHub currently
# calls latest" — resolved from the redirect target of /releases/latest,
# and validated to actually look like a release tag before being used
# for anything; a curl failure or an unexpected redirect must never read
# as "use nothing," it must die loudly here.
if [ -z "$VERSION" ]; then
  VERSION=$(curl -fsSL -o /dev/null -w '%{url_effective}' "$REPO_URL/releases/latest" | sed -E 's#.*/tag/##')
fi
case "$VERSION" in
  v[0-9]*.[0-9]*.[0-9]*) ;;
  *) die "release tag must look like vX.Y.Z (got '$VERSION') — pass --version=vX.Y.Z if latest-release resolution failed" ;;
esac

asset="wiki-${VERSION#v}-$triplet.tar.gz"
log "release $VERSION, asset $asset"

# 3. Download + verify. Never 2>/dev/null, never trust a bare "curl
# succeeded" — the checksum check is the actual validation, and a
# mismatch is always a hard die, not a warning.
staging=""
dl_dir=$(mktemp -d /tmp/wiki-install-XXXXXX)
trap 'rm -rf "$dl_dir" "$staging"' EXIT
curl -fsSL -o "$dl_dir/$asset" "$REPO_URL/releases/download/$VERSION/$asset"
curl -fsSL -o "$dl_dir/SHA256SUMS" "$REPO_URL/releases/download/$VERSION/SHA256SUMS"
if ! (cd "$dl_dir" && grep " $asset\$" SHA256SUMS | sha256sum -c -); then
  die "checksum mismatch for $asset against $VERSION's SHA256SUMS — refusing to install a corrupted or tampered download"
fi

# 4. Extract to a staging dir, never straight over a live install.
staging=$(mktemp -d /tmp/wiki-install-stage-XXXXXX)
tar -C "$staging" -xzf "$dl_dir/$asset"

# 5. Confirm before touching an existing install.
if [ -d "$INSTALL_ROOT/bin" ] && [ "$ASSUME_YES" != 1 ]; then
  printf 'wiki-install: %s already has a bin/ — this will back it up and replace it. Continue? [y/N] ' "$INSTALL_ROOT"
  read -r reply
  case "$reply" in
    y | Y | yes | YES) ;;
    *) die "aborted" ;;
  esac
fi

# 6. System user, same shape as tools/wiki-ops.sh's ensure_remote_user.
if ! id wiki >/dev/null 2>&1; then
  sudo useradd --system --home-dir "$INSTALL_ROOT" --shell /usr/sbin/nologin wiki
fi

# 7. Atomic swap — mirrors tools/wiki-ops.sh's deploy_swap_tree exactly:
# .bak-$STAMP the live bin/+static/, strip ONLY if this host actually has
# a `strip` that works (never assumed — a minimal image with none at all
# is a documented real case here, not hypothetical), move the new ones
# in, chown, and create vault_data BEFORE wiki.service ever starts (its
# ReadWritePaths= mount-namespace setup fails outright, code=226/
# NAMESPACE, if that directory doesn't already exist).
stamp=$(date +%Y%m%d-%H%M%S)
sudo mkdir -p "$INSTALL_ROOT"
if [ -d "$INSTALL_ROOT/bin" ]; then
  sudo cp -r "$INSTALL_ROOT/bin" "$INSTALL_ROOT/bin.bak-$stamp"
fi
if [ -d "$INSTALL_ROOT/static" ]; then
  sudo mv "$INSTALL_ROOT/static" "$INSTALL_ROOT/static.bak-$stamp"
fi
sudo chmod +x "$staging/bin/wiki-server" "$staging/bin/wiki-mcp" "$staging/bin/wiki-backup.sh"
if command -v strip >/dev/null 2>&1; then
  sudo strip --strip-all "$staging/bin/wiki-server" "$staging/bin/wiki-mcp"
else
  log "strip not found on this host -- shipping unstripped binaries"
fi
sudo rm -rf "$INSTALL_ROOT/bin"
sudo mv "$staging/bin" "$INSTALL_ROOT/bin"
sudo mv "$staging/static" "$INSTALL_ROOT/static"
[ -f "$INSTALL_ROOT/config.toml" ] || sudo cp "$staging/config.example.toml" "$INSTALL_ROOT/config.toml"
sudo cp "$staging/config.example.toml" "$INSTALL_ROOT/config.example.toml"
sudo mkdir -p "$INSTALL_ROOT/share/wiki/systemd"
sudo cp "$staging"/share/wiki/systemd/* "$INSTALL_ROOT/share/wiki/systemd/"
sudo mkdir -p "$INSTALL_ROOT/vault_data"
sudo chown -R wiki:wiki "$INSTALL_ROOT"
log "installed into $INSTALL_ROOT/"

# 8. systemd unit — the shipped unit hardcodes /opt/wiki, so a
# non-default INSTALL_ROOT needs a substitution, not a plain cp.
# Anchored on "=/opt/wiki" (right after the "="), not a bare "/opt/wiki"
# — the unit's EnvironmentFile=-/etc/opt/wiki/wiki.env line contains
# "/opt/wiki" too, as a substring of the unrelated FHS /etc/opt/<app>/
# convention path, which must NOT track a custom install root at all.
# A bare global replace there silently rewrites it to
# /etc/$INSTALL_ROOT/wiki.env — a path that doesn't exist, so the
# service starts with none of its [embeddings]/[llm] secrets loaded,
# with no error (EnvironmentFile='-' means a missing file is not fatal).
sed "s|=/opt/wiki|=$INSTALL_ROOT|g" "$INSTALL_ROOT/share/wiki/systemd/wiki.service" \
  | sudo tee /etc/systemd/system/wiki.service >/dev/null
sudo systemctl daemon-reload
sudo systemctl enable --now wiki.service
log "wiki.service enabled and started"

if [ "$SKIP_ADMIN" != 1 ]; then
  log "creating the admin account (interactive — password entry has echo disabled)"
  sudo -u wiki "$INSTALL_ROOT/bin/wiki-server" --create-admin
fi

cat >&2 <<EOF

wiki-install: done.
  - Edit $INSTALL_ROOT/config.toml (listen_addr/port/vault path/etc.), then
    sudo systemctl restart wiki.service to pick it up.
  - Check it's actually up: sudo systemctl status wiki.service
  - Full reference: docs/sbc-deployment.md in the project repo.
EOF
