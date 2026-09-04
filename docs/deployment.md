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
under `qemu-arm-static` (315 assertions, 117 test cases — re-verified 2026-09-04; this
number only ever grows as features get their own tests, don't be alarmed if it's
higher again by the time you read this, be alarmed if it's LOWER), the full
login → CSRF → document creation → atomic disk write → FTS5 search with snippet
highlighting cycle verified with live HTTP traffic against a real systemd unit
(`ProtectSystem=strict` and the rest of the hardening below included), alongside live
nginx/Samba/NFS/ProFTPD/mosquitto/munin, with zero impact on any of them. The code
contains nothing deliberately x86-specific, and this is now empirically confirmed, not
just "should work by construction".

## Recording your own live deployment target

Worth writing down somewhere durable — a private note, not this public repo — once
you have a real instance running: the host/IP, the install root, the public URL (and
whether it's reverse-proxied under a subpath — see below), and which systemd unit/user
runs it. Losing track of "which box is actually live" between sessions is a real
failure mode, not a hypothetical one — this section used to name a specific real host
here until the repo went public; that level of detail belongs in your own private
ops notes, not a public git history. Example shape, filled with placeholder values
(RFC 5737 documentation IP, `.example.com`):

- Host: `root@192.0.2.10` (`wiki.example.com`, armv7l/sunxi, Debian 9 stretch).
- Install root: `/opt/wiki` (`bin/`, `static/`, `config.toml`, `vault_data/`).
- Public URL: `http://192.0.2.10/wiki/` (nginx `default` vhost, prefix-stripping
  `proxy_pass` to `127.0.0.1:8080` — see "Reverse-proxying under a subpath" below;
  whether `[server].base_path` is set on this instance is worth checking directly
  rather than assumed from this doc — it's optional, closes one specific edge case).
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
git clone https://github.com/ashevchuk/personal-wiki.git wiki && cd wiki

# vcpkg is not vendored, cloned separately. FULL clone, not --depth 1 —
# see docs/architecture.md's "Build" section for why a shallow clone can
# silently miss vcpkg.json's pinned baseline commit.
git clone https://github.com/microsoft/vcpkg.git vcpkg
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
# edit as needed: [server].port, [vault].path, [mcp].scope
# [server].base_path is optional — see "Reverse-proxying under a subpath" below

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
back up `[vault].path` regularly. Two ways to actually do that:

**Ad hoc, from the Web UI**: Account page (admin only) → "Download backup" — hits
`GET /api/admin/backup` (`src/vault/BackupService.h`), which shells out to the
system `tar` binary (never `system()`/`popen()` — `fork()`+`execlp()` with an
explicit argv, so nothing about the vault path's own content can be interpreted as
shell syntax) and streams back a `.tar.gz` of the whole vault, `.trash/` and the
index db included, `.uploads-tmp/` (Drogon's own transient upload-staging buffer,
never real content) excluded. Good for "grab a snapshot right now before I do
something risky"; not a substitute for the automated path below — it only helps if
the server and its disk are both still alive, which is exactly the case a real
disaster (dead SD card) is not.

**Automated, via systemd timer** (`systemd/wiki-backup.{service,timer}`,
`systemd/wiki-backup.sh`) — NOT installed/enabled by a plain `cmake --install`;
opt in explicitly:

```sh
sudo cp /opt/wiki/share/wiki/systemd/wiki-backup.{service,timer} /etc/systemd/system/
sudo mkdir -p /etc/wiki
sudo cp /opt/wiki/share/wiki/systemd/wiki-backup.env.example /etc/wiki/wiki-backup.env
sudo "$EDITOR" /etc/wiki/wiki-backup.env   # set BACKUP_DIR to a DIFFERENT disk/mount, see below
sudo systemctl daemon-reload
sudo systemctl enable --now wiki-backup.timer
```

`wiki-backup.sh` tars `VAULT_PATH` straight off disk (same `.uploads-tmp/`
exclusion as the Web UI button, same atomic temp-file-then-rename discipline as
`VaultRepository`'s own document writes — a run that dies partway through never
leaves a truncated file under the real `wiki-backup-*.tar.gz` name), then prunes
down to `RETENTION_COUNT` (default 14), oldest first, only after a new backup has
actually landed. Deliberately does NOT go through `wiki-server`/the HTTP endpoint
above — no admin session or credentials needed, and it keeps working whether the
server is healthy, crashed, or mid-restart, which is the whole point of a backup
that's supposed to survive a disaster rather than assume one didn't happen.

**Point `BACKUP_DIR` at a disk/mount OTHER than the one `VAULT_PATH` lives on** —
an external USB drive, a network share, another machine over sshfs/NFS, anything
that doesn't share the SD card's own failure mode. `wiki-backup.service`'s
`ProtectSystem=strict` only grants read access to `/opt/wiki/vault_data` by
default (see the unit file's own comment) — an unusual `BACKUP_DIR` outside the
paths systemd hardening normally allows needs a
`ReadWritePaths=` override in `/etc/systemd/system/wiki-backup.service.d/`, not a
loosening of the shipped unit.

The timer defaults to `OnCalendar=daily`, `Persistent=true` (a missed run — e.g.
the device was off — fires as soon as it's back up, same convention as the
`systemd-timers.md` example in this very vault). Check it landed with
`systemctl list-timers wiki-backup.timer` and `journalctl -u wiki-backup.service`.

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
already-existing site (`https://example.com/wiki/`), it needs to know that prefix
exists so every `href`/`fetch()` URL/redirect it generates client-side actually points
somewhere real.

**Needs no server-side configuration for the common case.** `static/shell.html` is
served with the same body for every route (see `PageRoutes.cpp`), so it infers the
mount prefix itself, client-side, the moment it loads: an inline script (the very first
thing in `<head>`, before any other resource) matches `location.pathname` against this
app's own known route shapes (`/d/...`, `/edit/...`, `/search`, ...) and sets a
`<base href="{whatever came before that match}/">` tag, which every relative resource
link/fetch URL in the rest of the page then resolves against automatically. Point a
reverse proxy at a subpath, or none at all, and this correctly adapts either way with
nothing to set anywhere.

**One case that inference can never close by pattern-matching alone**, though: a path
matching NO known route (a typo, a stale `[[wiki-link]]`, anything `main.cpp`'s default
handler ends up serving `shell.html` for with a 404 status), on a browser that hasn't
loaded any page from this site yet either — no signal is left client-side to recover
the prefix from in that exact combination. A `localStorage`-cached last-known-good
prefix (`wiki.lastKnownBasePath`) covers the realistic case (a broken link clicked FROM
an already-loaded page on this same site) completely, but a genuine first hit — landing
directly on a broken/stale link with nothing cached yet — still can't know the prefix
from the client side alone, and degrades to an unstyled (but non-crashing) page. This
was caught live: a real user's very first visit to this exact deployment landed
directly on a stale link and hit precisely this gap.

**`[server].base_path` in `config.toml` closes it completely, optionally.** Yes, this
app had this setting, removed it, and brought it back — not a reversal so much as
scoping it correctly the second time: it's no longer required for the app to function
under a subpath at all (that's still automatic, see above), only to eliminate one
specific residual edge case inference structurally cannot solve. Set it and restart the
service:

```toml
[server]
base_path = "/wiki"
```

and `PageRoutes.cpp` bakes that prefix into EVERY served `shell.html` — matched route or
not — as an authoritative `window.__WIKI_KNOWN_BASE_PATH__`, which the client-side
script checks first and trusts over its own guessing. Leave it unset for a deployment on
its own (sub)domain, or if the cold-start edge case above is acceptable to leave
unstyled on a visitor's very first hit. See `static/shell.html`'s own inline script
comment and `src/config/AppConfig.h`'s comment on `basePath` for the full reasoning,
including the real bug this mechanism shipped with once before landing here (an earlier
fallback assumed an unmatched path's ENTIRE contents WAS the prefix, caught live against
this deployment's own real nginx config, not a synthetic test).

The nginx side is a plain prefix-stripping `proxy_pass`, with no response-body or
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

No `sub_filter`, no `proxy_redirect`, no `Accept-Encoding ""` hack — none of those
proxy-side response-rewriting tricks are needed regardless of whether `base_path` above
is set; they were necessary crutches BEFORE the frontend became fully client-rendered
(the first working version of this exact deployment actually went live through them,
back when the backend still templated pages server-side), and the current shape removes
the need for them completely and permanently, rather than patching the symptom on the
proxy side over and over.

## Remote MCP

Phase 2 feature — an admin-toggleable, bearer-token-protected `POST /mcp` HTTP
endpoint (see `docs/mcp.md`), letting an MCP client reach this wiki over the network
instead of only a local stdio spawn. Enable/disable, write access, the token, and the
IP allowlist are all managed live from the Account page — no config.toml edit, no
restart.

**Requires TLS in front of this app.** The bearer token travels in a plain
`Authorization` header on every request — over plain HTTP that's readable by anything
between the client and this box. `wiki-server` deliberately doesn't terminate TLS
itself (see "TLS / public internet access" above) — put the SAME reverse proxy this
app already needs for any public exposure in front of `/mcp` too; there's no separate
listener to configure, it's one more route on the existing `127.0.0.1:8080` upstream.

**The IP allowlist depends on the proxy setting the right headers correctly** — the
exact nginx block already shown above for the subpath case is what this needs, and
happens to already be right for it:

```nginx
proxy_set_header X-Real-IP $remote_addr;
proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
```

`auth::clientIp()` (`src/auth/ClientIp.h`) reads `X-Real-IP` first, falling back to
`X-Forwarded-For`'s LAST entry. Both matter for the same reason: `proxy_set_header`
OVERWRITES a header before forwarding it upstream (no client-supplied `X-Real-IP`
survives that — `$remote_addr` is nginx's own view of the TCP connection, not
spoofable from the client side), while `$proxy_add_x_forwarded_for` APPENDS
`$remote_addr` to whatever `X-Forwarded-For` the client already sent — so that
header's FIRST entry is exactly what the client claimed (trivially spoofable: a
request with `X-Forwarded-For: <an-allowlisted-ip>` would walk straight past an
allowlist that trusted the first entry), while the LAST is always nginx's own append.
Confirmed live against this exact deployment's real nginx config, not assumed —
including a direct test simulating a spoofed first entry with the real client IP
appended after it, correctly still blocked.

A deployment with a DIFFERENT proxy chain (more than one hop, or a proxy that doesn't
set `X-Real-IP`/doesn't use `$proxy_add_x_forwarded_for` the same way) needs to verify
its own directives produce the same guarantee — a misconfigured proxy here doesn't
break the bearer-token check, only the IP allowlist's own guarantee on top of it. The
token is still the actual gate; treat the allowlist as a defense-in-depth layer, not
the only thing standing between the internet and this vault.

**Whether this is currently on, and with what settings, is deliberately not recorded
here** — a live security-posture snapshot ("enabled, write access on, no IP
restriction, as of [date]") is the same category of information as a real host/domain:
accurate the day it's written, guaranteed to drift the next time anyone flips a toggle
in the Account page, and worth exactly nothing to a reader who can't act on it anyway.
Check the actual current state from the Account page's Remote MCP section directly —
that's also the ONLY place the raw bearer token itself is ever shown, exactly once, on
generation (`McpRemoteConfig::regenerateToken()` is built around that single display on
purpose — a plaintext credential has no business sitting in version control forever,
which is the same reason this whole paragraph doesn't try to track live state either).
To rotate it: Account page → Remote MCP section → Regenerate (invalidates the old
value immediately), or `POST /api/admin/mcp-remote-config/regenerate-token` as admin.
