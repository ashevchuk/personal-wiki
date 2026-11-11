# Changelog

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/);
versioning follows [SemVer](https://semver.org/). The version lives in
`CMakeLists.txt`'s `project(... VERSION ...)` and is embedded in both
binaries — check a running/built copy with `wiki-server --version` or
`wiki-mcp --version`.

## [Unreleased]

### Security

- Markdown links only keep their target for `http`, `https`, `mailto` and
  `tel` URLs and for relative links. A link with any other scheme
  (`javascript:`, `data:`, ...) renders as plain text, so a document body can
  no longer run script in the reader's session when a link is clicked.
- Paths with a dot-prefixed file or folder name are rejected by every
  document, folder and attachment operation (web API, remote and stdio MCP,
  agent tools). A soft-deleted public document is no longer readable under
  `.trash/`, and an MCP token can no longer read the index database or
  other internal files that live inside the vault directory.
- A file inside a document's `.assets/` folder is never treated as a
  document. A markdown attachment with its own `visibility: public` front
  matter no longer shows up in search or under `/api/documents/` while its
  owning document is private.
- Remote MCP (`POST /mcp`, `POST /mcp/agent`) checks the IP allowlist before
  the bearer token. A caller outside the allowlist gets `403` whether or not
  its token is valid, and the token's "last used" time is not updated.
- The CSRF token is accepted only in the `X-CSRF-Token` header, no longer
  as a `csrf_token` request parameter.
- New passwords (`wiki-server --create-admin`, the Account page) must be at
  least 12 characters. An existing shorter password keeps working until it
  is changed. Passwords over 1024 bytes are rejected without being hashed.
- The login and remote-MCP rate limiters bucket an IPv6 client by its /64
  instead of the full address, so rotating addresses inside one prefix no
  longer yields unlimited attempts, and they drop expired entries once they
  track 10,000 keys instead of growing without bound.

### Changed

- A single request body, and therefore an attachment uploaded through the
  web UI or remote MCP, is limited to 100 MiB (was 2 GiB). The limit applies
  before authentication, so it also bounds what an anonymous caller can make
  the server receive and spool to disk. The documented nginx configuration
  now sets `client_max_body_size 100m` to match; an existing proxy config
  with `client_max_body_size 0` keeps working. Local `wiki-mcp`
  `attach_file` with `source_path` is not affected.

## [0.10.0] - 2026-11-06

### Added

- Query blocks accept `header: false` to hide the table's header row and
  `columns: title, updated` to choose which columns are shown and in what
  order.

### Security

- The login rate limiter is keyed by the real client IP instead of the TCP
  peer. Behind a reverse proxy every visitor used to share the proxy's
  loopback bucket, so one attacker's failed attempts locked the admin out.
- Session and CSRF cookies are marked `Secure` behind a TLS-terminating
  proxy that sends `X-Forwarded-Proto: https`; before, only standalone TLS
  mode set the flag. The header is ignored in standalone TLS mode.
- Login verifies the password hash even for an unknown username, so response
  time no longer reveals whether the username exists.

## [0.9.5] - 2026-11-06

### Fixed

- Graph layout starts from an ellipse matching the box instead of a
  circle, so a document with a single link is laid out vertically in the
  narrow local-graph panel and its two labels no longer overlap.
- Nodes and labels at the edge of the graph are no longer clipped by the
  canvas: the layout is fitted and centred by the nodes' real bounding
  box instead of the simulation box, which has no walls.
- Overlapping graph labels no longer pile up: a label that would overlap
  one already drawn is hidden (hubs, the centre node and the hovered node
  win) and reappears on hover.
- In the local graph, labels of nodes above the centre are drawn above
  the node instead of below it, where the node's own edge ran through the
  text.

## [0.9.4] - 2026-11-06

### Fixed

- On the Account page's Remote MCP tab, a token row with a long label no
  longer pushes its Agent access / Regenerate / Delete controls onto a
  separate line; the label wraps inside its own block instead.

## [0.9.3] - 2026-11-06

### Fixed

- `DocumentService::update()` now records a `document_snapshots` row only
  when the document's body text actually changed, not on every save — a
  front-matter-only edit (e.g. toggling visibility) no longer leaves a
  snapshot that the history diff view has nothing to show a difference
  for.
- The document-history diff view no longer renders every unchanged line
  as a wall of dimmed text when the body hasn't changed; it now reports
  "No differences." instead.
- The `green` theme's document-history diff view no longer sits on a
  solid dark-green panel background.

## [0.9.2] - 2026-11-05

### Fixed

- Horizontal rules in rendered documents use the theme's dim green in the
  `green` theme instead of the browser's default light gray, matching the
  editor.

## [0.9.1] - 2026-11-05

### Fixed

- Search excerpts stored for documents are now cut on a character boundary.
  The 240-byte excerpt limit could land inside a multi-byte UTF-8 character
  (every Cyrillic letter is two bytes), leaving an invalid sequence in the
  index; serializing a search result carrying it failed, so `POST
  /mcp/agent` answered `502 agent request failed` whenever the model
  searched and a hit had such an excerpt. Existing excerpts are rebuilt by
  the full rescan that runs at every startup.

## [0.9.0] - 2026-11-05

### Added

- Remote MCP (`POST /mcp`) now supports multiple independently labeled bearer
  tokens instead of one singleton token shared by every client. Each token can
  be created, regenerated, or deleted on its own from the Account page's
  Remote MCP section without affecting any other token; `enabled`,
  write-access, and the IP allowlist remain global settings shared by all
  tokens. `GET /api/admin/mcp-audit-log` now records which token's label made
  each remote write. Upgrading carries the previous singleton token forward
  as a migrated row so already-configured clients keep working without
  reconfiguration.
- New remote agent endpoint, `POST /mcp/agent`: hands a short conversation to
  the wiki's own configured `[llm]`, which can search/browse/read documents
  and the calendar, then decides on its own when to call `create_document`.
  Deliberately excludes `update_document`/`attach_file` — letting a model
  rewrite existing content from free text is a bigger mistake-blast-radius
  than creating something new. Gated per-token by a new "Agent access"
  opt-in (default off, independent of a token's existing write access) on
  top of everything `POST /mcp` already requires. Every tool call, reads
  included, is recorded in `mcp_audit_log` with tool name prefixed
  `remote-agent:`.
- Account page: the token list renders as structured per-token rows
  (label, prefix, timestamps, controls) instead of one unstructured line;
  the reveal box for a newly created or regenerated token now has a Copy
  button matching the other connect-examples fields.

## [0.8.2] - 2026-11-04

### Fixed

- 0.8.1's `CMAKE_BUILD_TYPE=Release` default didn't actually produce a
  debug-info-free cross-compiled binary: `zig cc`/`zig c++` emit DWARF
  debug info by default even with no `-g` flag at all (confirmed live —
  a trivial `int main(){}` compiled with zero flags already had
  `.debug_info`; unlike gcc/clang, omitting `-g` is not "no debug info"
  for zig). Every `cross/*/{cc,c++}` wrapper now passes `-g0`
  unconditionally. Verified against a genuinely clean rebuild (shared
  vcpkg install root wiped, `VCPKG_BINARY_SOURCES=clear`, so nothing
  was silently restored from a pre-fix binary cache) for all four
  release triplets: `wiki-server` dropped from ~143MB to ~25-28MB
  unstripped, all `unit_tests` still pass under
  `qemu-arm-static`/`qemu-aarch64-static` with each target's real CPU
  model. A small remainder (~12MB on `x64-musl`) comes from zig's own
  bundled musl runtime (confirmed with a minimal reproduction — a
  statically-linked, already-`-g0`-compiled "hello world" still carries
  it), unrelated to this project's own code or dependencies, and
  already handled by the existing target-side `strip --strip-all` step.

Bump to 0.8.2.

## [0.8.1] - 2026-11-04

### Fixed

- `std::to_string(mtime.time_since_epoch().count())` in
  `LocalEmbeddingProvider.cpp` was an ambiguous overload call under
  zig's libc++ on 64-bit targets (`long`/`long long` both 8 bytes,
  equally viable matches) — a real cross-compile failure found on
  `aarch64-musl`/`x64-musl` once 0.8.0 made local embeddings compile by
  default there. Both arguments now cast to an explicit width before
  `std::to_string`.
- `WIKI_ENABLE_LOCAL_EMBEDDINGS`'s `-mcpu=generic+v7a+fp16` cross-compile
  override (needed for Cortex-A7's NEON) was gated only on
  `CMAKE_SYSTEM_PROCESSOR STREQUAL "arm"`, which `armv6-musl`'s ARM1176
  target also reports despite having no NEON unit at all — applying
  ARMv7+NEON codegen there crash-looped the service with an illegal
  instruction (`qemu-arm-static --cpu arm1176`, confirmed live) rather
  than just running slower, once 0.8.0 made this the default for that
  triplet too. `cross/arm-musl/toolchain.cmake` and
  `cross/armv6-musl/toolchain.cmake` now each set a
  `WIKI_CROSS_ARM_HAS_NEON` flag the override reads instead; without it,
  ggml's own compile-time feature probing correctly falls back to a
  scalar implementation. All four release triplets plus the native build
  verified clean after this fix (`qemu-arm-static`/`qemu-aarch64-static`
  for the ARM targets).

### Changed

- The default `CMAKE_BUILD_TYPE` (when none is passed) is now `Release`,
  not `RelWithDebInfo` — a statically-linked `wiki-server` carried
  roughly 100MB of DWARF debug info under the old default. Pass
  `-DCMAKE_BUILD_TYPE=RelWithDebInfo` explicitly for a build that keeps
  symbols for `addr2line`/`gdb`.

## [0.8.0] - 2026-11-04

### Changed

- `WIKI_ENABLE_LOCAL_EMBEDDINGS`/`WIKI_ENABLE_CLOUD_EMBEDDINGS` now
  default to `ON` (`CMakeLists.txt`), not opt-in — every build this
  project ships, including the GitHub Release binaries
  `.github/workflows/release.yml` publishes, now supports whatever
  `[embeddings]`/`[llm]` provider a `config.toml` sets. This removes the
  crash-loop risk a release binary previously had when swapped onto a
  target that already used either (`release_embeddings_guard`, now
  removed from `tools/wiki-ops.sh`/`tools/wiki-install.sh` along with the
  refusal it enforced — there's nothing left to guard against). Pass
  `tools/wiki-ops.sh build ... --no-local-embeddings
  --no-cloud-embeddings` (or `-DWIKI_ENABLE_LOCAL_EMBEDDINGS=OFF`/
  `-DWIKI_ENABLE_CLOUD_EMBEDDINGS=OFF` directly) for a deliberately
  lighter build without either — flags that didn't exist before this
  change, since there was previously no way to opt back OUT once the
  default flipped.

## [0.7.5] - 2026-11-04

### Fixed

- The "YT" toolbar button's real root cause, found via `getComputedStyle`
  in a real browser (0.7.3/0.7.4 guessed wrong): it sat 7px lower than
  its neighbors because both its own button-level `margin: 7px 5px`
  (from `.toastui-editor-defaultUI-toolbar button`, which every custom
  `el`-based toolbar item still gets) AND its wrapping
  `.toastui-editor-toolbar-item-wrapper` div's own `margin: 7px 5px`
  applied — Toast UI's own built-in buttons don't double up on this.
  Canceled the button's own `margin-top` directly, not the shared
  wrapper rule, so no other toolbar item is affected. It also lacked the
  `toastui-editor-toolbar-icons` class every built-in icon button has,
  which is what recolors the resting-state border for the dark/green
  themes — without it the button kept the vendored default's pale
  `#f7f9fc` border, visible against those themes' dark panels.

## [0.7.4] - 2026-11-04

### Fixed

- The "YT" toolbar button's text still sat visibly lower than its
  neighbors' icons after 0.7.3's box-size fix. The fixed 32x32 box every
  toolbar button gets (`.toastui-editor-defaultUI-toolbar button`) sets
  no `line-height` on the button itself — only on the wrapper `div`
  around it — which every other button never needed since its icon is a
  `background-image`, indifferent to line-height. This button's "YT" is
  real text, so its own vertical position depends on line-height and the
  loaded font's metrics. Set `line-height: 32px` directly on it.

## [0.7.3] - 2026-11-04

### Fixed

- The "YT" toolbar button sat visibly lower than its neighbors.
  `toastui-editor.css`'s own `.toastui-editor-defaultUI-toolbar button`
  rule (a bare element selector) gives every toolbar button a fixed
  32x32 box, which is what keeps them aligned in the row — the button
  added in 0.7.0 set its own inline `width: auto` / padding, overriding
  that box for just this one button. Removed the override; it now
  inherits the same fixed box as every other toolbar button.

## [0.7.2] - 2026-11-04

### Fixed

- The sidebar's theme-selection menu rendered behind the edit page's own
  form fields. `.theme-menu` was a plain `position: absolute` child of
  the (non-positioned) sidebar — a non-positioned ancestor can't win a
  page-level stacking fight against ANY positioned element elsewhere on
  the page with an explicit `z-index`, regardless of what `z-index` the
  menu itself sets. `.field-row` on the edit page (`position: relative;
  z-index: 1`, added for an unrelated reason — beating Toast UI Editor's
  own internal layers) was exactly such an element. Fixed the same way
  as the Type/Tags typeahead dropdown: `theme.js` now moves `#theme-menu`
  to `document.body` as a "portal" and positions it with `position:
  fixed` from the toggle button's own `getBoundingClientRect()`.

## [0.7.1] - 2026-11-04

### Fixed

- The new "YT" toolbar button (added in 0.7.0) did nothing when clicked.
  Toast UI Editor's custom toolbar items are wired up via a real DOM
  element on the item's `el` property, with the click listener attached
  to that element directly — not a declarative `onClick` field the
  library reads itself, which is what the first version passed and which
  the library silently ignored.

## [0.7.0] - 2026-11-04

### Added

- The WYSIWYG editor toolbar has a "YT" button next to Image/Link for
  inserting a YouTube video: prompts for a URL, then inserts it the same
  way an attachment's Image button does (`editor.exec("addImage", ...)`
  with `altText: "youtube"`, the sentinel `util/YouTubeEmbed.h` already
  looks for on save). The existing `customHTMLRenderer.image` preview
  hook (added for hand-typed `![youtube](url)`) shows the real thumbnail
  immediately instead of a broken-image icon.

## [0.6.0] - 2026-11-04

### Changed

- Site themes (classic/dark/green) are no longer hand-maintained in three
  separate places (`shell.html` twice, `theme.js` once). `PageRoutes.cpp`'s
  `discoverThemes()` now scans `static/css/themes/*.css` once at startup
  and generates the theme list, the sidebar's theme-menu buttons, and the
  pre-stylesheet-load anti-FOUC placeholder colors from whatever files
  are actually present. Each theme file declares its own placeholder
  colors via a `fouc-bg`/`fouc-fg` metadata comment near its top (those
  can't be read from the theme's real `--bg`/`--fg` custom properties,
  since the stylesheet hasn't loaded yet at the point they're needed).
  Adding a fourth theme now only needs a new CSS file plus that one
  comment — no JS or HTML to touch.

## [0.5.1] - 2026-11-04

### Fixed

- The Tags/Type dropdowns on the search page could both be open at once,
  with one visually covering the other — opening one now closes any
  other open dropdown of the same kind.
- Account page audit-log pagers (Remote MCP, Draft agent, Chat) jumped
  position page to page because entries wrapped to different line
  counts; entries are now truncated to one line (full text still
  reachable via a hover tooltip). The Trash pager had the same jump for
  a different reason (a short last page having fewer rows than a full
  one) — fixed separately with invisible filler rows, since the path
  column there must stay fully visible (unlike the audit log, truncating
  it would hide the detail the list exists to show).
- Every floating popup that used the same background color as ordinary
  buttons/inputs (account dialogs, the calendar date pickers, the
  month/year dropdown, the trash hover preview, the theme menu) now uses
  a dedicated, theme-defined `--popup-bg` instead, consolidating three
  different ad-hoc copies of the same fix that had drifted across the
  CSS. In the green theme specifically this replaces a green tint that
  read as an unwanted wash on a floating panel-sized surface; the other
  two themes keep their existing color, unchanged.
- The pressed Month/Week/Day calendar view button lost its text on
  hover — its own rule set a background but not a text color, so the
  page-wide hover-invert rule's color won by default and landed on
  near-black against that button's own dark background.

### Added

- The Draft panel has a "Use caret/selection position" checkbox
  (persisted per browser). Unchecked, the editor's caret and any text
  selection are no longer sent as insert-point context, so the agent
  always produces a full rewrite instead of an unexpected caret insert
  or selection replace at a stale cursor position.
- "Attach file" and "Upload" on the edit page now have tooltips
  explaining what each does, since the two were easy to mix up.

## [0.4.1] - 2026-11-04

### Fixed

- The editor's Upload button no longer reads front matter with its own
  hand-written parser. The file is decoded by the new
  `POST /api/documents/parse-import` using the same yaml-cpp reader as the
  vault, so the two can no longer disagree. Values the old reader got wrong
  now import correctly: escapes in double-quoted values (`\n`, `\t`,
  `\x01`), single-quoted values with a doubled apostrophe, tags containing
  a comma, CRLF line endings (the whole block was imported as body text),
  `...` as the block terminator, and block-style tag lists (which cleared
  the Tags field).

## [0.4.0] - 2026-11-04

### Added

- The Account page is split into tabs (Password, Backup & Trash, Remote MCP,
  Agents, Embeddings). The active tab is kept in the URL hash
  (`/account#mcp`), so it survives the full page reload every navigation
  does and can be linked to directly.
- The Remote MCP tab has a "Connect a client" block: the endpoint URL
  derived from the page's own origin and base path, plus copyable examples
  for `claude mcp add`, an `mcpServers` JSON config, and a `curl`
  `tools/list` call. Examples use a `<TOKEN>` placeholder; the token itself
  is never rendered there.

### Changed

- Folder names no longer carry a trailing `/`, and tags no longer carry a
  leading `#`, in the sidebar tree, the folder page (list and heading) and
  search results. A tag group's expand arrow already marks it as a group.
  The `/` inside a multi-level tag such as `lang/python` is the hierarchy
  separator and is unchanged where the full tag is shown.

## [0.3.4] - 2026-10-21

### Added

- `Dockerfile.release` + `docker-compose.release.yml` + `tools/wiki-ops.sh`'s
  new `container-release` variant (`build`/`verify`/`deploy`) (no binary
  change, so no version bump — this only fetches and repackages existing
  release tarballs, never compiles anything): builds the runtime image
  straight from a GitHub Release's static-musl tarball instead of
  compiling from source, verifying it against the release's own
  `SHA256SUMS` before unpacking it. Defaults to the latest release;
  `--release-tag=vX.Y.Z` (or a direct `--build-arg WIKI_VERSION=vX.Y.Z`
  without the script) pins an exact tag. See `docs/docker.md` and
  `docs/wiki-ops.md`.

### Fixed

- Toast UI Editor's `dark` UI mode (used by both `dark.css` and
  `green.css`) carried its own hardcoded palette and font, unrelated to
  the active site theme's CSS variables — most visible on `green.css`,
  where the editor rendered as a plain gray UI instead of the page's
  own green-on-black look. `edit.css` now re-points the editor's
  toolbar, popups, mode-switch tabs, WYSIWYG canvas, and the
  markdown-mode code-block background at the theme's own
  `--bg`/`--panel-bg-2`/`--border`/`--link`/`--fg` variables and font
  stack. `classic.css` is unaffected — its editor runs in Toast UI's
  own light mode and was not touched.
- Every toolbar button in Toast UI Editor lost its icon on hover, in
  all three themes. Each theme's generic `button:hover` rule set
  `background` as a shorthand, which resets `background-image` to
  `none` along with the hover color — and Toast UI's toolbar icons are
  drawn as a `background-image` sprite, so the shorthand wiped them on
  every hover regardless of theme. `classic.css`, `dark.css`, and
  `green.css` now set `background-color` instead, which recolors the
  same way without touching `background-image`.
- `tools/wiki-install.sh` (no binary change, so no version bump — this
  only touches the shell script, fetched from `main` directly, not
  from a tagged release asset):
  - `systemctl enable --now` only starts a unit that isn't already
    running — on an update (a box that already has `wiki.service`
    active, the common case) that's a no-op for the "now" part, so the
    live process kept running the OLD binary indefinitely while the
    new one sat unused on disk, with `systemctl status`/healthz/a
    standalone `--version` run all looking fine. Confirmed live on a
    real deployment. Now restarts explicitly when already active,
    mirroring `cmd_systemd_install`'s own (already-correct) logic in
    `tools/wiki-ops.sh`.
  - Release binaries are the plain `WIKI_ENABLE_LOCAL_EMBEDDINGS=OFF`/
    `WIKI_ENABLE_CLOUD_EMBEDDINGS=OFF` build (`release.yml` never
    passes either flag). Swapping onto a box whose `config.toml`
    already had `embeddings.provider`/`llm.provider` set to anything
    but `"none"` made `wiki-server` exit immediately on startup, and
    `Restart=on-failure` just kept retrying it — a silent crash-loop,
    confirmed live, that took the service down until the pre-swap
    backup was restored by hand. Now refuses up front if it detects
    this, before touching anything.
- `wiki-ops.sh deploy --variant=release` had the exact same
  embeddings-provider gap as `wiki-install.sh` above — it's the same
  release binary, just reached through the SSH path instead of running
  directly on the target. Added the same `release_embeddings_guard`
  check, confirmed against the same real deployment.
- Both embeddings-provider guards only matched a double-quoted TOML
  value (`provider = "local"`) — a single-quoted one
  (`provider = 'local'`, equally valid TOML) silently passed the check.
  Both now match either quote style.

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
