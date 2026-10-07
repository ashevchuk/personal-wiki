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

| Target | Variant | Needs on the dev machine | Needs on the target |
|---|---|---|---|
| Modern 64-bit SBC (Pi 4/5 Bookworm+) | `native` (build *on* the device) | — | C++20 compiler, CMake, Ninja |
| Old/weak/32-bit SBC (Pi 2/3 armhf, generic armv7) | `cross` | zig, a native `build/` already done once | nothing but the shipped binary |
| True ARMv6 (Pi 1/Zero/Zero W) | `cross --triplet=armv6-musl --cross-dir=cross/armv6-musl` | same as `cross` | nothing |
| Same as `cross`, but no local zig/vcpkg wanted | `cross-container` | only Docker | nothing |
| x86_64/arm64 desktop, NAS, cloud VM with modern Docker | `container` | Docker | Docker |
| Pi 4/5 (or any arm64/armv7 box) you'd rather not cross-compile for, via Docker | `container-arm` | Docker + buildx (+ QEMU binfmt for cross-arch) | Docker |

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
- **`cross`** — Path B, into `build-arm/`. Requires a native `build/` to already exist
  (specifically `build/vcpkg_installed/x64-linux/tools/drogon/drogon_ctl` — run `build
  native` at least once first; classic-mode cross builds reuse this host tool instead
  of cross-building Drogon's `ctl` code-generator). `--triplet=NAME`/`--cross-dir=PATH`
  pick a different `cross/<name>/` toolchain — default `arm-musl`/`cross/arm-musl`
  (armv7). For a true ARMv6 target (Raspberry Pi 1/Zero/Zero W):
  ```sh
  ./tools/wiki-ops.sh build cross --triplet=armv6-musl --cross-dir=cross/armv6-musl
  ```
  **Known limitation**: the output directory is always `build-arm/`, regardless of
  `--triplet`. Building `arm-musl` then `armv6-musl` (or vice versa) overwrites the
  previous target's binaries in place — fine for a one-off switch, not for keeping two
  cross targets' artifacts side by side. Rename `build-arm/` out of the way first if
  you need both.
- **`container`** — `docker compose build`, the existing x86_64/arm64 image, unchanged.
- **`cross-container`** — builds `cross/Dockerfile.builder` (zig + vcpkg + the
  `cross/arm-musl` toolchain, entirely inside the image — no local zig/vcpkg install
  needed) and extracts `wiki-server`/`wiki-mcp`/`unit_tests` into
  `build-arm-container/`. **Hardcoded to `arm-musl`/armv7** — `--triplet`/`--cross-dir`
  have no effect here; this path does not (yet) generalize to `armv6-musl`.
- **`container-arm`** — `docker buildx build --platform <platform> --load .` against
  the main `Dockerfile` (parameterized by `TARGETARCH`/`TARGETVARIANT` — see its own
  header comment). `--platform` defaults to `linux/arm/v7`; `linux/arm64` and
  `linux/amd64` also work (tagged `personal-wiki:arm64`/`personal-wiki:amd64` — only
  the *default* platform keeps the plain `personal-wiki:arm` tag that
  `docker-compose.arm.yml`/`deploy --variant=container-arm` expect). Needs a buildx
  builder (`docker buildx create --use`) and, for an architecture that isn't this
  host's own, QEMU binfmt registered (`docker run --privileged --rm tonistiigi/binfmt
  --install all`, one-time per host).

## `verify`

```
verify cross|container|cross-container|container-arm
       [--platform=linux/arm/v7] [--qemu-cpu=NAME]
```

Run this before a binary/image ever reaches real hardware — "compiling cleanly is not
the same as working."

- **`cross`** / **`cross-container`** — `qemu-arm-static` runs `unit_tests` (must
  report every case passed, not just exit 0) then `wiki-server --create-admin` as a
  real smoke test, against `build-arm/`/`build-arm-container/` respectively. Needs
  `qemu-user-static` installed on the dev machine.
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
  Needs the same buildx/binfmt prerequisites as `build container-arm`.

## `deploy`

```
deploy [--target=HOST] [--variant=V] [--first-time] [--with-backup-timer]
       [--static-only] [--skip-verify] [--qemu-cpu=NAME] [--port=N]
       [--yes] [--dry-run]
```

Never builds implicitly — run the matching `build` first. `--target` empty means
install on this machine, no ssh involved; otherwise every mutating step runs over one
`ssh`/`scp` session per command, using `WIKI_DEPLOY_SSH_PORT`.

**Binary variants (`native`/`cross`/`cross-container`)**:
1. Refuses to continue if `build`/`build-arm`/`build-arm-container` doesn't exist yet.
2. Unless `--skip-verify`, runs the equivalent of `verify cross`/`verify
   cross-container` first (native has no such step — nothing to emulate).
3. Stages the install tree (`cmake --install` for native; a hand-assembled
   bin/+static/+systemd-files tree for cross/cross-container, matching what `cmake
   --install` would have produced) into a local temp dir, ships it to the target, then
   does the atomic swap: `.bak-$STAMP` the live `bin/`/`static/`, `strip --strip-all`
   **on the target itself** (cross/cross-container only — the dev machine's own
   `strip` can't read a cross ARM ELF), move the new files in, `chown`.
4. `--first-time`: also edits `config.toml`'s `listen_addr`/`port`/`threads`/
   `[vault].path`/`[mcp].scope` (anchored `sed`, never a real TOML parser — the
   multi-line `[embeddings]`/`[llm]` blocks are left for you to hand-edit, same as the
   manual doc already says) and sets `[server].base_path` if `WIKI_DEPLOY_BASE_PATH`
   is non-blank, then runs `wiki-server --create-admin` with a real, unscripted TTY
   (password entry needs its echo disabled — this is never piped or captured).
5. Installs/enables the `wiki.service` systemd unit (restarts it, with a `[y/N]`
   confirmation unless `--yes`, if it's already running); `--with-backup-timer`
   additionally installs `wiki-backup.{service,timer}` and, if absent, a fresh
   `wiki-backup.env` from the example (**edit `BACKUP_DIR` on the target before
   trusting it** — point it at a different disk/mount than the vault).

**Container variants (`container`/`container-arm`)**: a completely different path —
`docker save`+`scp`+`docker load`+`docker compose up -d` for a remote target, or just
`docker compose up -d` locally (which builds on the fly if the image doesn't exist yet
— remote deploys need an explicit `build container`/`build container-arm` first,
since there's nothing local to `docker save` otherwise). `--port=N` here sets
`WIKI_HOST_PORT`, remapping only the **host**-side Docker port mapping — the
container's own internal port stays 8080 regardless (`docker/config.docker.toml` and
the `Dockerfile`'s `HEALTHCHECK`/`EXPOSE` both assume that internally).

**Neither `--first-time` nor `--with-backup-timer` does anything for container
variants** (fixed as of this doc — an earlier version wrongly tried to install a
systemd unit that's never staged for a Docker deployment; `--with-backup-timer` now
just logs that it has no effect instead). For a container deployment:
- Admin account: `docker compose exec wiki wiki-server --create-admin` yourself
  (needs the real TTY `exec` gives you — `-T`/detached would break the password
  prompt's echo-disabling, same note as `docker-compose.yml`'s own header comment).
- Config: baked at build time from `docker/config.docker.toml`, not editable through
  this script at all.
- Backup: `wiki-backup.sh` never ships inside the container image's runtime stage; back
  up the host's bind-mounted `vault_data/` directory directly (a plain `tar`, a host
  cron job, restic, whatever you'd use for any other bind-mounted directory) — nothing
  here automates that yet.

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
./tools/wiki-ops.sh build cross --triplet=armv6-musl --cross-dir=cross/armv6-musl
./tools/wiki-ops.sh verify cross --qemu-cpu=arm1176
./tools/wiki-ops.sh deploy --target=root@192.0.2.20 --variant=cross \
    --first-time --qemu-cpu=arm1176
```

(`deploy` re-verifies before shipping unless `--skip-verify` — pass the same
`--qemu-cpu` here too, or the re-verify falls back to the default, too-permissive
qemu CPU model.)

### Example: local Docker Compose test run

```sh
./tools/wiki-ops.sh build container
./tools/wiki-ops.sh verify container
./tools/wiki-ops.sh deploy --variant=container          # local: just `docker compose up -d`
docker compose exec wiki wiki-server --create-admin
```

### Example: updating an existing binary deployment after a code change

```sh
./tools/wiki-ops.sh build cross
./tools/wiki-ops.sh deploy --target=root@192.0.2.10 --variant=cross
```

(no `--first-time` — `config.toml` and the admin account are left untouched, matching
`docs/sbc-deployment.md`'s "Update" section.)

## `static-redeploy`

```
static-redeploy [--target=HOST] [--dry-run]
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
`systemctl restart wiki.service` as the real restart mechanism) — do not point this at
a container deployment; `static/` lives inside the image there, not at that path on
the host. Rebuild and `docker compose up -d` again instead.

## `setup-admin`

```
setup-admin [--target=HOST]
```

Just the `--create-admin` step on its own, same real-TTY discipline as `deploy
--first-time`. Useful to re-run standalone if you ever need to reset the single admin
row (overwrites both username and password together — there's no partial-recovery
path, same as the manual doc says). **Binary-style installs only** — assumes
`$install_root/bin/wiki-server` exists directly; for a container deployment use
`docker compose exec wiki wiki-server --create-admin` instead.

## `systemd`

```
systemd install [--target=HOST] [--with-backup-timer] [--yes] [--dry-run]
systemd status  [--target=HOST]
```

`install` is the standalone form of what `deploy` already does for the unit files —
useful to re-run idempotently (e.g. after hand-editing the shipped `wiki.service`) or
to add `--with-backup-timer` to an existing deployment that didn't opt in at first:

```sh
./tools/wiki-ops.sh systemd install --target=root@192.0.2.10 --with-backup-timer
```

`status` prints `systemctl status wiki.service` plus `systemctl list-timers
wiki-backup.timer` (or says it isn't installed). **Binary-style installs only** —
same reasoning as `static-redeploy` above; there's no `wiki.service` unit for a
container deployment at all.

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
- **`cross`'s output directory doesn't vary with `--triplet`.** See `build` above —
  switching between `arm-musl` and `armv6-musl` reuses (overwrites) `build-arm/`.
- **`cross-container` is hardcoded to `arm-musl`/armv7.** `--triplet`/`--cross-dir`
  have no effect on it; `armv6-musl` is reachable only via the bare `cross` variant.
- **`static-redeploy`, `setup-admin`, `systemd install|status`, and `deploy
  --static-only` all assume a binary-style install.** None of them check
  `WIKI_DEPLOY_VARIANT` first — pointing one at a container deployment's install root
  fails or does something meaningless rather than erroring clearly. Stick to
  `docker compose`/`docker compose exec` directly for container deployments' admin
  account, static assets, and process management.
- **`cmd_systemd_install`'s brace expansion** (`wiki-backup.{service,timer}`) assumes
  the target's default shell is bash (or another shell with brace expansion) — same
  assumption `docs/sbc-deployment.md`'s own manual recipe already makes, not a
  regression introduced here.
- **No multi-host inventory.** One `deploy.local.env` holds exactly one target; manage
  several real deployments with separate env files and `--target=`/`--port=` flags
  overriding per invocation, or several copies of `deploy.local.env` swapped in by hand.
