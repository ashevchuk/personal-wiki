# Changelog

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/);
versioning follows [SemVer](https://semver.org/). The version lives in
`CMakeLists.txt`'s `project(... VERSION ...)` and is embedded in both
binaries — check a running/built copy with `wiki-server --version` or
`wiki-mcp --version`.

## [Unreleased]

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
