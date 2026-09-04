# Deployment

## Real-hardware verification status

**Honestly, no sugar-coating**: a full native build (`cmake --build` from scratch, not
cross-compilation) has been verified on x86_64 Linux (Arch) only — `cmake --install`,
running the installed tree, a full security/E2E pass (`ctest`), a live VaultWatcher.
**A native build on the Raspberry Pi itself (compiling right on the device, as
described below under "Build") has not been done yet** — the dev environment has no
time to run an hours-long Drogon+OpenSSL build on a weak SBC every time.

Instead, **cross-compiled binaries HAVE actually been deployed to and verified on a
live target device** — armv7l, Debian 9 (stretch, EOL, glibc 2.24) — via
`arm-linux-musleabihf`+musl+static (see "Cross-compilation" below): `wiki-server` and
`wiki-mcp` running NATIVELY (not emulated) on the real device, `unit_tests` passing
under `qemu-arm-static` (112 assertions, 52 test cases), the full
login → CSRF → document creation → atomic disk write → FTS5 search with snippet
highlighting cycle verified with live HTTP traffic against a real systemd unit
(`ProtectSystem=strict` and the rest of the hardening below included), alongside live
nginx/Samba/NFS/ProFTPD/mosquitto/munin, with zero impact on any of them. The code
contains nothing deliberately x86-specific, and this is now empirically confirmed, not
just "should work by construction".

## Live production target

**The actual, currently-running instance — record this here, not just in a chat
session, or it gets lost the next time context gets compacted (confirmed the hard
way: happened once already).**

- Host: `root@192.0.2.10` (`httpd.wiki.example.com`, armv7l/sunxi, Debian 9
  stretch — the same real hardware "Real-hardware verification status" above
  describes).
- Install root: `/opt/wiki` (`bin/`, `static/`, `config.toml`, `vault_data/`).
- Public URL: `http://192.0.2.10/wiki/` (nginx `default` vhost, prefix-stripping
  `proxy_pass` to `127.0.0.1:8080` — see "Reverse-proxying under a subpath" below;
  `[server].base_path = "/wiki"` in `/opt/wiki/config.toml` is what makes the app
  itself aware of that prefix).
- systemd unit: `wiki.service`, `User=wiki`.

This target's own C toolchain/distro age is exactly why binaries reach it
cross-compiled from the dev machine (see "Cross-compilation" below), never
via a native `cmake --install` run on the device itself — the "Update" recipe
right below this section describes that native-build path for a *hypothetical*
device capable of it, not this one.

**Actual redeploy recipe used against this target** (backend OR frontend changes
— cross-compilation always produces both binaries, cheap enough not to special-case
static-only changes):

```sh
# 1. incremental cross-build (see "Cross-compilation" below for a from-scratch
#    setup — vcpkg_installed_arm/ and build-arm/ are both reused, not
#    recreated, on every subsequent deploy)
export PKG_CONFIG_LIBDIR="$PWD/vcpkg_installed_arm/arm-musl/lib/pkgconfig:$PWD/vcpkg_installed_arm/arm-musl/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=""
cmake --build build-arm -j"$(nproc)"
qemu-arm-static ./build-arm/tests/unit_tests   # must pass every case before shipping

# 2. ship the new binaries + static assets alongside the live ones (never
#    directly overwrite in place — a scp that dies mid-transfer must not
#    leave a half-written binary where systemd will find it on next restart)
scp build-arm/wiki-server build-arm/wiki-mcp root@192.0.2.10:/tmp/
scp -r static root@192.0.2.10:/tmp/static-new

# 3. on the target: back up what's live, swap the new files in, restart, verify
ssh root@192.0.2.10 '
  set -e
  STAMP=$(date +%Y%m%d-%H%M%S)
  cp /opt/wiki/bin/wiki-server /opt/wiki/bin/wiki-server.bak-$STAMP
  cp /opt/wiki/bin/wiki-mcp /opt/wiki/bin/wiki-mcp.bak-$STAMP
  mv /opt/wiki/static /opt/wiki/static.bak-$STAMP
  chmod +x /tmp/wiki-server /tmp/wiki-mcp
  mv /tmp/wiki-server /opt/wiki/bin/wiki-server
  mv /tmp/wiki-mcp /opt/wiki/bin/wiki-mcp
  mv /tmp/static-new /opt/wiki/static
  chown -R wiki:wiki /opt/wiki/bin /opt/wiki/static
  systemctl restart wiki.service
  sleep 1
  systemctl is-active wiki.service
  curl -s -o /dev/null -w "healthz: %{http_code}\n" http://127.0.0.1:8080/healthz
'
```

The `.bak-$STAMP` copies are never cleaned up automatically — `/opt/wiki` on a
long-lived deployment accumulates them; sweep old ones by hand occasionally.

## Prerequisites (on the target device — Raspberry Pi or another Linux/ARM64/x86_64 SBC)

- A C++20-capable GCC (verified on GCC 16.2.1; GCC has had C++20 since version 10, but
  newer means fewer surprises)
- CMake ≥ 3.21, Ninja
- git, curl
- Python 3 (for the `ctest` integration security run; not needed at runtime)
- A Linux kernel with inotify enabled (standard on any modern distro)

## Build (native, on the device itself)

```sh
git clone <repo-url> wiki && cd wiki

# vcpkg is not vendored, cloned separately (see CLAUDE.md)
git clone --depth 1 https://github.com/microsoft/vcpkg.git vcpkg
./vcpkg/bootstrap-vcpkg.sh -disableMetrics

cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"

# security/E2E pass before trusting the build (auth, CSRF, path
# traversal, session fixation, visibility gating, VaultWatcher — see
# tests/integration/security_e2e.py)
ctest --test-dir build --output-on-failure
```

On a weak SBC this can take substantially longer than on a desktop
(Drogon+OpenSSL+trantor from scratch isn't fast); it's a one-time cost.

## Install

```sh
sudo cmake --install build --prefix /opt/wiki
```

Places `bin/wiki-server`, `bin/wiki-mcp`, `static/`, `config.example.toml`, and
`share/wiki/systemd/{wiki.service,wiki.env.example}` under `/opt/wiki`. Does **not**
create a working `config.toml` — only the example (deliberately: a real config should
never silently materialize from defaults).

## First-time setup

```sh
# a dedicated unprivileged user/group — the unit file expects exactly these
sudo useradd --system --home-dir /opt/wiki --shell /usr/sbin/nologin wiki
sudo chown -R wiki:wiki /opt/wiki

cd /opt/wiki
sudo -u wiki cp config.example.toml config.toml
# edit as needed: [server].port, [server].base_path, [vault].path, [mcp].scope

# create the admin account (password entered interactively, echo disabled)
sudo -u wiki ./bin/wiki-server --create-admin
```

## systemd

```sh
sudo cp /opt/wiki/share/wiki/systemd/wiki.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now wiki.service
sudo systemctl status wiki.service
```

The unit already ships with hardening (`ProtectSystem=strict`, `NoNewPrivileges=yes`,
`ReadWritePaths=/opt/wiki/vault_data` — the only place the service actually writes).
`EnvironmentFile=-/etc/wiki/wiki.env` is optional — right now no environment variable
is read by the app at all (admin credentials live in SQLite, sessions are random
tokens with no secret-based signature); the file stays as a documented hook for the
future (e.g. a bearer token for a remote MCP transport in Phase 2).

## TLS / public internet access

`wiki-server` doesn't terminate TLS itself. For access outside the local network, put a
reverse proxy (nginx/Caddy/traefik) in front of it to handle TLS and proxy to
`127.0.0.1:8080` (or whatever `config.toml` specifies). Without that step, keep
`listen_addr = "127.0.0.1"` and don't expose the port directly.

## Backup

The source of truth is `[vault].path` (the directory of `.md` files plus `.trash/`).
The SQLite index (`[index].db_path`) is fully disposable and gets rebuilt by
`wiki-server --reindex` — backing it up isn't required, but doesn't hurt either
(faster to restore than a full rescan from scratch on a very large vault). Minimum:
back up `[vault].path` regularly.

## Update

```sh
cd wiki && git pull
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure   # before restarting prod
sudo cmake --install build --prefix /opt/wiki
sudo systemctl restart wiki.service
```

## Cross-compilation (armv7, musl, static) — for an old/weak target

When a native build on the device itself is impractical (an old distro with no modern
compiler, or just not worth an hours-long Drogon+OpenSSL build on a weak SBC) —
cross-compile from an x86_64 dev machine via [zig](https://ziglang.org/)
(`zig cc`/`zig c++`) as a self-contained C/C++ cross-compiler with a bundled musl libc +
libc++, fully static linking (`-static`). Why musl+static rather than a glibc
cross-toolchain: an old target (e.g. Debian 9 stretch, glibc 2.24 from 2016) would
break at runtime (`GLIBC_2.XX not found`) against any modern glibc cross-toolchain; a
fully static musl binary doesn't touch the target's glibc at all.

```sh
# 1. cross-build the dependencies via vcpkg (classic mode — drogon[ctl]
#    isn't supported on a cross target, so the ctl feature is skipped; an
#    already-built x64-linux drogon_ctl is passed in separately below)
cd vcpkg
./vcpkg install --classic --triplet arm-musl \
  --overlay-triplets=../cross/arm-musl --overlay-ports=../cross/overlay-ports \
  --x-install-root=../vcpkg_installed_arm \
  drogon sqlite3[core,fts5,json1] libargon2 nlohmann-json md4c yaml-cpp \
  tomlplusplus catch2
cd ..

# 2. configure+build the project against the cross-installed prefix
export PKG_CONFIG_LIBDIR="$PWD/vcpkg_installed_arm/arm-musl/lib/pkgconfig:$PWD/vcpkg_installed_arm/arm-musl/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=""
cmake -S . -B build-arm -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cross/arm-musl/toolchain.cmake \
  -DCMAKE_PREFIX_PATH=$PWD/vcpkg_installed_arm/arm-musl \
  -DCMAKE_FIND_ROOT_PATH=$PWD/vcpkg_installed_arm/arm-musl \
  -DDROGON_CTL_COMMAND=$PWD/build/vcpkg_installed/x64-linux/tools/drogon/drogon_ctl \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-arm -j"$(nproc)"

# 3. verify BEFORE shipping to real hardware — under qemu user-mode
qemu-arm-static ./build-arm/tests/unit_tests   # must pass every case, not just not-crash
qemu-arm-static ./build-arm/wiki-server --create-admin   # smoke test: a real binary, not just "it compiled"
```

**Known zig 0.16.0 bug**: linking many static `.a` archives for
`arm-linux-musleabihf` SIGSEGVs (`code=139`) in the bundled lld — but not because of
archive count or parallelism (both hypotheses were tested and ruled out), but because
of a specific flag: CMake (Ninja generator, ≥3.20) automatically adds
`-Xlinker --dependency-file=...` for linker-level dependency tracking, and that exact
flag crashes lld for this target deterministically, 100% of the time. Fix:
`set(CMAKE_LINK_DEPENDS_USE_LINKER OFF)` in `cross/arm-musl/toolchain.cmake` (CMake
falls back to non-linker-based dependency tracking on its own). Two smaller overlay
ports (`cross/overlay-ports/{brotli,libuuid}`) disable building their CLI/test
binaries for the same reason (linking a small executable against a single `.a` also
crashed).

**nginx reverse proxy right next to an nginx that's already live on the target**:
`wiki-server` deliberately listens only on `127.0.0.1:8080` (see `config.toml`), it
doesn't terminate TLS itself — adding a dedicated `server{}` block to the existing
nginx (a new subdomain or a `location`) is left to the administrator by hand, it does
NOT touch any existing nginx configuration automatically.

## Reverse-proxying under a subpath (e.g. `/wiki`)

When the app needs to be exposed not on its own (sub)domain but under a path on an
already-existing site (`https://example.com/wiki/`) — the app itself generates EVERY
`href`/`action`/`hx-get`/redirect Location/saved path in its own JS as an absolute path
rooted at "/" (`/login`, `/d/...`, `/css/...`). Without some agreement with the proxy
about the prefix, this would look clean on the `/wiki/` page itself but break
everything that page links to (CSS/JS 404s, the login form posting to `/login` instead
of `/wiki/login`, and so on).

**The correct fix is `[server].base_path` in `config.toml`**, not hacks on the nginx
side:

```toml
[server]
base_path = "/wiki"
```

Once set, the app itself bakes the prefix into EVERY `href`/`action`/`hx-*`/redirect/JS
path it generates (login/view/edit/search pages, `edit.js`, the `EditPage`/`SearchPage`
CSP templates) — the routes themselves stay unchanged (`/login`, `/d/{path}`, ...),
because the proxy strips the prefix BEFORE forwarding the request inward. That means
the nginx side is a plain prefix-stripping `proxy_pass`, with no response-body or
header rewriting at all:

```nginx
location = /wiki {
    return 301 /wiki/;
}

location /wiki/ {
    proxy_pass http://127.0.0.1:8080/;
    proxy_set_header Host $host;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto $scheme;
}
```

No `sub_filter`, no `proxy_redirect`, no `Accept-Encoding ""` hack — all of those were
necessary crutches BEFORE `base_path` existed in the app itself (the first working
version of this exact deployment actually went live through them); `base_path` removes
the need for them completely and permanently, rather than patching the symptom on the
proxy side over and over.
