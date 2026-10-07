# Deployment

## Real-hardware verification status

A full native build (`cmake --build` from scratch, not cross-compilation) is verified
on x86_64 Linux (Arch) only — `cmake --install`, running the installed tree, the full
`ctest` security/E2E pass, a live VaultWatcher. A native build on a Raspberry Pi itself
(compiling on the device, as described below under "Build") has not been done.

Cross-compiled binaries have been deployed to and verified on a live target device —
armv7l, Debian 9 (stretch, EOL, glibc 2.24) — via `arm-linux-musleabihf`+musl+static
(see "Cross-compilation" below): `wiki-server` and `wiki-mcp` run natively (not
emulated) on the device, `unit_tests` passes under `qemu-arm-static`, and the full
login → CSRF → document creation → atomic disk write → FTS5 search with snippet
highlighting cycle works over live HTTP against a real systemd unit (hardening as
described below), alongside live nginx/Samba/NFS/ProFTPD/mosquitto/munin on the same
box with no impact on any of them. The code has no x86-specific assumptions.

## Recording your own live deployment target

Keep the host/IP, install root, public URL (and reverse-proxy subpath, if any), and
the systemd unit/user running it in a private note outside this repo — not in version
control, since a public git history is the wrong place for a real deployment's
identifying details. Example shape, with placeholder values (RFC 5737 documentation
IP, `.example.com`):

- Host: `root@192.0.2.10` (`wiki.example.com`, armv7l/sunxi, Debian 9 stretch).
- Install root: `/opt/wiki` (`bin/`, `static/`, `config.toml`, `vault_data/`).
- Public URL: `http://192.0.2.10/wiki/` (nginx `default` vhost, prefix-stripping
  `proxy_pass` to `127.0.0.1:8080` — see "Reverse-proxying under a subpath" below;
  check directly on the instance whether `[server].base_path` is set — it's optional,
  closes one specific edge case).
- systemd unit: `wiki.service`, `User=wiki`.

An old/weak target device's toolchain is the reason to cross-compile from a dev
machine rather than run `cmake --install` natively on the device — the "Update"
recipe below describes the native-build path for a device capable of it.

**Redeploy recipe for a cross-compiled target** (backend or frontend changes —
cross-compilation always produces both binaries, so there's no reason to special-case
static-only changes here):

```sh
# 1. incremental cross-build (vcpkg_installed_arm/ and build-arm/ are both reused,
#    not recreated, on every subsequent deploy — see "Cross-compilation" below for
#    the from-scratch setup)
export PKG_CONFIG_LIBDIR="$PWD/vcpkg_installed_arm/arm-musl/lib/pkgconfig:$PWD/vcpkg_installed_arm/arm-musl/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=""
cmake --build build-arm -j"$(nproc)"
qemu-arm-static ./build-arm/tests/unit_tests   # must pass every case before shipping

# 2. ship the new binaries + static assets alongside the live ones (never
#    overwrite in place — an scp that dies mid-transfer must not leave a
#    half-written binary where systemd will find it on next restart)
scp build-arm/wiki-server build-arm/wiki-mcp root@192.0.2.10:/tmp/
scp -r static root@192.0.2.10:/tmp/static-new

# 3. on the target: back up what's live, strip + swap the new files in, restart, verify
ssh root@192.0.2.10 '
  set -e
  STAMP=$(date +%Y%m%d-%H%M%S)
  cp /opt/wiki/bin/wiki-server /opt/wiki/bin/wiki-server.bak-$STAMP
  cp /opt/wiki/bin/wiki-mcp /opt/wiki/bin/wiki-mcp.bak-$STAMP
  mv /opt/wiki/static /opt/wiki/static.bak-$STAMP
  chmod +x /tmp/wiki-server /tmp/wiki-mcp
  strip --strip-all /tmp/wiki-server /tmp/wiki-mcp
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

**Strip the binaries on the target device itself, never on the dev machine.**
`CMAKE_BUILD_TYPE=RelWithDebInfo` keeps debug symbols in the statically-linked ELF —
`wiki-server` ships at ~116M, `wiki-mcp` at ~61M; `strip --strip-all` drops that to
~14M for `wiki-server` (roughly 8x) with no behavior change. The dev machine's own
`strip` doesn't recognize this cross-compiled ARM ELF's architecture, and
`zig objcopy --strip-all` errors `unimplemented` for this static-musl-ARM shape — the
target's own native `strip` (real GNU binutils, correct architecture) is what works.
The unstripped `build-arm/wiki-server`/`wiki-mcp` stay on the dev machine, kept around
for `addr2line`/`gdb` if a crash ever needs symbols.

The `.bak-$STAMP` copies are never cleaned up automatically — sweep old ones by hand
occasionally on a long-lived deployment.

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
# [server].theme is optional too — which of classic/dark/green a fresh
# browser (nothing picked yet) lands on; see config.example.toml's own
# comment. Restart after changing either.

# create the admin account (username AND password entered interactively,
# password echo disabled) -- the username isn't fixed to "admin", it's
# whatever gets typed here; re-running --create-admin later overwrites
# BOTH username and password together, which is also the (only) way to
# change either one -- there's no config.toml knob for this, since
# SQLite's users table (id=1, the single enforced admin row) already IS
# the one source of truth for it.
sudo -u wiki ./bin/wiki-server --create-admin
```

## systemd

```sh
sudo cp /opt/wiki/share/wiki/systemd/wiki.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now wiki.service
sudo systemctl status wiki.service
```

The unit ships with hardening (`ProtectSystem=strict`, `NoNewPrivileges=yes`,
`ReadWritePaths=/opt/wiki/vault_data` — the only place the service writes — plus
`CapabilityBoundingSet=` empty, `RestrictAddressFamilies`,
`SystemCallFilter=@system-service`, `MemoryDenyWriteExecute`, and the other
`Protect*`/`Restrict*` directives — see `systemd/wiki.service`'s own comments for
what each covers). With `CapabilityBoundingSet=` empty the process loses
`CAP_DAC_OVERRIDE` even running as `root` — file ownership/permissions under
`ReadWritePaths` must be exactly right; the capability bounding set does not fall
back to privilege. Run `systemd-analyze security wiki.service` to see its score, and
exercise the app once after any change to this unit's hardening (login, create a
document, search, upload an attachment) — a hardening regression here fails at
runtime (a specific route breaks), not at `daemon-reload` time.
`EnvironmentFile=-/etc/opt/wiki/wiki.env` is optional — the leading `-` means systemd
still starts the unit if the file is missing. Admin credentials live in SQLite and
sessions are random tokens with no secret-based signature. The runtime secret readers
are `CloudEmbeddingProvider` and `CloudChatClient`: each `getenv()`s the variable
named by `[embeddings].api_key_env` / `[llm].api_key_env` in `config.toml` (see
`systemd/wiki.env.example`, `docs/embeddings.md`, `docs/llm.md`). For embeddings
`provider = "none"` / `"local"` and llm `provider = "none"`, or a loopback
OpenAI-compatible server that doesn't authenticate, the file can stay absent.

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
`GET /api/admin/backup` (`src/vault/BackupService.h`), which shells out to the system
`tar` binary (`fork()`+`execlp()` with an explicit argv, never `system()`/`popen()`,
so nothing about the vault path's own content can be interpreted as shell syntax) and
streams back a `.tar.gz` of the whole vault, `.trash/` and the index db included,
`.uploads-tmp/` (Drogon's own transient upload-staging buffer) and `.mcp-uploads/`
(remote-MCP large-file tickets) excluded. Useful for a quick snapshot before a risky
change; not a substitute for the automated path below, since it only works while the
server and its disk are both still alive.

**Automated, via systemd timer** (`systemd/wiki-backup.{service,timer}`,
`systemd/wiki-backup.sh`) — not installed/enabled by a plain `cmake --install`;
opt in explicitly:

```sh
sudo cp /opt/wiki/share/wiki/systemd/wiki-backup.{service,timer} /etc/systemd/system/
sudo mkdir -p /etc/opt/wiki
sudo cp /opt/wiki/share/wiki/systemd/wiki-backup.env.example /etc/opt/wiki/wiki-backup.env
sudo "$EDITOR" /etc/opt/wiki/wiki-backup.env   # set BACKUP_DIR to a DIFFERENT disk/mount, see below
sudo systemctl daemon-reload
sudo systemctl enable --now wiki-backup.timer
```

`wiki-backup.sh` tars `VAULT_PATH` straight off disk (same `.uploads-tmp/` and
`.mcp-uploads/` exclusion as the Web UI button, same atomic temp-file-then-rename
discipline as `VaultRepository`'s own document writes), then prunes down to
`RETENTION_COUNT` (default 14), oldest first, only after a new backup has landed.
It deliberately does not go through `wiki-server`/the HTTP endpoint above — no admin
session or credentials needed, and it keeps working whether the server is healthy,
crashed, or mid-restart.

**Point `BACKUP_DIR` at a disk/mount other than the one `VAULT_PATH` lives on** — an
external USB drive, a network share, another machine over sshfs/NFS, anything that
doesn't share the SD card's own failure mode. `wiki-backup.service`'s
`ProtectSystem=strict` only grants read access to `/opt/wiki/vault_data` by default
(see the unit file's own comment) — an unusual `BACKUP_DIR` outside the paths systemd
hardening normally allows needs a `ReadWritePaths=` override in
`/etc/systemd/system/wiki-backup.service.d/`, not a loosening of the shipped unit.

The timer defaults to `OnCalendar=daily`, `Persistent=true` (a missed run — e.g. the
device was off — fires as soon as it's back up). Check it landed with
`systemctl list-timers wiki-backup.timer` and `journalctl -u wiki-backup.service`.

## Update

```sh
cd wiki && git pull
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure   # before restarting prod
sudo cmake --install build --prefix /opt/wiki
sudo systemctl restart wiki.service
```

## Static-assets-only redeploy (frontend change, no rebuild needed)

When the only thing that changed is under `static/` (CSS, JS, `shell.html`) — no
`src/` touched, nothing to recompile or cross-compile — the full "Update" flow above is
overkill. From a dev machine that isn't the target itself (the normal case for this
project: cross-compiled ARM binaries built on x86_64, see "Cross-compilation" below),
push just the `static/` tree over SSH instead:

```sh
# 1. tar the WHOLE static/ tree, never a selective "the files I believe
#    changed" scp — see below for why that shortcut is unsafe.
tar -czf static-deploy.tar.gz -C static .
scp static-deploy.tar.gz root@<target>:/tmp/static-deploy.tar.gz

# 2. on the target: extract to a STAGING directory first, never straight
#    over the live one — a tar that dies mid-extract must not leave a
#    half-written static/ tree where the running server can see it.
ssh root@<target> '
  set -e
  rm -rf /tmp/static-new && mkdir -p /tmp/static-new
  tar -xzf /tmp/static-deploy.tar.gz -C /tmp/static-new
  chown -R wiki:wiki /tmp/static-new
  cd /opt/wiki
  rm -rf static.old
  mv static static.old && mv /tmp/static-new static
  systemctl restart wiki.service
  rm -rf static.old /tmp/static-deploy.tar.gz
'
```

**Never selectively `scp` "the files I changed"** — a partial sync can miss a file
that also changed, and a stale JS/CSS file doesn't fail loudly; nothing about the
running site looks broken until someone goes looking for a feature that should
already be live. Tar and swap the entire directory every time; the cost difference at
this project's size is a couple of seconds.

**A restart is mandatory, not optional, even though it's "just static files."** Most
of `static/` is served fresh from disk on every request (Drogon's own static-file
handler). `shell.html` is the one exception: `PageRoutes.cpp` reads it once at process
startup and caches the built HTML in memory (`buildShellHtml`/`g_shellHtml`) —
swapping the file on disk without restarting the process means every request keeps
getting the old cached `shell.html` indefinitely, silently.

**Verify by md5-summing every single file after, not the ones you think changed:**

```sh
find static -type f -exec md5sum {} \; | sed 's|static/||' | sort > /tmp/local.md5
ssh root@<target> 'cd /opt/wiki/static && find . -type f -exec md5sum {} \;' \
  | sed 's|  \./|  |' | sort > /tmp/remote.md5
diff /tmp/local.md5 /tmp/remote.md5 && echo "identical" || echo "MISMATCH"
```

Use `s|  \./|  |`, not `s|^\./|  |` — `md5sum`'s own output separator is two spaces
before the path, and `^\./` anchors at the start of the line (the hash column), so it
matches nothing and the diff silently shows a bogus per-path mismatch instead of
catching real content differences.

**When testing a redeploy against a local throwaway instance**, kill the old process
by an exact, verified PID (`ps aux | grep wiki-server`, then `kill -9 <pid>`), not
`pkill -f wiki-server` trusting its exit code alone. A surviving old process keeps
serving its own cached state (including a stale `shell.html`) while the new
`wiki-server` fails to bind and exits immediately — check its log for
`FATAL Address already in use` if a change appears to have no effect.

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
`arm-linux-musleabihf` SIGSEGVs (`code=139`) in the bundled lld. Root cause: not
archive count or parallelism, but a specific flag — CMake (Ninja generator, ≥3.20)
automatically adds `-Xlinker --dependency-file=...` for linker-level dependency
tracking, and that flag crashes lld for this target deterministically. Fix:
`set(CMAKE_LINK_DEPENDS_USE_LINKER OFF)` in `cross/arm-musl/toolchain.cmake` (CMake
falls back to non-linker-based dependency tracking on its own). The two overlay ports
`cross/overlay-ports/{brotli,libuuid}` disable building their CLI/test binaries for
the same reason (linking a small executable against a single `.a` also crashes).

**nginx reverse proxy next to an nginx that's already live on the target**:
`wiki-server` deliberately listens only on `127.0.0.1:8080` (see `config.toml`), it
doesn't terminate TLS itself — adding a dedicated `server{}` block to the existing
nginx (a new subdomain or a `location`) is left to the administrator by hand; it does
not touch any existing nginx configuration automatically.

### Porting to a different board (different CPU architecture)

**First, check whether you need this section at all.** The existing
`cross/arm-musl` target is armv7 (32-bit ARM, Cortex-A7-class or newer,
hardfloat) — if your board is ALSO armv7/armhf, the already-built binary
(or your own build from the exact recipe above, unmodified) just runs, no
porting needed. If your board is x86_64, skip cross-compilation entirely —
use the plain native build recipe at the top of this doc; `cross/` isn't
involved. This section is only for a genuinely different CPU architecture
(most commonly aarch64/arm64 — the 64-bit mode most current-generation
SBCs, including newer Raspberry Pi boards, actually ship by default).

**Everything target-specific lives in exactly four files**, all under
`cross/arm-musl/` — copy that whole directory to `cross/<your-triplet-name>/`
and edit only what's below (`ar`/`ranlib` need no changes at all — they're
architecture-agnostic wrappers around `zig ar`/`zig ranlib`):

1. **`cc`/`c++`** — change the `-target` string to your architecture's zig
   target triple (`zig targets` lists what your installed zig supports).
   `arm-linux-musleabihf` becomes, for example, `aarch64-linux-musl` for a
   64-bit ARM board. Drop `-mcpu=generic+v7a` unless your new target has an
   equivalent reason to pin a baseline CPU feature set — that flag exists
   specifically for armv7's own fragmented Cortex-A/ARMv7 feature story, it
   isn't a generic requirement.
2. **`toolchain.cmake`** — change `CMAKE_SYSTEM_PROCESSOR` and both
   `CMAKE_C_COMPILER_TARGET`/`CMAKE_CXX_COMPILER_TARGET` to match. Leave
   `CMAKE_LINK_DEPENDS_USE_LINKER OFF` and `CMAKE_CXX_SCAN_FOR_MODULES OFF`
   in place for your first attempt — both work around zig 0.16.0 bugs found
   on armv7 that are plausibly generic to zig's cross-linking path, though
   unverified against a second architecture. If your build links cleanly
   without them, drop them; if it SIGSEGVs identically to the documented
   armv7 bug above, the fix is the same one line.
3. **The vcpkg triplet file** (`arm-musl.cmake` → `<your-name>.cmake`) —
   change `VCPKG_TARGET_ARCHITECTURE` to vcpkg's own name for your
   architecture (`arm64` for aarch64, `x64` for x86_64, `riscv64` for
   RISC-V — see vcpkg's own triplet docs for the authoritative list). Keep
   `VCPKG_CRT_LINKAGE`/`VCPKG_LIBRARY_LINKAGE` `static` and the
   `VCPKG_CHAINLOAD_TOOLCHAIN_FILE` line pointing at your new
   `toolchain.cmake`.
4. **`cross/overlay-ports/`** (brotli, libuuid, md4c) — these are not
   triplet-specific; the same `--overlay-ports=cross/overlay-ports` flag
   applies regardless of which triplet you build. Whether the specific
   patches in them (a brotli CLI-binary link crash, a libuuid CLI/test
   build issue) are still necessary for your new architecture is unverified
   — they were found on armv7, not derived from a spec. Try building
   without them first; patch further only on a matching crash.

Then run the same recipe as the armv7 instructions above, pointed at your
new triplet/toolchain file instead. The same mandatory step applies:
`qemu-<your-arch>-static ./build-<name>/tests/unit_tests` must pass every
case, not just avoid crashing, before the binary goes near real hardware.

**One alternative worth considering before cross-compiling at all**: the
musl+static approach exists specifically to dodge `GLIBC_2.XX not found`
against an old target OS (Debian 9 stretch, glibc 2.24, in this project's
real deployment). If your board runs a reasonably current Linux distro, a
plain glibc cross-toolchain — or a native on-device build, if the board has
enough RAM/storage/patience for an hours-long Drogon+OpenSSL build — may
work and sidesteps the whole `cross/` mechanism. A native on-device build
has not been tried for this project (see "Real-hardware verification
status" above).

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
nothing to set anywhere. Stylesheets in `shell.html` itself are created in that same
inline script *after* `<base>` exists, not as static `<link href="css/...">` tags —
the HTML preload scanner otherwise fetches them against the document URL
(`/wiki/edit/css/edit.css`) and gets this SPA shell (`text/html`) instead of CSS.

**One case that inference can never close by pattern-matching alone**: a path
matching no known route (a typo, a stale `[[wiki-link]]`, anything `main.cpp`'s
default handler ends up serving `shell.html` for with a 404 status), on a browser
that hasn't loaded any page from this site yet — no signal is left client-side to
recover the prefix from. A `localStorage`-cached last-known-good prefix
(`wiki.lastKnownBasePath`) covers the realistic case (a broken link clicked from an
already-loaded page on this same site) completely, but a genuine first hit landing
directly on a broken/stale link with nothing cached yet degrades to an unstyled
(non-crashing) page.

**`[server].base_path` in `config.toml` closes this gap completely, optionally.**
Set it and restart the service:

```toml
[server]
base_path = "/wiki"
```

and `PageRoutes.cpp` bakes that prefix into every served `shell.html` — matched route
or not — as an authoritative `window.__WIKI_KNOWN_BASE_PATH__`, which the client-side
script checks first and trusts over its own guessing. Leave it unset for a deployment on
its own (sub)domain, or if the cold-start edge case above is acceptable to leave
unstyled on a visitor's very first hit. See `static/shell.html`'s own inline script
comment and `src/config/AppConfig.h`'s comment on `basePath` for the full reasoning.

The nginx side is a plain prefix-stripping `proxy_pass`, with no response-body or
header rewriting at all. `client_max_body_size 0` is required for large attachment
uploads (a 120 MB PDF, etc.); nginx's default is 1m and would 413 before
`wiki-server` ever saw the body. `wiki-server` itself caps a single request at 2 GiB.

```nginx
location = /wiki {
    return 301 /wiki/;
}

location /wiki/ {
    client_max_body_size 0;  # unlimited; wiki-server's own 2 GiB request cap still applies
    proxy_pass http://127.0.0.1:8080/;
    proxy_set_header Host $host;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto $scheme;
    # Draft / Chat agent SSE (`GET /api/agent/sessions/{id}/stream`) sets
    # X-Accel-Buffering: no so this location does not have to turn
    # proxy_buffering off for every other response.
}
```

No `sub_filter`, `proxy_redirect`, or `Accept-Encoding ""` hack is needed regardless
of whether `base_path` above is set — those proxy-side response-rewriting tricks only
applied when the backend templated pages server-side; the current fully
client-rendered frontend has no use for them.

## Remote MCP

Phase 2 feature — an admin-toggleable, bearer-token-protected `POST /mcp` HTTP
endpoint (see `docs/mcp.md`), letting an MCP client reach this wiki over the network
instead of only a local stdio spawn. Enable/disable, write access, the token, and the
IP allowlist are all managed live from the Account page — no config.toml edit, no
restart.

**Requires TLS in front of this app.** The bearer token travels in a plain
`Authorization` header on every request — over plain HTTP that's readable by anything
between the client and this box. `wiki-server` deliberately doesn't terminate TLS
itself (see "TLS / public internet access" above) — put the same reverse proxy this
app already needs for any public exposure in front of `/mcp` too; there's no separate
listener to configure, it's one more route on the existing `127.0.0.1:8080` upstream.

**The IP allowlist depends on the proxy setting the right headers correctly** — the
exact nginx block already shown above for the subpath case is what this needs:

```nginx
proxy_set_header X-Real-IP $remote_addr;
proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
```

`auth::clientIp()` (`src/auth/ClientIp.h`) reads `X-Real-IP` first, falling back to
`X-Forwarded-For`'s last entry. Both matter for the same reason: `proxy_set_header`
overwrites a header before forwarding it upstream (no client-supplied `X-Real-IP`
survives that — `$remote_addr` is nginx's own view of the TCP connection, not
spoofable from the client side), while `$proxy_add_x_forwarded_for` appends
`$remote_addr` to whatever `X-Forwarded-For` the client already sent — so that
header's first entry is exactly what the client claimed (trivially spoofable: a
request with `X-Forwarded-For: <an-allowlisted-ip>` would walk straight past an
allowlist that trusted the first entry), while the last is always nginx's own append.

A deployment with a different proxy chain (more than one hop, or a proxy that doesn't
set `X-Real-IP`/doesn't use `$proxy_add_x_forwarded_for` the same way) needs to verify
its own directives produce the same guarantee — a misconfigured proxy here doesn't
break the bearer-token check, only the IP allowlist's own guarantee on top of it. The
token is still the actual gate; treat the allowlist as a defense-in-depth layer, not
the only thing standing between the internet and this vault.

Whether remote MCP is currently enabled, and with what settings, is not tracked in
this doc — check the Account page's Remote MCP section directly; that's also the only
place the raw bearer token is ever shown, exactly once, on generation
(`McpRemoteConfig::regenerateToken()`). To rotate it: Account page → Remote MCP
section → Regenerate (invalidates the old value immediately), or
`POST /api/admin/mcp-remote-config/regenerate-token` as admin.
