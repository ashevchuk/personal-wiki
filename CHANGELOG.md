# Changelog

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/);
versioning follows [SemVer](https://semver.org/). The version lives in
`CMakeLists.txt`'s `project(... VERSION ...)` and is embedded in both
binaries — check a running/built copy with `wiki-server --version` or
`wiki-mcp --version`.

## [Unreleased]

## [0.3.3] - 2026-10-15

### Fixed

- `cross/armv6-musl/{cc,c++}` passed `-mcpu=arm1176jzf-s` (hyphen) to
  zig. zig's `-mcpu` syntax treats a trailing `-name` as "disable
  feature `name`" (the same `+`/`-` syntax `cross/arm-musl/`'s own
  `-mcpu=generic+v7a` uses to add one), so this parsed as "cpu=
  arm1176jzf, disable feature s" and failed immediately with `unknown
  CPU: 'arm1176jzf'` — confirmed live, this broke every CI build of
  this triplet (`.github/workflows/release.yml`'s `v0.3.1`/`v0.3.2`
  runs). The correct spelling is `arm1176jzf_s` (underscore), confirmed
  against zig's own `-mcpu` CPU list and a full local build.

## [0.3.2] - 2026-10-15

### Fixed

- `wiki-ops.sh verify`'s CLI had no way to skip the `--create-admin`
  smoke test (`cmd_verify_cross`/`cmd_verify_container_native` support a
  `skip_admin_smoke` parameter, but only `cmd_update` ever passed `1` to
  it — the standalone `verify` subcommand hardcoded `0` always). That
  smoke test needs a real interactive TTY (password entry, echo
  disabled), which a CI runner's `run:` step never has, so every
  `.github/workflows/release.yml` build job failed on `username must
  not be empty` despite `unit_tests` already passing clean — confirmed
  live on the `v0.3.1` tag's run. Added `--skip-admin-smoke` as a real
  `verify` flag, wired through to `cross`/`cross-container`/
  `container-native`; `release.yml` now passes it.

## [0.3.1] - 2026-10-15

### Fixed

- Both `Dockerfile` and `cross/Dockerfile.builder` had a `COPY cmake
  ./cmake` line referencing a top-level `cmake/` directory that exists
  only as an empty, never-committed leftover on some dev machines —
  nothing in `CMakeLists.txt` actually references it. On an actual
  fresh checkout (any CI runner, any new clone) that directory doesn't
  exist at all, so every containerized build variant
  (`container`/`container-arm`/`container-native`/`cross-container`)
  failed outright with `"/cmake": not found`, confirmed live by
  `.github/workflows/release.yml`'s first real run on a clean GitHub
  Actions checkout (every local test before that happened to have the
  stray directory present and masked it). Removed the dead `COPY` line
  from both Dockerfiles.

## [0.3.0] - 2026-10-15

### Added

- `.github/workflows/release.yml`: builds a fully static `wiki-server`/
  `wiki-mcp` for `arm-musl`, `armv6-musl`, `aarch64-musl`, and the new
  `x64-musl` target on every `vX.Y.Z` tag, and publishes one tarball per
  target plus a `SHA256SUMS` to that tag's GitHub Release.
- `cross/x64-musl/`: a fourth cross-compile triplet, generic static
  x86_64 via the same zig/musl toolchain shape as the existing ARM
  triplets — works through the existing `build cross`/`build
  cross-container` (no `cross/Dockerfile.builder` changes needed, it was
  already generic over the triplet).
- `tools/wiki-ops.sh package` — stages a build's output into the
  cmake-install-shaped tree `deploy` itself stages, as a local directory
  or `.tar.gz`; what `release.yml` uses to produce each release asset.
- `tools/wiki-ops.sh deploy --variant=release --release-tag=vX.Y.Z` —
  downloads and checksum-verifies a GitHub Release tarball and feeds it
  into the existing atomic-swap deploy path, no local build needed.
- `tools/wiki-install.sh` — standalone installer with no dependency on
  this repo's build tooling; downloads, verifies, and installs the
  matching release tarball directly on the target device.

### Fixed

- `cmd_systemd_install`/`cmd_backup_timer_install`'s install-root
  substitution (`sed 's|/opt/wiki|...|g'`) also matched the unrelated
  `/etc/opt/wiki/` FHS convention path inside
  `EnvironmentFile=-/etc/opt/wiki/wiki.env` and `wiki-backup.env`, since
  it contains that same substring. With any `--install-root` other than
  the default, this silently rewrote it to a path that doesn't exist —
  the service then started with none of its `[embeddings]`/`[llm]`
  secrets loaded, with no error at all (`EnvironmentFile=-` makes a
  missing file non-fatal). Both substitutions are now anchored to
  `=/opt/wiki` (right after the `=`), which matches only
  `ExecStart`/`WorkingDirectory`/`ReadWritePaths`/`ReadOnlyPaths`.

## [0.2.1] - 2026-10-12

### Fixed

- Static assets over Drogon's 200 KiB sendfile cutoff (the vendored
  circuitjs1/mermaid/Toast UI Editor bundles) were served fully
  uncompressed: `HttpResponseImpl::shouldBeCompressed()` refuses to
  gzip/brotli a sendfile-backed response, and no `.gz`/`.br` sidecar
  existed for `setGzipStatic`/`setBrStatic` to pick up. New
  `tools/compress-static.sh` generates `.gz` sidecars for qualifying
  files at install/staging time (hooked into `CMakeLists.txt`'s
  `install()`, `tools/wiki-ops.sh`'s cross/cross-container staging, and
  its static-only redeploy path) — nothing generated is committed to the
  repo.

## [0.2.0] - 2026-10-12

### Added

- `get_document` (both the stdio and remote-HTTP MCP transports) now
  includes a document's direct `[[wiki-link]]` neighbors — `Links`
  (outgoing) and `Backlinks` (incoming) — one hop only, so an agent can
  follow one with another `get_document` call instead of re-deriving the
  link graph itself from raw markdown text. New `GraphQueries::directLinks`.

## [0.1.0] - 2026-10-09

First tracked version. Everything before this point shipped without a
version number — see `git log` for that history; this file starts here
going forward.

### Added

- Version tracking itself: `CMakeLists.txt`'s `project(... VERSION ...)`
  as the single source, `wikicore::versionString()` reading it, and a
  `--version` flag on `wiki-server` and `wiki-mcp` that prints it and
  exits without touching config/db/vault.
