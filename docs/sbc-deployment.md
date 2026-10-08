# Single-Board Computer Deployment Runbook

Everything needed to get `wiki-server`/`wiki-mcp` running on a real
device — a Raspberry Pi or similar ARM/x86_64 SBC, or any other Linux
host — from build through install, systemd, TLS, backup, remote MCP, and
redeploys, in one place. `docs/architecture.md` carries the broader
design rationale; this is the operational runbook.

What this does **not** cover: connecting an MCP client to `wiki-mcp` once
it's built and installed (tool schemas, `claude_desktop_config.json`, the
remote HTTP transport's own protocol) — that's `docs/mcp.md`.

## Automating this with tools/wiki-ops.sh

Everything from "Install" through "Static-assets-only redeploy" below can
be driven by `tools/wiki-ops.sh` instead of typed by hand. It's one
script where flags take priority over `deploy.local.env` (gitignored,
never committed — copy `deploy.local.env.example` to start one), which
takes priority over an interactive prompt, which takes priority over a
hard default. `docs/wiki-ops.md` is the full reference (every flag,
worked examples, known limitations); `./tools/wiki-ops.sh help` always
reflects the current flag set fastest. The manual steps below stay the
reference for what each one actually does under the hood, and for anyone
without the script:

- `build native|cross|container|cross-container|container-arm|container-native`
  — Path A, Path B, the Docker image below, a containerized zig
  cross-toolchain (no local zig/vcpkg install needed), a multi-arch
  Docker image (amd64/arm64/armv7 via `--platform`), or a build done in
  Docker with the result deployed as a bare binary (no Docker on the
  target — see `docs/wiki-ops.md`'s glibc-compatibility caveat before
  pointing this at an old target).
- `verify cross|container|cross-container|container-arm|container-native`
  — runs the qemu-arm-static / `docker run` checks that "Verify BEFORE
  shipping anything to real hardware" below requires, before anything
  touches a target. `--qemu-cpu=NAME` pins the emulated core explicitly
  — needed for a narrower target than the default armv7 path (see
  `cross/armv6-musl/` for Raspberry Pi 1/Zero/Zero W).
- `deploy --target=HOST --first-time --with-backup-timer` — Install,
  first-time setup (including the `config.toml` fields below and
  `--create-admin`), systemd, and the opt-in backup timer, all in one
  atomic `bin/`+`static/` swap with a `.bak-$STAMP` safety copy on the
  target. `--port=N` sets `[server].port` for binary variants, or remaps
  only the container's host-side port via `WIKI_HOST_PORT` for container
  variants.
- `update --target=HOST` — the whole "Update" section below in one
  command: `git pull`, then the same variant's `build`, then a
  non-`--first-time` `deploy` (so `config.toml`, the vault, and the
  admin account stay untouched). `--static-only` skips straight to
  `static-redeploy` instead, for a pull that only touched `static/`.
- `static-redeploy --target=HOST` — the whole "Static-assets-only
  redeploy" section below, automatically: tar the whole tree, stage,
  swap, restart, md5-verify every file.
- `nginx-config root --domain=D` / `nginx-config subpath --base-path=/wiki`
  — renders the matching block from "Reverse proxy" below to a file. TLS
  certificate issuance itself (certbot etc.) stays manual — this only
  writes the proxy config that expects the cert to already exist at the
  path it references.
- `systemd install --target=HOST --with-backup-timer` / `systemd status`
  — the "systemd" and "Backup" sections below as a standalone,
  re-runnable, idempotent step.
- `setup-admin --target=HOST` — just `--create-admin`, keeping a real
  interactive TTY (never piped or scripted) for the password prompt.

## Real-hardware verification status

This section tells you what's actually been run on real devices, as
opposed to just believed to work — useful for deciding whether your own
board falls on the "verified" or "probably fine but untested" side of
the line below.

**Native build.** A full native build (`cmake --build` from scratch, no
cross-compilation) is verified on x86_64 Linux (Arch) and on a real
aarch64 SBC (an Orange Pi One Plus, 970 MiB RAM, Armbian/Debian trixie —
a 3 GiB swap file was added first, since vcpkg building Drogon/OpenSSL
this way is genuinely RAM-hungry at that size). On the SBC itself: a full
`git clone` + vcpkg bootstrap + `cmake --build`, `wiki-server` starting
and shutting down cleanly, and all 1619 unit test assertions passing
natively. Compiling on-device took substantially longer than the x86_64
dev-machine build, as expected (see "Path A" below). The extra swap
mattered only lightly in practice — peak observed usage stayed well
under the board's 970 MiB, and swap was barely touched — but it's still
the right precaution to take beforehand, since this build is genuinely
heavier than this board's bare RAM reliably covers.

**Cross-compiled binaries**, deployed to and verified on two separate
live devices:

- **armv7l, Debian 9 (stretch, EOL, glibc 2.24)** — via
  `arm-linux-musleabihf` + musl + static linking (see "Path B" below).
  `wiki-server`/`wiki-mcp` run natively there (not emulated), `unit_tests`
  passes under `qemu-arm-static`, and the full
  login → CSRF → document creation → atomic disk write → FTS5 search
  (with snippet highlighting) cycle works over live HTTP against a real
  systemd unit, running alongside live nginx/Samba/NFS/ProFTPD/
  mosquitto/munin on the same box with no impact on any of them.
- **aarch64, Armbian/Debian trixie, kernel 6.18 (the same Orange Pi One
  Plus, with `CONFIG_COMPAT` not set — a 32-bit binary can't even
  `exec()` there)** — via a new `aarch64-linux-musl` target
  (`cross/aarch64-musl/`, see "Porting to a different board" below). All
  1619 unit test assertions pass running directly on the device, with no
  qemu needed at all — real hardware beats emulation when you have it.
  Both the nginx-fronted reverse-proxy shape *and* standalone TLS
  ([tls] in config.toml, self-signed cert, real `setfacl`-granted ACL
  access) were deployed and verified end-to-end on this same device:
  HTTPS termination, HSTS, `Secure` cookies, a live certbot-style cert
  rotation picked up by `CertWatcher` with no restart (confirmed via a
  changed TLS fingerprint while the process PID stayed the same), and —
  the actual security property standalone TLS exists to guarantee — a
  spoofed `X-Real-IP` against `/mcp` does **not** reset the remote-MCP
  rate limiter in that mode. `[llm]` cloud chat (`CloudChatClient`
  against the real Anthropic API, with the key delivered via the
  `tools/wiki-ops.sh secrets` mechanism) was also exercised end-to-end
  over the standalone-TLS connection, with a real model reply streamed
  back over SSE. Nothing in the code carries an x86-specific assumption.

**`container-arm`** (a `buildx`-built, QEMU-emulated ARM image, deployed
via `docker compose` rather than as a plain binary) runs on the same real
aarch64 SBC, with the image loaded via `docker save`/`docker load` rather
than pulled from a registry. Verified end-to-end as a passwordless-sudo,
non-root, non-`docker`-group SSH user, over live HTTP against the running
container: `--create-admin`, login, document create/update,
`/api/search` (including FTS5 snippet highlighting), fail-safe-private
visibility gating (an anonymous caller gets `404`, not `403`, for a
private document, so its existence isn't revealed), and a live
`VaultWatcher` pickup of a file written directly to the
host-bind-mounted `vault_data`, with no `--reindex` call in between.

The plain `container` variant remains unverified on real ARM hardware
(`container-native` below *is* verified there).

**`cross-container`** (the same aarch64-musl cross build as above, but
produced inside `cross/Dockerfile.builder` instead of needing zig
installed on the build machine) runs as a plain binary+systemd install
on the same aarch64 SBC. A non-default install root and a non-default
port are both fully supported — the shipped systemd units and the
post-restart health check both correctly pick up either override.
Verified live against exactly that combination: unit tests pass under
`qemu-aarch64-static` (1621 assertions, 315 cases), and the same
login/create/update/search/visibility-gating cycle as `container-arm`
above, against the newly-deployed binary.

**The plain `container` variant** (host architecture, no QEMU involved)
is built, verified, and run via `docker compose` on x86_64 — the full
`ctest` suite (unit tests, `security_e2e`, both stress scripts) runs and
passes inside the Docker build itself, before the image is even tagged.
It was exercised end-to-end the same way as the ARM variants above
(`--create-admin`, login, document create/update, search,
fail-safe-private visibility gating, and a `VaultWatcher` pickup of a
file written directly into the host-bind-mounted `vault_data`). This
variant has no cross-compilation, QEMU emulation, or install-root
indirection involved at all, which makes it the simplest of all the
paths in this doc to get right.

**`container-native`** (a buildx+QEMU-built `linux/arm64` binary,
dynamically linked against the build image's own glibc — not static like
`cross`/`cross-container`) deploys as a plain binary+systemd install on
the same aarch64 SBC, exercising the same `cmd_systemd_install`
install-root and health-check-port substitution as `cross-container`, on
a second, independently-built binary. The full `ctest` suite runs for
this exact `arm64` build inside the Docker build step itself (a cached
layer shared with `container-arm`'s own `arm64` build of the same
source). Verified live, end-to-end: `--create-admin`, login, document
create/update, search, fail-safe-private visibility gating, and a
`VaultWatcher` pickup of a file written directly to the host
`vault_data`.

## Recording your own live deployment target

Keep the host/IP, install root, public URL (and reverse-proxy subpath,
if any), and the systemd unit/user running it in a private note outside
this repo — not in version control, since a public git history is the
wrong place for a real deployment's identifying details. Here's the
shape to use, with placeholder values (an RFC 5737 documentation IP, an
`.example.com` domain):

- Host: `root@192.0.2.10` (`wiki.example.com`, armv7l/sunxi, Debian 9
  stretch).
- Install root: `/opt/wiki` (`bin/`, `static/`, `config.toml`,
  `vault_data/`).
- Public URL: `http://192.0.2.10/wiki/` (nginx `default` vhost,
  prefix-stripping `proxy_pass` to `127.0.0.1:8080` — see "Reverse
  proxy" below; check directly on the instance whether
  `[server].base_path` is set — it's optional, and closes one specific
  edge case).
- systemd unit: `wiki.service`, `User=wiki`.

An old or weak target device's toolchain is the reason to cross-compile
from a dev machine, rather than run `cmake --install` natively on the
device — see "Update" below for the native-build path on a device
capable of it.

## Three ways to get this running on the device

| | Native build (on-device) | Cross-compile (from dev machine) | Docker |
|---|---|---|---|
| When to use | Modern distro, capable enough CPU/RAM, time to spare | Old distro (no C++20 compiler available), weak CPU, or just don't want to burn hours on-device | Capable, modern-enough device (Pi 4/5, 64-bit OS) where you'd rather not manage a toolchain at all |
| Toolchain | The device's own GCC/Clang ≥ C++20 | [zig](https://ziglang.org/) (`zig cc`/`zig c++`), bundles its own musl libc + libc++ | Whatever `docker build` pulls in, entirely inside the image |
| Output | Dynamically linked against the device's own glibc | Fully static (`-static`), zero runtime dependency on the target's libc | A container image; the device's own userland is untouched |
| Verified live | Yes — see "Real-hardware verification status" above | Yes — see "Real-hardware verification status" above | `container-arm`/`container-native` yes, on real ARM SBC hardware — see above; the plain `container` variant not yet on ARM specifically (built/run and verified on x86_64 — see `docs/docker.md`) |

Pick **native** if the device is reasonably capable and current
(Raspberry Pi OS Bookworm+, a recent Debian/Ubuntu ARM64). Pick
**cross-compile** if the target is old, weak, or end-of-life (e.g.
Debian 9 stretch on 32-bit ARM, glibc 2.24) — a modern glibc
cross-toolchain would link against a newer glibc than the target has and
fail at runtime with `GLIBC_2.XX not found`; a static musl binary
sidesteps that entirely by not touching the target's libc at all.

**Docker** sits between the two: it needs a device modern enough to run
a current Docker Engine in the first place, which rules it out for
exactly the old/weak targets cross-compilation exists for — but if the
device does qualify, it trades a from-scratch native build for a
`docker build` and skips toolchain management entirely. The rest of this
runbook covers only the native/cross-compile paths in detail; see
`docs/docker.md` for the Docker one.

## Path A — Native build, on the device

### Prerequisites on the device

- A C++20-capable compiler (verified against GCC 16.2.1; GCC has
  supported C++20 since version 10, so newer is safer)
- CMake ≥ 3.21, Ninja
- git, curl
- Python 3 (for the `ctest` security integration test; not needed at
  runtime)
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

Expect this to take substantially longer than on a desktop — building
Drogon+OpenSSL+trantor from scratch is not a quick job on a weak SBC.
It's a one-time cost per device/OS image, though.

## Path B — Cross-compile from an x86_64 dev machine

### Toolchain setup (one-time, on the dev machine)

Install [zig](https://ziglang.org/) (any recent 0.16.x release works;
the exact `code=139` linker bug described below is confirmed against
0.16.0 specifically, but the fix applies regardless). The repo already
ships the cross-compilation scaffolding under `cross/`:

- `cross/arm-musl/{cc,c++,ar,ranlib}` — thin wrapper scripts pinning the
  target (`arm-linux-musleabihf`, `-mcpu=generic+v7a`).
- `cross/arm-musl/toolchain.cmake` — the CMake toolchain file.
  `CMAKE_FIND_ROOT_PATH` is deliberately left unset here; pass it at
  configure time instead (below), so this file stays reusable without a
  hardcoded absolute path.
- `cross/arm-musl/arm-musl.cmake` — the vcpkg overlay triplet (static,
  release-only).
- `cross/overlay-ports/{brotli,libuuid}` — patched vcpkg ports that skip
  building CLI/test executables that hit a zig/lld linker bug (see
  "Known issues" below).

### Cross-build the dependencies

```sh
cd vcpkg
./vcpkg install --classic --triplet arm-musl \
  --overlay-triplets=../cross/arm-musl --overlay-ports=../cross/overlay-ports \
  --x-install-root=../vcpkg_installed_arm \
  drogon sqlite3[core,fts5,json1] argon2 nlohmann-json md4c yaml-cpp \
  tomlplusplus catch2
cd ..
```

This uses vcpkg's classic mode, not manifest mode, because
`drogon[ctl]` isn't supported on a cross target — the `ctl`
code-generator tool needs to run on the build host, not the target — so
the `ctl` feature is deliberately omitted here. An already-built
x64-linux `drogon_ctl` gets passed to the project's own configure step
instead (see below), reusing the host tool.

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

`DROGON_CTL_COMMAND` needs a native x64-linux `drogon_ctl` built
beforehand. The simplest way to get one is to have already done a normal
native build in `build/` (Path A on the dev machine itself), or just run
`vcpkg install drogon[ctl]` for the host triplet.

`PKG_CONFIG_LIBDIR` (not `PKG_CONFIG_PATH`, which only appends) fully
replaces pkg-config's search path, so it can't accidentally resolve a
host x86_64 `.pc` file instead of the cross-built one.
`PKG_CONFIG_SYSROOT_DIR=""` avoids prefixing paths that are already
correct.

### Verify BEFORE shipping anything to real hardware

Install `qemu-user-static` (which provides `qemu-arm-static`) on the dev
machine and actually run the cross-compiled binaries under emulation —
compiling cleanly is not the same as working:

```sh
qemu-arm-static ./build-arm/tests/unit_tests
# must report e.g. "All tests passed (N assertions in M test cases)" — not just exit 0

qemu-arm-static ./build-arm/wiki-server --create-admin
# real smoke test: actually creates an admin row via a real ARM binary
```

Only copy the binaries to the target once both of those pass cleanly.

### Known issues hit during cross-compilation (and their fixes)

- **zig 0.16.0's bundled lld `SIGSEGV`s (`code=139`) when statically
  linking many `.a` archives for `arm-linux-musleabihf`.** The root
  cause isn't archive count or parallelism — it's one specific flag,
  `-Xlinker --dependency-file=...`, which CMake's Ninja generator
  (≥3.20) adds automatically for linker-level dependency tracking. That
  flag crashes lld for this target 100% of the time, deterministically.
  Already fixed in `cross/arm-musl/toolchain.cmake`:
  `set(CMAKE_LINK_DEPENDS_USE_LINKER OFF)` makes CMake fall back to
  non-linker-based dependency tracking, with no other effect.
- **Linking a small executable against a single `.a` crashes the same
  way**, for brotli's CLI tool and libuuid's `test_uuid` — the same
  underlying lld bug, just a different trigger shape. Fixed via the two
  overlay ports under `cross/overlay-ports/`, which skip building those
  specific executables (neither is actually needed — only the libraries
  are).
- **CMake/Ninja auto-enable C++20 module dependency scanning**
  (`clang-scan-deps -format=p1689`) whenever Clang+C++20+Ninja are all in
  play, regardless of whether the project actually uses modules (it
  doesn't). The *system* `clang-scan-deps` can't resolve zig's bundled
  libc++ for a foreign `--target`, so it fails with false
  `<string>`/`<vector>`/etc. "file not found" errors, even though normal
  compilation finds those headers fine. Already fixed:
  `set(CMAKE_CXX_SCAN_FOR_MODULES OFF)`.

### Porting to a different board (different CPU architecture)

**First, check whether you need this section at all.** The existing
`cross/arm-musl` target is armv7 (32-bit ARM, Cortex-A7-class or newer,
hardfloat) — if your board is *also* armv7/armhf, the already-built
binary (or your own build from the exact recipe above, unmodified) just
runs, no porting needed. If your board is x86_64, skip cross-compilation
entirely and use the plain native build recipe under "Path A" — `cross/`
isn't involved there at all. This section is only for a genuinely
different CPU architecture — most commonly aarch64/arm64, the 64-bit
mode most current-generation SBCs (including newer Raspberry Pi boards)
actually ship by default.

**Everything target-specific lives in exactly four files**, all under
`cross/arm-musl/`. Copy that whole directory to
`cross/<your-triplet-name>/` and edit only what's below —`ar`/`ranlib`
need no changes at all, since they're architecture-agnostic wrappers
around `zig ar`/`zig ranlib`:

1. **`cc`/`c++`** — change the `-target` string to your architecture's
   zig target triple (`zig targets` lists what your installed zig
   supports). `arm-linux-musleabihf` becomes, for example,
   `aarch64-linux-musl` for a 64-bit ARM board. Drop
   `-mcpu=generic+v7a` unless your new target has an equivalent reason
   to pin a baseline CPU feature set — that flag exists specifically for
   armv7's own fragmented Cortex-A/ARMv7 feature story, it isn't a
   generic requirement.
2. **`toolchain.cmake`** — change `CMAKE_SYSTEM_PROCESSOR` and both
   `CMAKE_C_COMPILER_TARGET`/`CMAKE_CXX_COMPILER_TARGET` to match. Leave
   `CMAKE_LINK_DEPENDS_USE_LINKER OFF` and `CMAKE_CXX_SCAN_FOR_MODULES OFF`
   in place for your first attempt — both work around zig 0.16.0 bugs
   found on armv7 that are plausibly generic to zig's cross-linking
   path, though unverified against a second architecture. If your build
   links cleanly without them, drop them; if it `SIGSEGV`s identically
   to the documented armv7 bug above, the fix is the same one line.
3. **The vcpkg triplet file** (`arm-musl.cmake` → `<your-name>.cmake`) —
   change `VCPKG_TARGET_ARCHITECTURE` to vcpkg's own name for your
   architecture (`arm64` for aarch64, `x64` for x86_64, `riscv64` for
   RISC-V — see vcpkg's own triplet docs for the authoritative list).
   Keep `VCPKG_CRT_LINKAGE`/`VCPKG_LIBRARY_LINKAGE` as `static`, and
   keep the `VCPKG_CHAINLOAD_TOOLCHAIN_FILE` line pointing at your new
   `toolchain.cmake`.
4. **`cross/overlay-ports/`** (brotli, libuuid, md4c) — these aren't
   triplet-specific; the same `--overlay-ports=cross/overlay-ports` flag
   applies regardless of which triplet you build. Whether the specific
   patches in them (a brotli CLI-binary link crash, a libuuid CLI/test
   build issue) are still necessary for your new architecture is
   unverified — they were found on armv7, not derived from a spec. Try
   building without them first, and patch further only if you hit a
   matching crash.

Then run the same recipe as the armv7 instructions above, pointed at
your new triplet/toolchain file instead. The same mandatory step
applies: `qemu-<your-arch>-static ./build-<name>/tests/unit_tests` must
pass every case, not just avoid crashing, before the binary goes near
real hardware — or, if you actually have the real board, skip qemu and
run the test binary there directly. That's strictly better than
emulation, and it's what verified the aarch64-musl target below.

**`cross/aarch64-musl/` is exactly this recipe already done** — built
and verified running natively (not under emulation) on a real aarch64
target (the Orange Pi One Plus, Armbian/Debian trixie, kernel 6.18,
`CONFIG_COMPAT` not set — meaning a 32-bit `arm-musl` binary can't
execute there at all, `ENOEXEC`, not just "runs slower"). It uses
`aarch64-linux-musl`, the exact target string this section already used
as its own worked example above, with `-mcpu=generic` (no `+v7a`-style
baseline needed) and `VCPKG_TARGET_ARCHITECTURE arm64`. All 1619 unit
test assertions passed running directly on the device, with no qemu
involved. Use `--triplet=aarch64-musl --cross-dir=cross/aarch64-musl`
with `build cross`/`deploy`.

**One alternative worth considering before cross-compiling at all:** the
musl+static approach exists specifically to dodge `GLIBC_2.XX not found`
against an old target OS (Debian 9 stretch, glibc 2.24, in this
project's real deployment). If your board runs a reasonably current
Linux distro, a plain glibc cross-toolchain — or even a native
on-device build, if the board has enough RAM/storage/patience for an
hours-long Drogon+OpenSSL build — may work and sidesteps the whole
`cross/` mechanism entirely. A native on-device build on a weaker board
hasn't been tried for this project specifically (see "Real-hardware
verification status" above).

## Install (Path A or B — Docker doesn't use this step, see `docs/docker.md`)

```sh
sudo cmake --install build --prefix /opt/wiki       # or build-arm for a cross-build
```

This places `bin/wiki-server`, `bin/wiki-mcp`, `static/`,
`config.example.toml`, and
`share/wiki/systemd/{wiki.service,wiki.env.example}` under `/opt/wiki`.
It does **not** create a working `config.toml` — only the example — so a
real config never silently materializes from defaults.

For a cross-build, `cmake --install` still runs on the dev machine
(against `build-arm`), into a local staging prefix, which then gets
transferred as a whole tree (e.g. `tar czf` + `scp`) to the target —
there's no `cmake --install` step happening on the target itself.

## First-time setup on the target

```sh
sudo useradd --system --home-dir /opt/wiki --shell /usr/sbin/nologin wiki
sudo chown -R wiki:wiki /opt/wiki

cd /opt/wiki
sudo -u wiki cp config.example.toml config.toml
```

Edit `config.toml` as needed. These are the fields that actually matter
for a real deployment:

```toml
[server]
listen_addr = "127.0.0.1"   # keep on loopback; a reverse proxy handles TLS/public exposure
port = 8080
threads = 2
# base_path = "/wiki"       # OPTIONAL — see "Reverse proxy" below
# theme = "classic"         # OPTIONAL — which of classic/dark/green a fresh browser
                             # (nothing picked yet) lands on; see config.example.toml's
                             # own comment.

[vault]
path = "./vault_data"       # relative to the working directory (WorkingDirectory= in the unit)

[mcp]
scope = "admin"             # "admin" sees public+private (local spawn by the owner); "public" for any future remote transport
```

Restart the service after changing either `base_path` or `theme`.

Create the admin account:

```sh
sudo -u wiki ./bin/wiki-server --create-admin
```

This prompts for a username and password interactively, with the input
not echoed and nothing logged. The username isn't fixed to "admin" — it's
whatever gets typed here. Save the password before closing the terminal:
running this command again overwrites *both* the username and password
together, which is also the only recovery path if the password is lost.
SQLite's single enforced admin row (id=1) is the one source of truth for
it — there's no `config.toml` knob and no "show me the current password"
escape hatch.

## systemd

```sh
sudo cp /opt/wiki/share/wiki/systemd/wiki.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now wiki.service
sudo systemctl status wiki.service
```

The shipped unit is already hardened: `ProtectSystem=strict`,
`NoNewPrivileges=yes`, `ProtectHome=yes`, `PrivateTmp=yes`,
`ReadWritePaths=/opt/wiki/vault_data` (the only place the service
writes), plus an empty `CapabilityBoundingSet=`,
`RestrictAddressFamilies`, `SystemCallFilter=@system-service`,
`MemoryDenyWriteExecute`, and the other `Protect*`/`Restrict*`
directives (see `systemd/wiki.service`'s own comments for what each one
covers). With `CapabilityBoundingSet=` empty, the process loses
`CAP_DAC_OVERRIDE` even running as `root` — file ownership and
permissions under `ReadWritePaths` have to be exactly right, since the
capability bounding set provides no privilege fallback here. Run
`systemd-analyze security wiki.service` to see its score, and exercise
the app once after any change to this unit's hardening (login, create a
document, search, upload an attachment) — a hardening regression here
fails at runtime, as a specific broken route, not at `daemon-reload`
time.

`ReadWritePaths=/opt/wiki/vault_data` already covers Drogon's own
internal upload-buffering directory too (used to stage large multipart
request bodies to disk, independent of the app's own attachment
storage) — it's pointed at `<vault_path>/.uploads-tmp` (an absolute path,
computed at startup), which falls under the same `ReadWritePaths` entry
and is skipped by `IndexBuilder`'s existing "skip `.git`/`.trash`/
anything-dot" rule, so it's never mistaken for a document. There's
nothing to configure manually for this.

`EnvironmentFile=-/etc/opt/wiki/wiki.env` is optional (note the leading
`-`) — the file is allowed to simply not exist. The runtime secret
readers are `CloudEmbeddingProvider` and `CloudChatClient`: each calls
`getenv()` on the variable named by `[embeddings].api_key_env` /
`[llm].api_key_env` in `config.toml` (see `systemd/wiki.env.example`,
`docs/embeddings.md`, `docs/llm.md`). For embeddings
`provider = "none"`/`"local"` and LLM `provider = "none"`, or a loopback
OpenAI-compatible server that doesn't authenticate, this file can stay
absent. Admin credentials live in SQLite; sessions are unsigned random
tokens with no secret-based signature.

## TLS / public internet access

`wiki-server` doesn't terminate TLS itself by default. For access
outside the local network, put a reverse proxy (nginx/Caddy/traefik) in
front of it to handle TLS and proxy to `127.0.0.1:8080` (or whatever
`config.toml` specifies) — see "Reverse proxy" below. Without that step,
keep `listen_addr = "127.0.0.1"` and don't expose the port directly.
This is comment-only guidance, not something `AppConfig::load()`
enforces — the Docker image's own `docker/config.docker.toml`
deliberately sets `listen_addr = "0.0.0.0"` with no `[tls]` table at
all, since the container's network namespace, not this value, is what
actually controls real exposure there. There's no single rule that's
correct for every deployment shape, so this stays the deploying admin's
own judgment call.

The one exception is standalone TLS mode, below — `wiki-server` *can*
terminate TLS itself, as a second, equally-supported option to running
it behind nginx.

## Standalone TLS (no reverse proxy)

For a single-purpose box that only ever runs this app, `wiki-server` can
terminate TLS itself instead of needing nginx as a second moving part.
Set in `config.toml`:

```toml
[server]
listen_addr = "0.0.0.0"   # or a specific interface — no longer loopback-only

[tls]
enabled = true
cert_file = "/etc/letsencrypt/live/example.com/fullchain.pem"
key_file = "/etc/letsencrypt/live/example.com/privkey.pem"
```

A background watcher (`src/server/CertWatcher`) detects a certbot
renewal (an atomic symlink swap inside that `live/<domain>/` directory)
and reloads the cert/key into the already-running process, with no
restart needed. It re-validates both files as parseable PEM *and* that
the key actually matches the certificate before ever reloading them, so
a transiently half-written file — or the two symlinks updating a moment
apart from each other mid-renewal — gets skipped (logged, retried on the
next change) rather than risking a crash.

**Permission gotcha:** `/etc/letsencrypt/{live,archive}` is root-only
(`0700`) by default, and `wiki-server`'s own run-as user needs read
access or it can neither start nor pick up renewals. A one-time ACL
survives every future renewal (it's a symlink swap inside the same
directory tree each time, not a fresh directory), so it's less fragile
than a certbot deploy-hook that has to re-run on every renewal:

```sh
setfacl -R -m u:wiki:rx /etc/letsencrypt/live /etc/letsencrypt/archive
```

(substitute the actual user `wiki-server` runs as for `wiki`.)

`auth::clientIp()` (used by the remote-MCP rate limiter and IP
allowlist, see "Remote MCP" below) automatically stops trusting
`X-Real-IP`/`X-Forwarded-For` whenever `[tls].enabled` is true — there's
no separate setting for this. It's derived rather than configurable,
because there is by definition no reverse proxy in front in this mode:
`wiki-server` is the direct, sole TCP endpoint, so nothing legitimate
would ever send those headers, and `req->getPeerAddr()` is already
correct. `X-Accel-Buffering: no` (set by `AgentRoutes.cpp` on the
Draft/Chat SSE stream) is advisory to a reverse proxy only — it's simply
unused and harmless here, with no proxy around to read it.

## Reverse proxy

This section covers the still-default, still-fully-supported
nginx-fronted mode. `wiki-server`, by default, listens only on
`127.0.0.1:8080` and is meant to sit behind a reverse proxy for anything
beyond local access — see "Standalone TLS (no reverse proxy)" above for
the alternative where `wiki-server` terminates TLS itself instead.
Nothing here changes for an existing nginx-fronted deployment:
`clientIp()`'s header trust is derived from `[tls].enabled` (false in
this mode, exactly as it was before standalone TLS existed), not a
separate setting that needs updating.

### On its own (sub)domain

The simplest case — the app owns the whole (sub)domain, with no
path-prefix concerns:

```nginx
server {
    listen 443 ssl;
    server_name wiki.example.com;
    ssl_certificate     /etc/letsencrypt/live/wiki.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/wiki.example.com/privkey.pem;

    location / {
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
}
```

`client_max_body_size 0` is required for large attachment uploads (a
120 MB PDF, etc.) — nginx's default is 1m, which would `413` before
`wiki-server` ever saw the body.

### Under a subpath of an existing site (e.g. `/wiki`)

**No `config.toml` setting is needed for the common case.** The frontend
is a client-rendered static shell (`static/shell.html`, served with the
same body no matter what path it's requested from — see
`docs/architecture.md`). Its own inline bootstrap script infers the
mount prefix itself on load, by matching `location.pathname` against
this app's known route shapes (`/d/...`, `/edit/...`, `/search`, …) and
setting a `<base href>` from whatever came before that match. This works
correctly under any subpath, or none, automatically. Stylesheets in
`shell.html` itself are created by that same inline script *after*
`<base>` exists, not as static `<link href="css/...">` tags — otherwise
the HTML preload scanner would fetch them against the document URL
(`/wiki/edit/css/edit.css`) and get this SPA shell (`text/html`) back
instead of CSS.

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

No `sub_filter`, `proxy_redirect`, or response-rewriting of any kind is
needed on the nginx side — that whole class of workaround only applied
back when the backend templated pages server-side. The current
client-rendered shell has no use for it, regardless of whether
`base_path` below is set.

**One gap this inference can never close by pattern-matching alone:** a
path matching no known route (a typo, a stale `[[wiki-link]]`), on a
browser with nothing yet cached in `localStorage` either — no signal
survives client-side for that exact combination. The
`localStorage`-cached last-known-good prefix
(`wiki.lastKnownBasePath`) covers the realistic case (a broken link
clicked from an already-loaded page) completely, but a genuinely cold
start — landing directly on a stale link with nothing cached yet —
still degrades to an unstyled (never crashing) page.

**`[server].base_path = "/wiki"` in `config.toml` closes this
completely, optionally** (restart the service after setting it) —
`PageRoutes.cpp` then bakes that prefix into every served
`shell.html`, matched route or not, as an authoritative
`window.__WIKI_KNOWN_BASE_PATH__` that the bootstrap script trusts over
its own guessing. Skip this setting for a deployment on its own
(sub)domain, or if that one cold-start edge case is acceptable to leave
unstyled. The full reasoning is in `shell.html`'s own inline script
comment and `src/config/AppConfig.h`'s comment on `basePath`.

## Remote MCP

This is an admin-toggleable, bearer-token-protected `POST /mcp` HTTP
endpoint (see `docs/mcp.md`) that lets an MCP client reach this wiki over
the network, instead of only through a local stdio spawn. Enabling or
disabling it, write access, the token, and the IP allowlist are all
managed live from the Account page — no `config.toml` edit, no restart.

**This requires TLS in front of the app** — either the reverse proxy
above, or `wiki-server`'s own standalone TLS mode (see "Standalone TLS
(no reverse proxy)" above). The bearer token travels in a plain
`Authorization` header on every request, so over plain HTTP it's
readable by anything between the client and this box. If you're using
the reverse-proxy mode, put the same proxy this app already needs for
any public exposure in front of `/mcp` too — there's no separate
listener to configure; it's just one more route on the existing
`127.0.0.1:8080` upstream.

**The IP allowlist depends on the proxy setting the right headers
correctly** — this only matters in the nginx-fronted mode
(`[tls].enabled = false`). In standalone TLS mode, `clientIp()` already
returns the real caller from the raw TCP peer, with no header involved
at all, since there's by definition no proxy in front to have set one.
For the nginx-fronted mode, the exact directives already shown above for
the subpath case are what's needed:

```nginx
proxy_set_header X-Real-IP $remote_addr;
proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
```

Whenever `[tls].enabled = false`, `auth::clientIp()`
(`src/auth/ClientIp.h`) reads `X-Real-IP` first, falling back to
`X-Forwarded-For`'s *last* entry. Both of those details matter for the
same reason: `proxy_set_header` overwrites a header before forwarding it
upstream, so no client-supplied `X-Real-IP` survives that —
`$remote_addr` is nginx's own view of the TCP connection, which isn't
spoofable from the client side. `$proxy_add_x_forwarded_for`, on the
other hand, *appends* `$remote_addr` to whatever `X-Forwarded-For` the
client already sent — so that header's first entry is exactly what the
client claimed (trivially spoofable: a request with
`X-Forwarded-For: <an-allowlisted-ip>` would walk straight past an
allowlist that trusted the first entry), while the last entry is always
nginx's own append.

A deployment with a different proxy chain — more than one hop, or a
proxy that doesn't set `X-Real-IP`/doesn't use
`$proxy_add_x_forwarded_for` the same way — needs to verify its own
directives produce the same guarantee. A misconfigured proxy here
doesn't break the bearer-token check, only the IP allowlist's own
guarantee on top of it — the token is still the actual gate, so treat
the allowlist as a defense-in-depth layer, not the only thing standing
between the internet and this vault.

Whether remote MCP is currently enabled, and with what settings, isn't
tracked in this doc — check the Account page's Remote MCP section
directly; that's also the only place the raw bearer token is ever shown,
exactly once, at the moment it's generated
(`McpRemoteConfig::regenerateToken()`). To rotate it: Account page →
Remote MCP section → Regenerate (this invalidates the old value
immediately), or `POST /api/admin/mcp-remote-config/regenerate-token`
as admin.

## Backup

The vault directory (`[vault].path`, plus its `.trash/`) is the entire
source of truth. The SQLite index (`[index].db_path`) is fully
disposable — `wiki-server --reindex` rebuilds it from scratch — so
backing it up is optional; doing so just saves a rescan on restore for a
very large vault, nothing more. The minimum viable backup is just the
vault directory, regularly. There are two ways to actually do that:

**Ad hoc, from the web UI:** Account page (admin only) → "Download
backup." This hits `GET /api/admin/backup`
(`src/vault/BackupService.h`), which shells out to the system `tar`
binary (`fork()`+`execlp()` with an explicit argv, never
`system()`/`popen()`, so nothing about the vault path's own content can
be interpreted as shell syntax) and streams back a `.tar.gz` of the
whole vault — `.trash/` and the index db included,
`.uploads-tmp/` (Drogon's own transient upload-staging buffer) and
`.mcp-uploads/` (remote-MCP large-file tickets) excluded. This is useful
for a quick snapshot before a risky change, but it's not a substitute
for the automated path below, since it only works while the server and
its disk are both still alive.

**Automated, via a systemd timer**
(`systemd/wiki-backup.{service,timer}`, `systemd/wiki-backup.sh`). This
isn't installed or enabled by a plain `cmake --install` — opt in
explicitly:

```sh
sudo cp /opt/wiki/share/wiki/systemd/wiki-backup.{service,timer} /etc/systemd/system/
sudo mkdir -p /etc/opt/wiki
sudo cp /opt/wiki/share/wiki/systemd/wiki-backup.env.example /etc/opt/wiki/wiki-backup.env
sudo "$EDITOR" /etc/opt/wiki/wiki-backup.env   # set BACKUP_DIR to a DIFFERENT disk/mount, see below
sudo systemctl daemon-reload
sudo systemctl enable --now wiki-backup.timer
```

`wiki-backup.sh` tars `VAULT_PATH` straight off disk (with the same
`.uploads-tmp/` and `.mcp-uploads/` exclusion as the web UI button, and
the same atomic temp-file-then-rename discipline as
`VaultRepository`'s own document writes), then prunes down to
`RETENTION_COUNT` (default 14), oldest first, only after a new backup
has landed successfully. It deliberately doesn't go through
`wiki-server`/the HTTP endpoint above — no admin session or credentials
needed, and it keeps working whether the server is healthy, crashed, or
mid-restart.

**Point `BACKUP_DIR` at a disk or mount other than the one `VAULT_PATH`
lives on** — an external USB drive, a network share, another machine
over sshfs/NFS, anything that doesn't share the SD card's own failure
mode. `wiki-backup.service`'s `ProtectSystem=strict` only grants read
access to `/opt/wiki/vault_data` by default (see the unit file's own
comment) — an unusual `BACKUP_DIR` outside the paths systemd hardening
normally allows needs a `ReadWritePaths=` override in
`/etc/systemd/system/wiki-backup.service.d/`, not a loosening of the
shipped unit.

The timer defaults to `OnCalendar=daily`, `Persistent=true` (so a missed
run — e.g. the device was off — fires as soon as it's back up). Check it
landed with `systemctl list-timers wiki-backup.timer` and
`journalctl -u wiki-backup.service`.

## Update

`./tools/wiki-ops.sh update --target=HOST` runs this whole section in
one command — `git pull`, the matching `build`, then a
non-`--first-time` `deploy` — see `docs/wiki-ops.md`'s `update` section.
What follows is the same sequence done by hand.

```sh
cd wiki && git pull
cmake --build build -j"$(nproc)"                        # or build-arm for cross
ctest --test-dir build --output-on-failure               # before touching prod
sudo cmake --install build --prefix /opt/wiki             # or transfer a cross-build tree
sudo systemctl restart wiki.service
```

For a cross-compiled deployment, rebuild `build-arm`, re-verify under
`qemu-arm-static` (see "Verify BEFORE shipping anything to real
hardware" above), then ship just the two binaries plus `static/` rather
than running `cmake --install` on the target itself:

```sh
# 1. incremental cross-build (vcpkg_installed_arm/ and build-arm/ are both reused,
#    not recreated, on every subsequent deploy)
export PKG_CONFIG_LIBDIR="$PWD/vcpkg_installed_arm/arm-musl/lib/pkgconfig:$PWD/vcpkg_installed_arm/arm-musl/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=""
cmake --build build-arm -j"$(nproc)"
qemu-arm-static ./build-arm/tests/unit_tests   # must pass every case before shipping

# 2. ship the new binaries + static assets alongside the live ones (never
#    overwrite in place — an scp that dies mid-transfer must not leave a
#    half-written binary where systemd will find it on next restart)
scp build-arm/wiki-server build-arm/wiki-mcp root@<target>:/tmp/
scp -r static root@<target>:/tmp/static-new

# 3. on the target: back up what's live, strip + swap the new files in, restart, verify
ssh root@<target> '
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

**Strip the binaries on the target device itself, never on the dev
machine.** `CMAKE_BUILD_TYPE=RelWithDebInfo` keeps debug symbols in the
statically-linked ELF — `wiki-server` ships at roughly 116M,
`wiki-mcp` at roughly 61M. `strip --strip-all` drops that down to around
14M for `wiki-server` (roughly 8x smaller), with no behavior change. The
dev machine's own `strip` doesn't recognize this cross-compiled ARM
ELF's architecture, and `zig objcopy --strip-all` errors `unimplemented`
for this static-musl-ARM shape — the target's own native `strip` (real
GNU binutils, correct architecture) is what actually works. The
unstripped `build-arm/wiki-server`/`wiki-mcp` stay on the dev machine,
kept around for `addr2line`/`gdb` if a crash ever needs symbols.

The `.bak-$STAMP` copies are never cleaned up automatically — sweep old
ones by hand occasionally on a long-lived deployment.

## Static-assets-only redeploy (frontend change, no rebuild needed)

When the only thing that changed is under `static/` (CSS, JS,
`shell.html`) — nothing under `src/` touched, nothing to recompile or
cross-compile — the full "Update" flow above is overkill. From a dev
machine that isn't the target itself (the normal case for this project,
since binaries are cross-compiled on x86_64), push just the `static/`
tree over SSH instead:

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

**Never selectively `scp` "the files I changed."** A partial sync can
miss a file that also changed, and a stale JS/CSS file doesn't fail
loudly — nothing about the running site looks broken until someone goes
looking for a feature that should already be live. Tar and swap the
entire directory every time; at this project's size, the cost difference
is a couple of seconds.

**A restart is mandatory, not optional, even though it's "just static
files."** Most of `static/` is served fresh from disk on every request,
through Drogon's own static-file handler. `shell.html` is the one
exception: `PageRoutes.cpp` reads it once at process startup and caches
the built HTML in memory (`buildShellHtml`/`g_shellHtml`) — swapping the
file on disk without restarting the process means every request keeps
getting the old cached `shell.html` indefinitely, silently.

**Verify by md5-summing every single file afterward, not just the ones
you think changed:**

```sh
find static -type f -exec md5sum {} \; | sed 's|static/||' | sort > /tmp/local.md5
ssh root@<target> 'cd /opt/wiki/static && find . -type f -exec md5sum {} \;' \
  | sed 's|  \./|  |' | sort > /tmp/remote.md5
diff /tmp/local.md5 /tmp/remote.md5 && echo "identical" || echo "MISMATCH"
```

Use `s|  \./|  |`, not `s|^\./|  |` — `md5sum`'s own output puts two
spaces before the path, and `^\./` anchors at the start of the line (the
hash column), so it matches nothing, and the diff would silently show a
bogus per-path mismatch instead of catching real content differences.

**When testing a redeploy against a local throwaway instance**, kill the
old process by an exact, verified PID (`ps aux | grep wiki-server`, then
`kill -9 <pid>`) — not `pkill -f wiki-server` trusting its exit code
alone. A surviving old process keeps serving its own cached state
(including a stale `shell.html`) while the new `wiki-server` fails to
bind and exits immediately. Check its log for `FATAL Address already in
use` if a change appears to have had no effect.

## Troubleshooting checklist

- **`journalctl -u wiki.service` shows `Read-only file system` errors on
  startup** — something is trying to write outside
  `ReadWritePaths=/opt/wiki/vault_data`. This is already fixed for
  Drogon's own upload-buffer path (see "systemd" above); if it shows up
  again for a different path, either add that path to `ReadWritePaths=`
  in the unit, or, if it's app-generated, redirect it into the vault
  path the way `.uploads-tmp` already is.
- **`GET /` (or the subpath-prefixed equivalent) returns a 404** — this
  would be unexpected: `PageRoutes` registers `/` as the same shell as
  `/search`, and the client-side router renders search in place there
  with no bounce. Confirm `/healthz` and `/search` both return `200`
  before suspecting anything is actually broken.
- **CSS/JS 404, or the login form posts to the wrong path, when
  reverse-proxied under a subpath** — set `[server].base_path` (see
  "Reverse proxy" above) and restart; that closes this permanently.
  Without it, an unmatched path falls back to the last known-good prefix
  cached in `localStorage`, which is empty on a genuinely cold start (a
  first-ever visit in that browser landing directly on a subpath'd URL
  with nothing cached yet) — load `/search` (or any known route) first
  to warm the cache as a one-off workaround, if setting `base_path`
  isn't an option.
- **A static cross-compiled link crashes with `code=139`** — see "Known
  issues" above. This is almost certainly the
  `-Xlinker --dependency-file=...` / `CMAKE_LINK_DEPENDS_USE_LINKER`
  issue, if the toolchain file has drifted from
  `cross/arm-musl/toolchain.cmake`.
- **Locked out of the admin account** — there's no "recover the
  password" path by design, since only an argon2id hash is ever stored.
  Stop the service first, then re-run
  `sudo -u wiki ./bin/wiki-server --create-admin` on the target (this
  overwrites the single admin row), and start the service back up.
  Running `--create-admin` concurrently against a live service's SQLite
  connection isn't supported.
