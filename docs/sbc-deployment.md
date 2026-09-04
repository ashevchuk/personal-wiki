# Single-Board Computer Deployment Runbook

Everything needed to get `wiki-server`/`wiki-mcp` running on a real single-board
computer (Raspberry Pi or similar ARM/x86_64 SBC), start to finish, in one place. This
is the practical runbook; `docs/deployment.md` and `docs/architecture.md` carry the
broader design rationale and history — this file exists so a deployment doesn't require
reading either of them first. What it does NOT cover: actually connecting an MCP client
to `wiki-mcp` once it's built and installed (tool schemas, `claude_desktop_config.json`,
the remote HTTP transport) — that's `docs/mcp.md`, a genuinely separate concern from
getting the binary onto the device in the first place.

See `docs/deployment.md`'s "Real-hardware verification status" for exactly what has and
hasn't been verified live: cross-compiled binaries have been deployed to and verified
running natively on a real armv7 (Debian 9 stretch) target; a full native build
compiled on-device has not been done.

## Three ways to get this running on the device

| | Native build (on-device) | Cross-compile (from dev machine) | Docker |
|---|---|---|---|
| When to use | Modern distro, capable enough CPU/RAM, time to spare | Old distro (no C++20 compiler available), weak CPU, or just don't want to burn hours on-device | Capable, modern-enough device (Pi 4/5, 64-bit OS) where you'd rather not manage a toolchain at all |
| Toolchain | The device's own GCC/Clang ≥ C++20 | [zig](https://ziglang.org/) (`zig cc`/`zig c++`), bundles its own musl libc + libc++ | Whatever `docker build` pulls in, entirely inside the image |
| Output | Dynamically linked against the device's own glibc | Fully static (`-static`), zero runtime dependency on the target's libc | A container image; the device's own userland is untouched |
| Verified live | Not yet (no on-device compile has been run to completion) | Yes — see `docs/deployment.md` | Not on real ARM SBC hardware specifically (built/run and verified on x86_64 — see `docs/docker.md`) |

Pick native if the device is reasonably capable and current (Raspberry Pi OS Bookworm+,
a recent Debian/Ubuntu ARM64). Pick cross-compile if the target is old/weak/EOL (e.g.
Debian 9 stretch on 32-bit ARM, glibc 2.24) — a modern glibc cross-toolchain would link
against a newer glibc than the target has and fail at runtime with
`GLIBC_2.XX not found`; a static musl binary sidesteps that by not touching the
target's libc at all. Docker sits between the two: it needs a device modern enough to
run a current Docker Engine in the first place — which rules it out for exactly the
old/weak targets cross-compilation exists for — but if the device qualifies, it trades
a from-scratch native build for a `docker build` and skips toolchain management
entirely. The rest of this runbook covers only the native/cross-compile paths in
detail; see `docs/docker.md` for the Docker one.

## Path A — Native build, on the device

### Prerequisites on the device

- A C++20-capable compiler (verified against GCC 16.2.1; GCC has supported C++20 since
  version 10, newer is safer)
- CMake ≥ 3.21, Ninja
- git, curl
- Python 3 (for the `ctest` security integration test; not needed at runtime)
- A Linux kernel with inotify enabled (standard everywhere modern)

### Build

```sh
git clone https://github.com/ashevchuk/personal-wiki.git wiki && cd wiki

# FULL clone, not --depth 1 — see docs/architecture.md's "Build" section
# for why a shallow clone can silently miss vcpkg.json's pinned baseline
# commit (worse on a slow SBC network link, but no less necessary).
git clone https://github.com/microsoft/vcpkg.git vcpkg
./vcpkg/bootstrap-vcpkg.sh -disableMetrics

cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"

ctest --test-dir build --output-on-failure   # don't skip this before trusting the build
```

Expect this to take substantially longer than on a desktop — Drogon+OpenSSL+trantor
from scratch is not a quick build on a weak SBC. It's a one-time cost per device/OS
image.

## Path B — Cross-compile from an x86_64 dev machine

### Toolchain setup (one-time, on the dev machine)

Install [zig](https://ziglang.org/) (any recent 0.16.x release works; the exact
`code=139` linker bug below has only been confirmed against 0.16.0, but the fix applies
regardless). The repo already ships the cross-compilation scaffolding under `cross/`:

- `cross/arm-musl/{cc,c++,ar,ranlib}` — thin wrapper scripts pinning the target
  (`arm-linux-musleabihf`, `-mcpu=generic+v7a`).
- `cross/arm-musl/toolchain.cmake` — the CMake toolchain file. `CMAKE_FIND_ROOT_PATH`
  is deliberately left unset here — pass it at configure time (below), so this file
  stays reusable without a hardcoded absolute path.
- `cross/arm-musl/arm-musl.cmake` — the vcpkg overlay triplet (static, release-only).
- `cross/overlay-ports/{brotli,libuuid}` — patched vcpkg ports that skip building CLI/
  test executables that hit a zig/lld linker bug (see "Known issues" below).

### Cross-build the dependencies

```sh
cd vcpkg
./vcpkg install --classic --triplet arm-musl \
  --overlay-triplets=../cross/arm-musl --overlay-ports=../cross/overlay-ports \
  --x-install-root=../vcpkg_installed_arm \
  drogon sqlite3[core,fts5,json1] libargon2 nlohmann-json md4c yaml-cpp \
  tomlplusplus catch2
cd ..
```

Classic mode, not manifest mode: `drogon[ctl]` isn't supported on a cross target (the
`ctl` code-generator tool needs to run on the build host, not the target), so the `ctl`
feature is deliberately omitted here — an already-built x64-linux `drogon_ctl` gets
passed to the project's own configure step instead (see below), reusing the host tool.

### Configure and build the project

```sh
export PKG_CONFIG_LIBDIR="$PWD/vcpkg_installed_arm/arm-musl/lib/pkgconfig:$PWD/vcpkg_installed_arm/arm-musl/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=""

cmake -S . -B build-arm -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cross/arm-musl/toolchain.cmake \
  -DCMAKE_PREFIX_PATH=$PWD/vcpkg_installed_arm/arm-musl \
  -DCMAKE_FIND_ROOT_PATH=$PWD/vcpkg_installed_arm/arm-musl \
  -DDROGON_CTL_COMMAND=$PWD/build/vcpkg_installed/x64-linux/tools/drogon/drogon_ctl \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-arm -j"$(nproc)"
```

`DROGON_CTL_COMMAND` needs a native x64-linux `drogon_ctl` built beforehand — the
simplest way to get one is to have already done a normal native build in `build/`
(Path A on the dev machine itself, or just `vcpkg install drogon[ctl]` for the host
triplet).

`PKG_CONFIG_LIBDIR` (not `PKG_CONFIG_PATH` — that only appends) fully replaces
pkg-config's search path so it can't accidentally resolve a host x86_64 `.pc` file
instead of the cross-built one; `PKG_CONFIG_SYSROOT_DIR=""` avoids prefixing paths that
are already correct.

### Verify BEFORE shipping anything to real hardware

Install `qemu-user-static` (provides `qemu-arm-static`) on the dev machine and actually
run the cross-compiled binaries under emulation — compiling cleanly is not the same as
working:

```sh
qemu-arm-static ./build-arm/tests/unit_tests
# must report e.g. "All tests passed (N assertions in M test cases)" — not just exit 0

qemu-arm-static ./build-arm/wiki-server --create-admin
# real smoke test: actually creates an admin row via a real ARM binary
```

Only after both of those pass cleanly should the binaries get copied to the target.

### Known issues hit during cross-compilation (and their fixes)

- **zig 0.16.0's bundled lld SIGSEGVs (`code=139`) when statically linking many `.a`
  archives for `arm-linux-musleabihf`.** Root cause, found by bisecting a manually
  reproduced link command flag-by-flag: NOT archive count, NOT parallelism (both tested
  and ruled out) — it's `-Xlinker --dependency-file=...`, which CMake's Ninja generator
  (≥3.20) adds automatically for linker-level dependency tracking. That one flag
  crashes lld for this target 100% of the time, deterministically. Fix (already applied
  in `cross/arm-musl/toolchain.cmake`): `set(CMAKE_LINK_DEPENDS_USE_LINKER OFF)` — CMake
  falls back to non-linker-based dependency tracking with no other effect.
- **Linking a small executable against a single `.a` also crashed the same way** for
  brotli's CLI tool and libuuid's `test_uuid` — same underlying lld bug, different
  trigger shape. Fixed via the two overlay ports under `cross/overlay-ports/`, which
  skip building those specific executables (neither is actually needed — only the
  libraries are).
- **CMake/Ninja auto-enable C++20 module dependency scanning** (`clang-scan-deps
  -format=p1689`) whenever Clang+C++20+Ninja are all in play, regardless of whether the
  project uses modules (it doesn't). The *system* `clang-scan-deps` can't resolve zig's
  bundled libc++ for a foreign `--target`, so it fails with false
  `<string>`/`<vector>`/etc. "file not found" errors even though normal compilation
  finds them fine. Fixed (already applied): `set(CMAKE_CXX_SCAN_FOR_MODULES OFF)`.

## Install (Path A or B — Docker doesn't use this step, see `docs/docker.md`)

```sh
sudo cmake --install build --prefix /opt/wiki       # or build-arm for a cross-build
```

Places `bin/wiki-server`, `bin/wiki-mcp`, `static/`, `config.example.toml`, and
`share/wiki/systemd/{wiki.service,wiki.env.example}` under `/opt/wiki`. Does **not**
create a working `config.toml` — only the example, so a real config never silently
materializes from defaults.

For a cross-build, `cmake --install` still runs on the dev machine (against
`build-arm`) into a local staging prefix, which then gets transferred as a whole tree
(e.g. `tar czf` + `scp`) to the target — there's no `cmake --install` step happening on
the target itself.

## First-time setup on the target

```sh
sudo useradd --system --home-dir /opt/wiki --shell /usr/sbin/nologin wiki
sudo chown -R wiki:wiki /opt/wiki

cd /opt/wiki
sudo -u wiki cp config.example.toml config.toml
```

Edit `config.toml` as needed — the fields that actually matter for a real deployment:

```toml
[server]
listen_addr = "127.0.0.1"   # keep on loopback; a reverse proxy handles TLS/public exposure
port = 8080
threads = 2
# base_path = "/wiki"       # OPTIONAL — see "Under a subpath of an existing site" below

[vault]
path = "./vault_data"       # relative to the working directory (WorkingDirectory= in the unit)

[mcp]
scope = "admin"             # "admin" sees public+private (local spawn by the owner); "public" for any future remote transport
```

Create the admin account:

```sh
sudo -u wiki ./bin/wiki-server --create-admin
```

This prompts for a username and password interactively (echo disabled, nothing gets
logged). **Actually save the password somewhere before closing the terminal** — running
this again overwrites the same single admin row, which is the only recovery path if the
password gets lost, but it also means there's no "show me the current password" escape
hatch; losing it silently locks the account out until someone reruns this exact
command.

## systemd

```sh
sudo cp /opt/wiki/share/wiki/systemd/wiki.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now wiki.service
sudo systemctl status wiki.service
```

The shipped unit is already hardened:

```
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
ReadWritePaths=/opt/wiki/vault_data
```

`ReadWritePaths=/opt/wiki/vault_data` is the ONLY path the service can write to —
everything else under `ProtectSystem=strict` is read-only to it. This already accounts
for Drogon's own internal upload-buffering directory (used to stage large multipart
request bodies to disk, independent of the app's own attachment storage): it's pointed
at `<vault_path>/.uploads-tmp` (an absolute path, computed at startup), which is
covered by the same `ReadWritePaths` entry and skipped by `IndexBuilder`'s existing
"skip `.git`/`.trash`/anything-dot" rule, so it's never mistaken for a document. This
was a real bug caught live on the first real-hardware deploy — the app used to default
to a relative `./uploads` path that Drogon resolved against its document root
(`static/`), landing outside `ReadWritePaths` and spamming
`Error 30 creating path ...: Read-only file system` at startup. It's fixed in the code
now; nothing to configure manually for this.

`EnvironmentFile=-/etc/wiki/wiki.env` is optional (note the leading `-`) — nothing
currently reads any environment variable (admin credentials live in SQLite, sessions
are unsigned random tokens), so the file is allowed to simply not exist.

## Reverse proxy

`wiki-server` never terminates TLS itself and, by default, listens only on
`127.0.0.1:8080` — it's meant to sit behind a reverse proxy for anything beyond local
access.

### On its own (sub)domain

The simplest case — the app owns the whole (sub)domain, no path-prefix concerns:

```nginx
server {
    listen 443 ssl;
    server_name wiki.example.com;
    ssl_certificate     /etc/letsencrypt/live/wiki.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/wiki.example.com/privkey.pem;

    location / {
        proxy_pass http://127.0.0.1:8080/;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
    }
}
```

### Under a subpath of an existing site (e.g. `/wiki`)

**No `config.toml` setting needed for the common case.** The frontend is a
client-rendered static shell (`static/shell.html`, served with the same body no
matter what path it's requested from — see `docs/architecture.md`); its own inline
bootstrap script infers the mount prefix itself on load, by matching
`location.pathname` against this app's known route shapes (`/d/...`, `/edit/...`,
`/search`, ...) and setting a `<base href>` from whatever came before that match —
works correctly under any subpath, or none, automatically:

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

No `sub_filter`, no `proxy_redirect`, no response-rewriting of any kind needed on the
nginx side — that whole class of hack was only ever necessary back when the backend
still templated pages server-side; the current client-rendered shell removes the need
for all of it permanently, regardless of whether `base_path` below is set.

**One gap this inference can never close by pattern-matching alone**: a path matching
NO known route (a typo, a stale `[[wiki-link]]`) on a browser with nothing yet cached
in `localStorage` either — no signal survives client-side for that exact combination.
The `localStorage`-cached last-known-good prefix (`wiki.lastKnownBasePath`) covers the
realistic case (a broken link clicked from an already-loaded page) completely, but a
genuinely cold start — landing directly on a stale link, nothing cached yet — still
degrades to an unstyled (never crashing) page. Caught live: a real user's first-ever
visit to a real deployment landed exactly there.

**`[server].base_path = "/wiki"` in `config.toml` closes this completely, optionally**
(restart the service after setting it) — `PageRoutes.cpp` then bakes that prefix into
every served `shell.html`, matched route or not, as an authoritative
`window.__WIKI_KNOWN_BASE_PATH__` the bootstrap script trusts over its own guessing.
Skip it for a deployment on its own (sub)domain, or if that one cold-start edge case is
acceptable to leave unstyled. Full reasoning in `shell.html`'s own inline script
comment and `src/config/AppConfig.h`'s comment on `basePath`.

## Backup

The vault directory (`[vault].path`, plus its `.trash/`) is the entire source of truth.
The SQLite index (`[index].db_path`) is fully disposable — `wiki-server --reindex`
rebuilds it from scratch — backing it up is optional (saves a rescan on restore for a
very large vault, nothing more). Minimum viable backup: the vault directory, regularly
— two ways to actually make that happen, full detail in `docs/deployment.md`'s own
"Backup" section:

- **Ad hoc**: Account page (admin, Web UI) → "Download backup" — streams a `.tar.gz` of
  the whole vault on demand. Only useful while the box and its disk are both still
  alive, which is exactly the case a dead SD card is not.
- **Automated, unattended**: an opt-in `systemd` timer
  (`share/wiki/systemd/wiki-backup.{service,timer}`, installed the same way `wiki.service`
  itself was above — NOT enabled by a plain `cmake --install`). Tars `VAULT_PATH`
  straight off disk, independent of `wiki-server` entirely — keeps working whether the
  service is healthy, crashed, or mid-restart. Point its `BACKUP_DIR` at a DIFFERENT
  disk/mount than the SD card the vault itself lives on (a USB drive, a network share,
  another machine over sshfs/NFS) — a backup on the same card it's meant to protect
  against doesn't survive that card dying, which is the actual disaster this exists for.

## Update

```sh
cd wiki && git pull
cmake --build build -j"$(nproc)"                        # or build-arm for cross
ctest --test-dir build --output-on-failure               # before touching prod
sudo cmake --install build --prefix /opt/wiki             # or transfer a cross-build tree
sudo systemctl restart wiki.service
```

For a cross-compiled deployment, "install" means: rebuild `build-arm`, re-verify under
`qemu-arm-static`, transfer the new `bin/wiki-server`/`bin/wiki-mcp` to the target
(`scp` is fine — they're fully static, single files, no dependency tree to sync),
`systemctl stop wiki.service` → replace the binary → `chown wiki:wiki` → `chmod 755` →
`systemctl start wiki.service`.

## Troubleshooting checklist

- **`journalctl -u wiki.service` shows `Read-only file system` errors on startup** —
  something is trying to write outside `ReadWritePaths=/opt/wiki/vault_data`. Already
  fixed for Drogon's own upload-buffer path (see "systemd" above); if this shows up
  again for a different path, either add it to `ReadWritePaths=` in the unit or, if
  it's app-generated, redirect it into the vault path the way `.uploads-tmp` already is.
- **`GET /` (or the subpath-prefixed equivalent) returns a 404** — expected if there's
  no request-matching route (no dedicated homepage view exists; the client-side router
  redirects bare `/` to `/search`). Confirm `/healthz` and `/search` both return `200`
  before suspecting anything is actually broken.
- **CSS/JS 404 or the login form posts to the wrong path when reverse-proxied under a
  subpath** — set `[server].base_path` (see "Under a subpath of an existing site"
  above) and restart; that closes this permanently. Without it, an unmatched path
  falls back to the last known-good prefix cached in `localStorage`, empty on a
  genuinely cold start (first-ever visit in that browser landing directly on a
  subpath'd URL with nothing cached yet) — load `/search` (or any known route) first
  to warm the cache as a one-off workaround if setting `base_path` isn't an option.
- **A static cross-compiled link crashes with `code=139`** — see "Known issues" above;
  almost certainly the `-Xlinker --dependency-file=...` / `CMAKE_LINK_DEPENDS_USE_LINKER`
  issue if the toolchain file has drifted from `cross/arm-musl/toolchain.cmake`.
- **Locked out of the admin account** — there is no "recover the password" path by
  design (only an argon2id hash is stored). Stop the service, re-run
  `sudo -u wiki ./bin/wiki-server --create-admin` on the target (overwrites the single
  admin row), start the service back up. Stopping first is what's actually been
  verified to work cleanly; running `--create-admin` concurrently against a live
  service's SQLite connection hasn't been tested and isn't a claim made here.
