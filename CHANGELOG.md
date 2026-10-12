# Changelog

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/);
versioning follows [SemVer](https://semver.org/). The version lives in
`CMakeLists.txt`'s `project(... VERSION ...)` and is embedded in both
binaries — check a running/built copy with `wiki-server --version` or
`wiki-mcp --version`.

## [Unreleased]

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
