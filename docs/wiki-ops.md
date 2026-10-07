# tools/wiki-ops.sh — Build and Deploy Reference

`tools/wiki-ops.sh` automates the manual flow in `docs/sbc-deployment.md` — every
subcommand below is a thin wrapper around an exact sequence from that doc (or, for
`cross-container`/`container-arm`, newer infrastructure under `cross/` and the main
`Dockerfile`, parameterized by `TARGETARCH`/`TARGETVARIANT`). This document is the
detailed reference, with every flag and worked examples; `docs/sbc-deployment.md`
stays the explanation of *what each step actually does and why* — read that first if
something here doesn't make sense, or if you're working without the script at all.
`./tools/wiki-ops.sh help` always reflects the script's current, real flag set faster
than this doc can — trust that output over this doc if they ever disagree.

## Resolution order

Every value the script needs (host, install root, port, …) is resolved in this order,
stopping at the first one that supplies it:

1. A command-line flag (`--target=...`).
2. `deploy.local.env` in the repo root — gitignored, never committed. Create it with
   `./tools/wiki-ops.sh config init` (copies `deploy.local.env.example`), then edit it,
   or just leave fields blank and let step 3 fill them in per run.
3. An interactive prompt, with a shown default — only when both stdin and stdout are a
   real terminal.
4. A hard default (e.g. `8080` for the port, `/opt/wiki` for the install root).

A value whose hard default is legitimately empty (`WIKI_DEPLOY_HOST` — blank means "no
ssh, install locally") never reaches an error; it just resolves to blank. Everything
else errors out plainly if nothing above supplies it and there's no terminal to ask.

Secrets (an embeddings/LLM API key's actual *value*, not its env-var *name*) never
flow through this resolver or `deploy.local.env` at all — see "Secrets" under `deploy`
below.

## Getting started

```sh
./tools/wiki-ops.sh config init          # copies deploy.local.env.example
$EDITOR deploy.local.env                 # fill in what you know now; the rest gets asked per run
./tools/wiki-ops.sh config show          # see what's actually set (comments/blanks stripped)
```

A filled-in example for a real armv7 Raspberry Pi reached over ssh:

```sh
WIKI_DEPLOY_HOST=root@192.0.2.10
WIKI_DEPLOY_SSH_PORT=22
WIKI_DEPLOY_INSTALL_ROOT=/opt/wiki
WIKI_DEPLOY_VARIANT=cross
WIKI_DEPLOY_SERVICE_USER=wiki
WIKI_DEPLOY_DOMAIN=wiki.example.com
WIKI_DEPLOY_BASE_PATH=
WIKI_DEPLOY_PORT=
WIKI_DEPLOY_BACKUP_DIR=/mnt/usb-backup
WIKI_DEPLOY_WITH_BACKUP_TIMER=true
WIKI_DEPLOY_EMBEDDINGS_API_KEY_ENV=
WIKI_DEPLOY_LLM_API_KEY_ENV=
```

With this file in place, `./tools/wiki-ops.sh deploy --first-time` needs no flags at
all — everything it needs is already answered.

## Which variant for which hardware

Same decision as `docs/sbc-deployment.md`'s own table, mapped onto `build`/`deploy
--variant=`:

| Target | Variant | Needs on the dev machine | Needs on the target | Build time |
|---|---|---|---|---|
| Modern 64-bit SBC (Pi 4/5 Bookworm+) | `native` (build *on* the device) | — | C++20 compiler, CMake, Ninja | Short (host-speed compile) |
| Old/weak/32-bit SBC (Pi 2/3 armhf, generic armv7) | `cross` | zig, a native `build/` already done once | nothing but the shipped binary | Short — zig cross-compiles at host speed, no emulation involved |
| True ARMv6 (Pi 1/Zero/Zero W) | `cross --triplet=armv6-musl --cross-dir=cross/armv6-musl` | same as `cross` | nothing | Short, same reason as `cross` |
| Same as `cross`, but no local zig/vcpkg wanted | `cross-container` | only Docker | nothing | Short, same reason as `cross` |
| x86_64/arm64 desktop, NAS, cloud VM with modern Docker | `container` | Docker | Docker | Short if built for the host's own architecture (no `--platform` override) |
| Pi 4/5 (or any arm64/armv7 box) you'd rather not cross-compile for, via Docker | `container-arm` | Docker + buildx (+ QEMU binfmt for cross-arch) | Docker | **Very long — see the `container-arm` note below** |
| x86_64 (or arm64 via `--platform`) target that should run a bare binary, but you'd rather not install vcpkg/cmake/ninja locally | `container-native` | Docker (+ buildx only if `--platform` overrides this host's own arch) | nothing — no Docker needed | Short with no `--platform`; with `--platform` targeting a non-host arch, same cost as `container-arm` (very long) |

## `build`

```
build native|cross|container|cross-container|container-arm [--skip-tests]
      [--triplet=NAME] [--cross-dir=PATH] [--platform=linux/arm/v7]
      [--local-embeddings] [--cloud-embeddings] [--dry-run]
```

**`--local-embeddings`/`--cloud-embeddings`** toggle the only two real build-time
feature flags that exist (`CMakeLists.txt`'s `WIKI_ENABLE_LOCAL_EMBEDDINGS`/
`WIKI_ENABLE_CLOUD_EMBEDDINGS`, both `OFF` by default — see `docs/embeddings.md`).
They're independent, not mutually exclusive — pass either, both, or neither. **There
is no LLM/chat build flag** — the Draft panel and sidebar Chat are always compiled in,
toggled purely at runtime via `config.toml`'s `[llm]` section (`provider = "none"`
hides them); see `docs/llm.md`. `WIKI_ENABLE_FUZZING` (the one other `option()` in
`CMakeLists.txt`) is a dev-only `tests/fuzz/` flag, needs Clang specifically, and has
no place in a build/deploy flow — not exposed here.

For `native`/`cross`, these just append the matching `-DWIKI_ENABLE_...=ON` to the
existing `cmake` configure line. For `container`/`container-arm`/`cross-container`,
Docker has no equivalent of a cache flag — `wiki-ops.sh` passes
`--build-arg WIKI_CMAKE_EXTRA_ARGS="-DWIKI_ENABLE_...=ON ..."`, and both `Dockerfile`
and `cross/Dockerfile.builder` declare a matching `ARG WIKI_CMAKE_EXTRA_ARGS=`
appended verbatim to their own `cmake` configure line.

**Real asymmetry in risk, not just a formality**: `--cloud-embeddings` is just an
HTTP client (OpenSSL, already a transitive Drogon dependency on every triplet/
toolchain this project uses) — low-risk anywhere. **`--local-embeddings` pulls
llama.cpp via CMake `FetchContent` and builds it from source — UNVERIFIED under the
zig toolchain (`cross`/`cross-container`)**: llama.cpp's own CMake has hardware-
feature-detection assumptions that have never been exercised against zig's
cross-compilation path here. `build cross`/`build cross-container
--local-embeddings` print a warning saying exactly this; it is not a solved problem,
just a flag that's now possible to pass.

- **`native`** — `cmake configure+build(+ctest)` into `build/`, exactly
  `docs/sbc-deployment.md`'s Path A. `--skip-tests` skips the `ctest` run.
- **`cross`** — Path B. Requires a native `build/` to already exist
  (specifically `build/vcpkg_installed/x64-linux/tools/drogon/drogon_ctl` — run `build
  native` at least once first; classic-mode cross builds reuse this host tool instead
  of cross-building Drogon's `ctl` code-generator). `--triplet=NAME` picks a different
  `cross/<name>/` toolchain — default `arm-musl` (armv7); `--cross-dir=PATH` overrides
  which directory to read it from, but defaults to `cross/$triplet` automatically (the
  established convention — every `cross/<name>/` directory is named after its own
  triplet), so you only ever need to pass `--triplet` alone. For a true ARMv6 target
  (Raspberry Pi 1/Zero/Zero W):
  ```sh
  ./tools/wiki-ops.sh build cross --triplet=armv6-musl
  ```
  **Output directory tracks the triplet**: `build-arm/` for the default `arm-musl`
  (preserving the long-documented name so nothing that already assumes it breaks),
  `build-$triplet/` for anything else (e.g. `build-armv6-musl/`) — switching triplets
  never overwrites a different target's own build output. `verify`/`deploy` compute
  the same directory name the same way, from the same `--triplet` (or
  `WIKI_DEPLOY_TRIPLET` in `deploy.local.env`) — see `deploy` below.
- **`container`** — `docker compose build`, the existing x86_64/arm64 image, unchanged.
- **`cross-container`** — builds `cross/Dockerfile.builder` (zig + vcpkg + the
  `cross/<name>/` toolchain `--triplet` names — default `arm-musl` — entirely inside
  the image, no local zig/vcpkg install needed) and extracts
  `wiki-server`/`wiki-mcp`/`unit_tests` into `build-arm-container/` (or
  `build-$triplet-container/` for a non-default triplet, same naming rule as `cross`
  above). `--cross-dir` has no effect here (the Dockerfile always derives
  `cross/${triplet}` itself, matching the one real convention this repo uses) —
  only `--triplet` matters:
  ```sh
  ./tools/wiki-ops.sh build cross-container --triplet=armv6-musl
  ```
- **`container-arm`** — `docker buildx build --platform <platform> --load .` against
  the main `Dockerfile` (parameterized by `TARGETARCH`/`TARGETVARIANT` — see its own
  header comment). `--platform` defaults to `linux/arm/v7`; `linux/arm64` and
  `linux/amd64` also work (tagged `personal-wiki:arm64`/`personal-wiki:amd64` — only
  the *default* platform keeps the plain `personal-wiki:arm` tag that
  `docker-compose.arm.yml`/`deploy --variant=container-arm` expect). Needs a buildx
  builder (`docker buildx create --use`) and, for an architecture that isn't this
  host's own, QEMU binfmt registered (`docker run --privileged --rm tonistiigi/binfmt
  --install all`, one-time per host).
  **Expect this to run very long for `--platform=linux/arm/v7` (or any architecture
  other than this host's own) — this is not a fluke, every step genuinely runs under
  QEMU emulation**, including the apt package installs, vcpkg's own dependency builds
  (Drogon, OpenSSL, trantor, ...), and the project's own compile. `linux/arm/v7`
  specifically has no prebuilt `vcpkg-tool` binary upstream, so vcpkg builds its own
  tool from source under emulation before installing a single dependency — confirmed
  to take a large chunk of that time by itself. `cross`/`cross-container` exist as the
  fast alternative for exactly this target: zig cross-compiles at native host speed
  with no emulation anywhere in the pipeline, which is why those variants stay short.
- **`container-native`** — build inside Docker, deploy a bare binary with no Docker on
  the target. `docker build --target build .` stops at the main `Dockerfile`'s first
  stage, which already ran a full `cmake --install build --prefix /opt/wiki` (same
  install() rules `native` itself runs) — `wiki-ops.sh` extracts that whole tree via
  `docker create`+`docker cp`, no `--output`/buildx needed for the default case. No
  `--platform` given: builds for whatever architecture this Docker daemon's own host
  is (plain legacy builder, matches `Dockerfile`'s `TARGETARCH=amd64` default on a
  typical amd64 dev machine). `--platform=linux/arm64` (needs buildx) targets a
  *different* architecture than this host's own — a real alternative to `cross`/
  `cross-container` for arm64 specifically, with no zig and no musl involved at all.
  ```sh
  ./tools/wiki-ops.sh build container-native          # no Docker needed on the target
  ./tools/wiki-ops.sh verify container-native
  ./tools/wiki-ops.sh deploy --target=root@192.0.2.30 --variant=container-native --first-time
  ```
  **The real caveat, not a formality**: the extracted binary is dynamically linked
  against the *build image's* glibc (`debian:bookworm-slim`). Deploying it to a target
  with an **older** glibc fails with `GLIBC_2.XX not found` — the exact problem
  `cross/`'s static-musl path exists to sidestep. Fine for a target on Debian
  bookworm+ (or any distro with glibc at least that new); for an old or unknown
  target, use `cross` instead, not this.
  **`--platform=linux/arm64`/`linux/arm/v7` (anything other than this host's own
  architecture) hits the same QEMU-emulation cost as `container-arm` above, and runs
  just as long** — this variant's short build time only applies with no `--platform`
  override.

## `verify`

```
verify cross|container|cross-container|container-arm|container-native
       [--triplet=NAME] [--platform=linux/arm/v7] [--qemu-cpu=NAME]
```

Run this before a binary/image ever reaches real hardware — "compiling cleanly is not
the same as working."

- **`cross`** / **`cross-container`** — `qemu-arm-static` runs `unit_tests` (must
  report every case passed, not just exit 0) then `wiki-server --create-admin` as a
  real smoke test, against whichever directory `build`'s own `--triplet`-derived
  naming produced (`build-arm/` for the default, `build-$triplet/` otherwise — pass
  the same `--triplet` here that you built with). Needs `qemu-user-static` installed
  on the dev machine.
  **`--qemu-cpu=NAME` matters more than it looks.** qemu's *default* CPU model (no
  `-cpu` flag) emulates a broader instruction set than some real cores — confirmed:
  the stock armv7 `arm-musl` binary passes clean under the bare default, then SIGILLs
  under `-cpu arm1176` specifically. For `armv6-musl` (ARM1176JZF-S — Raspberry Pi
  1/Zero/Zero W), the bare default would give a **false pass**:
  ```sh
  ./tools/wiki-ops.sh verify cross --qemu-cpu=arm1176
  ```
  Run `qemu-arm-static -cpu help` yourself to confirm the exact model name this
  qemu build supports before trusting a value here.
- **`container`** — `docker build` + a temporary `docker run -p 18080:8080` +
  `/healthz` curl check, torn down after.
- **`container-arm`** — same idea under `--platform` (default `linux/arm/v7`, QEMU
  emulation unless it happens to match the host's own architecture), port `18081`.
  Unlike `container`, this does **not** build anything itself — same convention as
  `cross`/`cross-container`/`container-native` below: it tests the exact image
  `build container-arm` already produced (`personal-wiki:arm`, or
  `personal-wiki:$(platform suffix)` for a non-default `--platform`), and dies with a
  clear message if that image doesn't exist yet. Run `build container-arm` first —
  verify paying the "very long" QEMU cost a second time, on top of build already
  having paid it once, would defeat the point of the two being separate steps.
  **`verify` never respects `--dry-run`, on any variant** — it's a real smoke test,
  not a plan.
- **`container-native`** — runs `wiki-server --create-admin` directly, no qemu (it's a
  native-architecture binary, assuming `--platform` wasn't used to cross-build it with
  buildx — that combination has no local smoke test available beyond the full `ctest`
  run that already happened *inside* the `docker build` itself).

## `deploy`

```
deploy [--target=HOST] [--variant=V] [--triplet=NAME] [--first-time]
       [--with-backup-timer] [--static-only] [--skip-verify]
       [--qemu-cpu=NAME] [--port=N] [--yes] [--dry-run]
```

Never builds implicitly — run the matching `build` first. `--target` empty means
install on this machine, no ssh involved; otherwise every mutating step runs over one
`ssh`/`scp` session per command, using `WIKI_DEPLOY_SSH_PORT`.

**Binary variants (`native`/`cross`/`cross-container`/`container-native`)**:
1. Refuses to continue if `build`/`build-arm`/`build-arm-container`/
   `build-container-native` doesn't exist yet.
2. Unless `--skip-verify`, runs the equivalent of `verify cross`/`verify
   cross-container`/`verify container-native` first (native has no such step —
   nothing to emulate or re-run locally).
3. Stages the install tree (`cmake --install` for native; a hand-assembled
   bin/+static/+systemd-files tree for cross/cross-container, matching what `cmake
   --install` would have produced; a plain copy of the already-extracted tree for
   `container-native`, which is already install()-shaped) into a local temp dir, ships
   it to the target, then does the atomic swap: `.bak-$STAMP` the live
   `bin/`/`static/`, `strip --strip-all` **on the target itself** (cross/
   cross-container/container-native — the dev machine's own `strip` can't always read
   a cross ARM ELF, and stripping on the target is simplest to keep uniform even for
   `container-native`'s native-arch binary, which the dev machine's `strip` *can*
   read), move the new files in, `chown`.
4. `--first-time`: also edits `config.toml`'s `listen_addr`/`port`/`threads`/
   `[vault].path`/`[mcp].scope` (anchored `sed`, never a real TOML parser — the
   multi-line `[embeddings]`/`[llm]` blocks are left for you to hand-edit, same as the
   manual doc already says) and sets `[server].base_path` if `WIKI_DEPLOY_BASE_PATH`
   is non-blank, then runs `wiki-server --create-admin` with a real, unscripted TTY
   (password entry needs its echo disabled — this is never piped or captured).
5. Installs/enables the `wiki.service` systemd unit (restarts it, with a `[y/N]`
   confirmation unless `--yes`, if it's already running); `--with-backup-timer`
   additionally ships `wiki-backup.sh`+its unit/timer/env fresh from this repo's own
   `systemd/` directory (see `cmd_backup_timer_install`) and enables the timer —
   **edit `BACKUP_DIR` on the target before trusting it** unless
   `WIKI_DEPLOY_BACKUP_DIR` in `deploy.local.env` already pre-filled it, point it at a
   different disk/mount than the vault.

`--triplet=NAME` (or `WIKI_DEPLOY_TRIPLET` in `deploy.local.env`) picks which
`build-*`/`build-*-container` directory `cross`/`cross-container` deploy from —
matters once you've ever built more than one cross target; ignored for every other
variant.

**Container variants (`container`/`container-arm`)**: a completely different path for
the app itself — `docker save`+`scp`+`docker load`+`docker compose up -d` for a remote
target, or just `docker compose up -d` locally (which builds on the fly if the image
doesn't exist yet — remote deploys need an explicit `build container`/`build
container-arm` first, since there's nothing local to `docker save` otherwise).
`--port=N` here sets `WIKI_HOST_PORT`, remapping only the **host**-side Docker port
mapping — the container's own internal port stays 8080 regardless
(`docker/config.docker.toml` and the `Dockerfile`'s `HEALTHCHECK`/`EXPOSE` both assume
that internally).

**`--with-backup-timer` now works identically to every other variant** — the same
`cmd_backup_timer_install` ships `wiki-backup.sh`+unit/timer/env fresh from this
repo's `systemd/` directory regardless of how the app itself runs; `VAULT_PATH`
defaults to `$install_root/vault_data`, which is also exactly where
`docker-compose.yml`'s bind mount puts the real vault on the host. `--first-time`
still does nothing for these two variants (no `config.toml`/admin-account step exists
to run — see below). For a container deployment:
- Admin account: `docker compose exec wiki wiki-server --create-admin` yourself
  (needs the real TTY `exec` gives you — `-T`/detached would break the password
  prompt's echo-disabling, same note as `docker-compose.yml`'s own header comment).
  `setup-admin`/`deploy --first-time` refuse outright rather than attempting this —
  see `setup-admin` below.
- Config: baked at build time from `docker/config.docker.toml`, not editable through
  this script at all.

**`--port=N` for binary variants** sets `config.toml`'s `[server].port` directly
(only takes effect with `--first-time`, since that's the only time this script
touches `config.toml` at all).

**`--static-only`** short-circuits straight to the same thing as the standalone
`static-redeploy` subcommand below, ignoring every build-variant-specific step.

### Example: first deploy to a real armv7 Pi over ssh

```sh
./tools/wiki-ops.sh build native --skip-tests      # one-time: produces the native drogon_ctl build/ needs
./tools/wiki-ops.sh build cross
./tools/wiki-ops.sh verify cross
./tools/wiki-ops.sh deploy --target=root@192.0.2.10 --variant=cross \
    --first-time --with-backup-timer
```

### Example: first deploy to a Pi Zero/1 (ARMv6)

```sh
./tools/wiki-ops.sh build native --skip-tests
./tools/wiki-ops.sh build cross --triplet=armv6-musl
./tools/wiki-ops.sh verify cross --triplet=armv6-musl --qemu-cpu=arm1176
./tools/wiki-ops.sh deploy --target=root@192.0.2.20 --variant=cross --triplet=armv6-musl \
    --first-time --qemu-cpu=arm1176
```

(`deploy` re-verifies before shipping unless `--skip-verify` — pass the same
`--triplet`/`--qemu-cpu` here too, or the re-verify falls back to the default armv7
directory/too-permissive qemu CPU model. Put `WIKI_DEPLOY_TRIPLET=armv6-musl` in
`deploy.local.env` instead of repeating `--triplet` on every call.)

### Example: local Docker Compose test run

```sh
./tools/wiki-ops.sh build container
./tools/wiki-ops.sh verify container
./tools/wiki-ops.sh deploy --variant=container          # local: just `docker compose up -d`
docker compose exec wiki wiki-server --create-admin
```

### Example: updating an existing binary deployment after a code change

```sh
./tools/wiki-ops.sh update --target=root@192.0.2.10 --variant=cross
```

Equivalent to, and replaces, the three-step `git pull` + `build cross` + `deploy
--target=root@192.0.2.10 --variant=cross` sequence by hand — see `update` below.
`deploy` here never gets `--first-time`, so `config.toml` and the admin account are
left untouched, matching `docs/sbc-deployment.md`'s "Update" section.

### Example: build in Docker, deploy a bare binary (no Docker on the target)

```sh
./tools/wiki-ops.sh build container-native
./tools/wiki-ops.sh verify container-native
./tools/wiki-ops.sh deploy --target=root@192.0.2.30 --variant=container-native \
    --first-time --with-backup-timer
```

No vcpkg/cmake/ninja/zig needed on the dev machine — only Docker. The target needs no
Docker at all, same as `native`/`cross`. Remember the glibc caveat under `build`
above before pointing this at anything other than a reasonably current target distro.

### Example: adding the backup timer to an existing container deployment

```sh
./tools/wiki-ops.sh systemd install --target=root@192.0.2.10 --variant=container --with-backup-timer
```

Works whether `wiki-server` itself runs natively or in a container — this only ever
touches `wiki-backup.sh`/its unit/timer/env, shipped fresh from this repo, never
anything a previous deploy step staged. `--variant=container` here only matters to
correctly *skip* the (nonexistent, for this variant) `wiki.service` part — the backup
install itself doesn't change shape.

## `update`

```
update [--target=HOST] [--variant=V] [--triplet=NAME] [--platform=P]
       [--static-only] [--with-backup-timer] [--skip-verify]
       [--qemu-cpu=NAME] [--port=N] [--yes] [--dry-run]
```

`git pull`, then the matching `build $variant`, then a non-`--first-time` `deploy` —
the exact sequence the "updating an existing binary deployment" example above used to
spell out as two separate commands, now one. `git pull` always runs in the current
working directory (the dev machine's own checkout) — never on the target itself, which
never needs its own git checkout for any variant.

`--static-only` skips both `build` and the full `deploy` swap, going straight to
`static-redeploy` instead — for a pull that only touched `static/` (CSS/JS/
`shell.html`), there's no C++ to rebuild. Every other flag `build`/`deploy` accept
individually also applies here (one shared flag parser) — `--triplet`/`--platform`
pick the variant's build target, `--with-backup-timer`/`--port`/`--qemu-cpu` reach
`deploy`. `--first-time` has no effect here on purpose — `update` is for an existing
deployment; use `deploy --first-time` for the first one.

**Unless `--skip-verify`, the inherited re-verify step always skips its
`--create-admin` smoke test specifically** — that sub-step needs a real interactive
TTY (password entry with echo disabled), which an automated `update` run never has.
`unit_tests` under `qemu-arm-static` (for `cross`/`cross-container`) still runs
normally; only the admin-creation half is skipped. `verify cross`/`deploy` run
standalone are unaffected — they still run the full smoke test, admin-creation
included.

**`--dry-run` is not a full no-op here, same as it already isn't for plain `deploy`**:
the build step's own commands are properly skipped, but `deploy`'s inherited
re-verify step (unless `--skip-verify`) and its read-only `systemctl is-active` probe
both still run for real — the probe exists specifically so the dry-run preview can
truthfully report which branch (`restart` vs. `enable --now`) a real run would take.
Nothing mutating runs under `--dry-run`; that probe is read-only by design — it
never restarts or reconfigures anything.

## `static-redeploy`

```
static-redeploy [--target=HOST] [--variant=V] [--dry-run]
```

The whole "Static-assets-only redeploy" recipe, automatically: tar the **entire**
`static/` tree (never a partial copy), stage it on the target, atomic swap, mandatory
`systemctl restart wiki.service` (`shell.html` is cached in-process at startup —
without the restart the old cached copy keeps being served indefinitely), then
md5sum-verify every single file against the local tree, not just the ones you think
changed.

```sh
./tools/wiki-ops.sh static-redeploy --target=root@192.0.2.10
```

**Assumes a binary-style install** (`$install_root/static` as a plain directory,
`systemctl restart wiki.service` as the real restart mechanism) — `static/` lives
inside the image for a container deployment, not at that path on the host. If
`--variant`/`WIKI_DEPLOY_VARIANT` resolves to `container`/`container-arm`, this
refuses outright with a clear error instead of attempting it (silent if the variant
is simply unknown — blank never errors, matching every other optional resolver
value). Rebuild and `docker compose up -d` again for a container deployment instead.

## `setup-admin`

```
setup-admin [--target=HOST] [--variant=V]
```

Just the `--create-admin` step on its own, same real-TTY discipline as `deploy
--first-time`. Useful to re-run standalone if you ever need to reset the single admin
row (overwrites both username and password together — there's no partial-recovery
path, same as the manual doc says). **Binary-style installs only** — assumes
`$install_root/bin/wiki-server` exists directly; refuses outright (same as
`static-redeploy` above) if `--variant`/`WIKI_DEPLOY_VARIANT` resolves to
`container`/`container-arm` — use `docker compose exec wiki wiki-server
--create-admin` for those instead.

## `systemd`

```
systemd install [--target=HOST] [--variant=V] [--with-backup-timer] [--yes] [--dry-run]
systemd status  [--target=HOST]
```

`install` is the standalone form of what `deploy` already does for the unit files —
useful to re-run idempotently (e.g. after hand-editing the shipped `wiki.service`) or
to add `--with-backup-timer` to an existing deployment that didn't opt in at first.
Unlike `static-redeploy`/`setup-admin`, this one doesn't refuse for
`container`/`container-arm` — it just *skips* the `wiki.service` part for them (logged,
not silent) since Docker's own `restart: unless-stopped` already owns that process;
`--with-backup-timer` works identically for every variant, see the container-backup
example under `deploy` above.

**Always backs up the target's existing `wiki.service` to `wiki.service.bak-$STAMP`
before overwriting it, and asks for confirmation first (`--yes` bypasses it, same as
the adjacent restart-confirmation) if that existing unit's `EnvironmentFile=` path
differs from the one this repo's own `systemd/wiki.service` ships** — a customized
target (a different env-file location than the documented
`/etc/opt/wiki/wiki.env`) silently reverting to this repo's default on a routine
`deploy`/`update` is exactly what caused a real production outage before this check
existed:

```sh
./tools/wiki-ops.sh systemd install --target=root@192.0.2.10 --with-backup-timer
```

`status` prints `systemctl status wiki.service` plus `systemctl list-timers
wiki-backup.timer` (or says it isn't installed) — meaningful for any variant now:
`wiki.service` legitimately reports "not found" for a container deployment, and
`wiki-backup.timer` legitimately exists if you've set it up via `systemd install
--with-backup-timer` above.

## `nginx-config`

```
nginx-config root --domain=D [--out=FILE]
nginx-config subpath --base-path=/wiki [--out=FILE]
```

Renders one of the two reverse-proxy blocks from `docs/sbc-deployment.md`'s "Reverse
proxy" section to a file (or stdout without `--out`) — it never touches a target's
live nginx config, and **TLS certificate issuance itself (certbot etc.) stays entirely
manual**: the rendered block references `/etc/letsencrypt/live/$domain/...` paths that
must already exist.

```sh
./tools/wiki-ops.sh nginx-config root --domain=wiki.example.com --out=/tmp/wiki.conf
./tools/wiki-ops.sh nginx-config subpath --base-path=/wiki
```

The `subpath` form also reminds you to set `[server].base_path` in `config.toml` and
restart — closes the one cold-start edge case client-side URL inference can't, see
`docs/sbc-deployment.md`. Running the `root` form instead of `subpath` (moving to a
dedicated (sub)domain) means **removing** `base_path` from `config.toml` if it was set
for a previous subpath deployment — one `config.toml`, one global value, baked into
every served page regardless of which hostname the request came in on.

## `config`

```
config init     # copies deploy.local.env.example to deploy.local.env
config show     # prints deploy.local.env with comments/blanks stripped
```

`init` refuses to overwrite an existing `deploy.local.env`, and refuses to create one
at all if `git check-ignore` doesn't confirm it's actually gitignored — a defensive
check against a future `.gitignore` edit silently removing that protection, not
because today's rules need adding to.

## Known limitations

- **No rollback subcommand.** Every atomic swap leaves `.bak-$STAMP` copies
  (`bin.bak-*`, `static.bak-*`) on the target, never cleaned up automatically — restore
  one by hand (`mv` it back over the current one, `systemctl restart wiki.service`),
  same as `docs/sbc-deployment.md` already documents. Nothing here automates that walk
  back yet.
- **`remote_sh`'s own preamble (`set -euo pipefail; ...`) assumes the target's login
  shell is bash-compatible.** `pipefail` isn't a POSIX option — a target whose
  `ssh user@host` login shell is `dash`/plain `sh` would fail this `set` call itself,
  before `$script` ever runs. Debian/Ubuntu's default root/admin shell is bash (true
  for every real target this project has actually been deployed to), so this is
  unverified rather than confirmed-broken — but genuinely untested against a
  non-bash login shell.
- **`wiki-backup.service`'s `ReadOnlyPaths=` is hardcoded to `/opt/wiki/vault_data`**
  in the shipped unit file itself (`systemd/wiki-backup.service`) — if your
  `--target`'s install root isn't `/opt/wiki`, the timer's own sandboxing won't permit
  reading the real vault path even though `cmd_backup_timer_install` correctly points
  `VAULT_PATH` at it. `systemd install --with-backup-timer` logs a warning when this
  applies; fixing it means either always deploying to `/opt/wiki`, or adding a
  `ReadWritePaths=`/`ReadOnlyPaths=` override in
  `/etc/systemd/system/wiki-backup.service.d/` on the target by hand — this script
  doesn't do that for you.
- **No multi-host inventory.** One `deploy.local.env` holds exactly one target; manage
  several real deployments with separate env files and `--target=`/`--port=`/
  `--triplet=` flags overriding per invocation, or several copies of
  `deploy.local.env` swapped in by hand.
