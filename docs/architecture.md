# Architecture

## Overview

A personal wiki/knowledge base: markdown files on disk are the source of truth; SQLite
is a disposable secondary index (FTS5 + metadata), fully rebuildable by a full vault
rescan. The service itself is an MCP server (stdio and, optionally, remote HTTP) for
LLM clients, not a client of one — see [MCP integration](mcp.md). Auth is single-admin
(argon2id, SQLite-backed sessions). Every document has `visibility: public|private` in
its front matter, defaulting to `private` whenever the field is missing or malformed
(fail-safe); a private document returns `404`, not `403`, to an anonymous request, so
its existence is never revealed. There is no server-side HTML/JS generation: the C++
side is a pure JSON API, and every page is a static shell (`static/shell.html`) plus
client-side JS rendering from `fetch()` responses. An earlier milestone used Drogon CSP
views + htmx for server-rendered fragments; both are gone, along with the vendored htmx
bundle.

## Component layout

`libwikicore` (`src/core/`, `src/vault/`, `src/index/`, `src/config/`, `src/util/`)
holds vault access, the SQLite index/search, and MCP tool logic, with no dependency on
Drogon or OpenSSL. `src/auth/` and `src/controllers/` are a web-only concern and are
**not** part of wikicore. Two executables link wikicore:

- `wiki-server` — HTTP via Drogon, `src/main.cpp` + `src/auth/` + `src/controllers/`.
  Rescans the vault unconditionally on every startup (the index is never trusted on
  faith) and also on `--reindex` / `POST /api/admin/reindex`.
- `wiki-mcp` — stdio MCP entrypoint, `src/mcp_main.cpp` + `src/mcp/McpServer.cpp`,
  spawned directly by an MCP client. No per-request auth (the process spawn itself is
  the trust boundary). Does **not** rescan the vault at startup — a client may spawn it
  often, and a full walk on every spawn would defeat the "starts instantly" requirement;
  it trusts the existing index.

## Storage and indexing

- `vault::DocumentService` is the only thing that writes a document: assembles front
  matter, writes atomically (temp file + `rename()`, atomic within one filesystem), then
  calls `index::IndexUpdater::upsertOne` — in that order, so the index is never updated
  for content that didn't actually land on disk. Soft-delete `rename()`s the document
  and its `<stem>.assets/` folder into `.trash/<same relative path>`, rather than
  removing anything.
- Document rename/move re-paths the existing SQLite row in place
  (`IndexUpdater::repathOne`) rather than delete+insert, so `rowid_id` — and therefore
  `document_snapshots`/history — survives the move. Folder move applies the same
  contract over a path prefix. Both rewrite inbound `[[wiki-link]]`s and
  `assets/<stem>.assets/` hrefs in other documents and reindex those sources.
- `index::IndexBuilder::fullRescan()` runs at every `wiki-server` startup and on demand;
  `index::VaultWatcher` is an inotify-based background thread that picks up changes made
  while the server is already running (external editor, `git pull`, a sync tool) within
  about a second, debounced per path. It is best-effort, not a correctness guarantee —
  `--reindex` remains the authoritative fallback.
- `VaultWatcher::start()` blocks until its initial recursive watch registration
  completes (a `std::promise`/`std::future` the watcher thread fulfills), rather than
  returning immediately and registering the watch asynchronously: the kernel cannot
  queue an inotify event for a watch that doesn't exist yet, so a disk change landing in
  that window would otherwise be lost permanently, not merely delayed.
- `VaultWatcher` uses its own SQLite connection, never the one Drogon's request threads
  share. `IndexUpdater::upsertOne`/`removeOne` each wrap a `BEGIN IMMEDIATE...COMMIT`
  sequence under a `std::mutex`; two threads racing a `BEGIN` on the *same* connection
  handle is a hard SQLite error ("cannot start a transaction within a transaction"), not
  safe serialization — separate connections to the same WAL-mode file coordinate
  correctly at the file level, which a single shared connection does not. Read-only
  queries (`allIndexedPaths`, `rowIdForPath`, `findPathByUuid`) are not guarded by the
  mutex: each is a single prepare/step/destroy with no transaction window for another
  thread to land inside.
- FTS5 `snippet()` reads raw, unfiltered document text and inserts its own match
  markers. Those markers are bound as control bytes (`\x01`/`\x02`), never literal
  `<mark>`/`</mark>` in the SQL text; at render time the whole snippet is HTML-escaped
  first, and only afterward are the control bytes swapped for real `<mark>` tags. Doing
  this in the other order either breaks the tags or lets raw document content through
  unescaped — there is no safe order with literal markers in the SQL.
- Nav/tag queries are visibility-aware: a tag used only by private documents
  contributes no count at all to an anonymous caller, not zero displayed as a real tag.

## Auth and security

- Registering a Drogon `HttpFilter` by name has two requirements: `registerHandler`
  looks filters up in `DrClassMap` by their fully-qualified, demangled type name (e.g.
  `wikicore::auth::AuthFilter`, not the bare class name), and nothing registers the
  filter with `DrClassMap` at all unless something in the program ODR-uses
  `YourFilter::classTypeName()` (a forced call in `main.cpp`, right before route
  registration) — a static member of a class template is only instantiated if
  referenced. Getting either wrong fails silently at runtime (`middleware X not found`
  in the log), not at build time.
- CSRF token delivery: the synchronizer token lives server-side in
  `sessions.csrf_token`; it reaches the client via a second, non-HttpOnly cookie
  (`wiki_csrf_token`) set alongside the session cookie at login. `CsrfFilter` validates
  against the server-side value — the cookie is only the delivery channel.
- **Neither `AuthFilter` nor `CsrfFilter` blocks an unauthenticated request by itself.**
  `AuthFilter` only annotates the request; `CsrfFilter` passes a request with no session
  straight through, leaving the authorization decision to the handler. Every mutating
  handler calls `requireAdminApi(req)` as its first line for exactly this reason —
  listing these filters on a route is not proof the route is protected; the handler code
  is.
- `auth::clientIp()` takes a `trustProxyHeaders` bool (`main.cpp` passes
  `!cfg.tls.enabled` — derived from whether standalone TLS is on, not a separate
  config knob, since that already tells you whether a reverse proxy can possibly be
  in front). Only when true does it trust `X-Real-IP` first, then `X-Forwarded-For`'s
  **last** entry, never its first, falling back to `req->getPeerAddr()` otherwise. A
  correctly configured reverse proxy appends to `X-Forwarded-For`
  (`$proxy_add_x_forwarded_for`) rather than overwriting it, so a client can prepend any
  IP it likes — the first entry is attacker-controlled, the last is always the proxy's
  own append — but only trustworthy at all when a proxy is actually there to have done
  that (`tls.enabled = false`, the nginx-fronted default). Any code that needs the
  caller's real IP (an allowlist, a per-IP rate limit) must go through this function
  with the right `trustProxyHeaders` value, never `req->getPeerAddr()` directly and
  never assume the header is safe unconditionally — in standalone TLS mode there is no
  proxy, `getPeerAddr()` is already the real caller, and the header is spoofable by
  that same direct caller.
- **Standalone TLS** (`[tls]` in `config.toml`) terminates TLS inside `wiki-server`
  itself — a second, equally supported alternative to the nginx-fronted shape, not a
  replacement for it. `src/server/CertWatcher` (OpenSSL-dependent, so it lives outside
  `wikicore` like `auth/`/`controllers/`) watches the cert file's directory for a
  certbot renewal and calls `drogon::app().reloadSSLFiles()` — but only after
  independently confirming both the cert and key still parse AND that the key matches
  the certificate (`X509_check_private_key`), since trantor's own reload path throws
  on a mismatch and an uncaught throw there kills that event-loop thread; certbot
  updates the cert/key symlinks as two separate filesystem writes, so a transiently
  mismatched pair during a renewal is real, not hypothetical. `listen_addr` being
  non-loopback is NOT validated by `AppConfig::load()` either with or without
  `tls.enabled` — Docker's own `docker/config.docker.toml` needs `0.0.0.0` with no
  `[tls]` table at all (the container's network namespace controls real exposure
  there, not this value), so there is no single rule that's correct for every
  deployment shape; this field stays comment-only guidance.
- CIDR prefix parsing rejects a negative parsed value explicitly, rather than letting it
  fall through to the same sentinel used for "no prefix given" (which would silently
  become `/32`).
- Static files are served only from `static/` (`app().setDocumentRoot("static")`),
  never the project root — otherwise Drogon's static-file fallback would expose
  `config.toml` and source over HTTP.
- `vault::createVaultBackup` shells out to the system `tar` via `fork()`+`execlp()`,
  never `system()`/`popen()`: both of the latter run the command through `/bin/sh -c`,
  so a vault path containing a shell metacharacter becomes a parsing hazard; `execlp()`
  takes an explicit argv with no shell involved. The backup excludes `.uploads-tmp/`
  (Drogon's transient multipart-upload staging directories) and `.mcp-uploads/`
  (one-shot remote-MCP upload tickets) — neither holds real content. The opt-in systemd
  timer (`systemd/wiki-backup.sh`) reimplements the same `tar` invocation standalone
  rather than calling the admin HTTP endpoint, so a disaster-recovery backup keeps
  working even when `wiki-server` is down.
- Security response headers (CSP, `X-Content-Type-Options: nosniff`,
  `X-Frame-Options: DENY`, `Referrer-Policy: strict-origin-when-cross-origin`) are added
  via `registerPreSendingAdvice`, not `registerPostHandlingAdvice` — Drogon only runs
  static-file responses through the former. The CSP allows `script-src 'self'
  'unsafe-inline'` because `shell.html` carries two inline bootstrap scripts; `worker-src
  'self'` is spelled out explicitly for `graph-layout.js`'s dedicated Worker rather than
  relying on a `script-src` fallback. `frame-src https://www.youtube.com` is the one
  cross-origin iframe exception, for YouTube embeds.
- `systemd/wiki.service` runs with `ProtectSystem=strict`, `NoNewPrivileges`,
  `ProtectHome`, `PrivateTmp`, a restricted `CapabilityBoundingSet`,
  `RestrictAddressFamilies`/`RestrictNamespaces`/`RestrictSUIDSGID`/`RestrictRealtime`,
  `LockPersonality`, `MemoryDenyWriteExecute`, the `Protect{KernelTunables,
  KernelModules,KernelLogs,ControlGroups,Clock,Hostname}` family, `PrivateDevices`, and
  `SystemCallArchitectures=native`. `SystemCallFilter=@system-service` is deliberately
  **not** set: it crashed the ARM/musl production target with a seccomp kill
  (`code=killed, status=31/SYS`) that x86_64/glibc testing did not reproduce — the
  allowed syscall set under `@system-service` is itself architecture/libc-dependent.
  `qemu-arm-static` cannot catch this class of regression either way, since it emulates
  instructions, not kernel-level seccomp filtering.
- `wiki.env.example` / `EnvironmentFile=-/etc/opt/wiki/wiki.env` follows FHS
  (`/opt/wiki` → `/etc/opt/wiki/`); the leading `-` keeps the unit startable when the
  file is absent. `AppConfig` does not map config keys from the environment — the only
  runtime secret consumers are `CloudEmbeddingProvider`/`CloudChatClient`, which
  `getenv()` whatever `[embeddings].api_key_env`/`[llm].api_key_env` names (see
  [embeddings](embeddings.md), [llm](llm.md)). Admin credentials live in SQLite;
  sessions and remote-MCP tokens are random opaque values, not signed from this file.
  Deployments using `provider = "none"`/`"local"` don't need the file at all.
- `tests/integration/security_e2e.py` (wired through `ctest`) exercises anonymous writes
  against every mutating route, CSRF, path traversal, session fixation (an
  attacker-chosen session cookie set before login is never adopted — the server always
  issues a fresh token on successful auth), visibility gating across `/d/`,
  `/api/search`, `/api/nav/tree`, `/api/nav/tags`, `/assets/`, rate limiting, and a live
  `VaultWatcher` check. `tests/integration/stress_concurrency.py` /
  `stress_resources.py` (label `stress`, excluded by `ctest -LE stress`) separately cover
  concurrency races and resource-exhaustion behavior. `tests/fuzz/` (off by default,
  `WIKI_ENABLE_FUZZING`, Clang-only) carries coverage-guided libFuzzer harnesses for
  `PathGuard::resolve()` and `parseFrontMatter()`, the two parsers that take raw
  untrusted bytes directly.
- A path segment longer than the filesystem's `NAME_MAX` throws
  `std::filesystem::filesystem_error`, whose `.what()` embeds the full absolute on-disk
  path. Every mutating route's generic `catch (const std::exception&)` must come after
  an explicit `catch (const std::filesystem::filesystem_error&)` that returns a clean
  `400`/`"invalid path"` — `DocumentRoutes.cpp`, `FolderRoutes.cpp`,
  `VersionRoutes.cpp`'s `document-restore` handler, and `RemoteMcpRoutes.cpp`'s
  `create_document`/`update_document` all need this; the generic catch alone leaks the
  absolute vault path to the caller. `RemoteMcpRoutes.cpp` still logs the full detail to
  `mcp_audit_log` (admin-only) while returning only `"invalid path"` to the client.
- `IndexUpdater`'s mutex (above) exists because concurrent writes to the same document
  path previously raced the shared-connection transaction and could surface SQLite's own
  stale error text as a spurious `500`.
- Remote MCP's bearer-token comparison uses `auth::constantTimeEquals` (wrapping
  OpenSSL's `CRYPTO_memcmp`), not `==` — this check gates authentication itself for an
  anonymous caller on a public endpoint, unlike `CsrfFilter`'s comparison, which guards
  an already-authenticated session and does not need to be constant-time.
- No automated CVE scanner currently covers this project's vcpkg C/C++ dependencies as a
  whole: neither OSV-Scanner nor Trivy has a vcpkg extractor, and GitHub's Dependency
  Graph does not list vcpkg among its supported ecosystems (only NuGet, for C/C++).
  OSV.dev's commit-based query (`POST /v1/query {"commit": "<sha>"}`) against each
  dependency's exact pinned upstream commit is a working manual spot-check technique,
  not a CI-automatable scan. `overlay-ports/md4c/` carries a patch for `OSV-2022-126` (a
  heap-buffer-overflow read in `md_analyze_table_alignment()`, present in the pinned
  `release-0.5.3`, fixed upstream but not yet in a tagged release) — drop the overlay
  once `vcpkg.json`'s `builtin-baseline` advances past a release that includes the fix.
  GitHub Dependabot alerts do cover `.github/workflows/ci.yml`'s GitHub Actions
  dependencies, a disjoint set from vcpkg's C/C++ packages.
- CodeQL's `paths`/`paths-ignore` config has no effect once a compiled language is
  analyzed with real build steps (`build-mode: manual`) — GitHub's own docs state this
  applies only "when you analyze a compiled language without building the code." The
  workflow instead runs the dependency-installing CMake configure step *before*
  `codeql-action/init` enables compiler tracing, so vcpkg's own from-source dependency
  builds happen outside the traced window and don't surface as alerts against vendored
  code.

## Content rendering pipeline

- Markdown: **md4c**, not cmark-gfm — lighter, plain C, GFM extensions
  (tables/strikethrough/tasklists) via parser flags. Runs with
  `MD_FLAG_NOHTMLBLOCKS`/`SPANS` — raw HTML passthrough is disabled, and that flag *is*
  the sanitization; there is no separate pass.
- Front matter: **yaml-cpp**, not a hand-rolled parser — front matter gets hand-edited
  outside the app too, so real YAML edge cases (the "Norway problem", arbitrary
  quoting/lists/dates) are better delegated to a battle-tested library.
- `[[wiki-link]]`/`[[target|label]]` syntax is rewritten to a plain CommonMark
  `[label](d/target)` link by a hand-rolled scanner (`util/WikiLinks.cpp`, no
  `std::regex`) **before** `renderMarkdownToHtml` runs — md4c never sees the bracket
  syntax. `normalizeTarget()` (trim, strip a leading `/`, append `.md` if missing) keeps
  the write side (`document_links`, via `IndexUpdater`) and read side
  (`NavQueries::backlinks`) in agreement. `target_path` is plain `TEXT`, not a foreign
  key, so a link to a document that doesn't exist yet is recorded as a "red link" and
  starts resolving the moment a document lands at that path. The generated href must
  carry a `d/` prefix: `shell.html` sets `<base href="{basePath}/">`, so a relative href
  resolves against the mount root (route space, `/d/{path}`), while
  `normalizeTarget()`'s output is in file space (the vault's own layout) — the two
  happen to look identical for a simple target but are not the same path space.
- YouTube embeds and Mermaid diagrams both use a marker-then-substitute scheme to work
  around `MD_FLAG_NOHTMLBLOCKS`/`SPANS`: a pre-pass turns a recognized construct into
  inert, ordinary CommonMark (`![](youtube-embed:ID)` for a recognized URL; a fenced
  ` ```mermaid ` block is already first-class CommonMark and needs no pre-pass rewrite
  at all), md4c renders that as inert HTML, and only afterward does a post-substitution
  step swap the resulting tag for the real `<iframe>`/`<pre class="mermaid">`. The
  YouTube video ID is validated to exactly 11 URL-safe characters at both ends of this
  round trip. `rewriteYouTubeEmbeds` runs from inside `renderMarkdownToHtml` itself
  (unlike `WikiLinks`, which the caller chains in beforehand), because turning `<img>`
  into `<iframe>` requires touching md4c's own HTML output.
  - A hand-typed `<img src="youtube-embed:ID">` in a document body cannot forge this:
    literal HTML is `&lt;`-escaped by md4c like any other tag. A hand-typed *CommonMark*
    image (`![](youtube-embed:ID)`) is not raw HTML and does get substituted — this
    grants no new capability today (the single admin who can write documents can already
    embed any video via a real URL) but means the substitution step is reachable without
    going through `rewriteYouTubeEmbeds`'s own URL-recognition step.
- Mermaid rendering is lazy and entirely client-side (`static/js/mermaid-render.js`):
  the ~5.3 MiB vendored `mermaid.min.js` bundle is fetched only when a page actually
  contains a `pre.mermaid` block, since the deployment target includes low-power SBC
  hardware over possibly slow links. Diagram colors use `theme: "base"` with
  `themeVariables` read live from the page's own CSS custom properties
  (`--panel-bg`/`--fg`/`--fg-dim`), rather than a second hardcoded palette that could
  drift from the CSS — mermaid's theming only accepts hex colors, so a theme whose
  `--border` is an `rgba()` value falls back to `--fg-dim` (hex in every theme) for
  diagram borders/lines.
  - The editor's live preview plugs Toast UI Editor's `customHTMLRenderer.codeBlock`
    hook and reuses the same lazy-loader; that hook's return value is honored only by
    the Markdown-mode Preview panel — the WYSIWYG canvas always keeps a code block as
    its own editable widget. Because the Preview panel is `display:none` whenever the
    Write tab is active, and mermaid marks a block "already processed" after one render
    attempt regardless of the result, a diagram whose first render happened while hidden
    would otherwise stay at zero height permanently; `refreshPreview()` checks computed
    `display` before rendering, and a `MutationObserver` watches the Preview panel's own
    `display` toggle (no public editor event fires for that specific tab switch).
- Fenced ` ```query ` blocks render as a live, re-queried-on-every-view index/dashboard.
  `index::QueryBlocks::parseAndRun` never builds SQL from the block's own text: the
  grammar is a fixed set of `key: value` lines (`tag`, `type`, `folder`, `orphans`,
  `sort`, `order`, `limit`, `search`), each recognized key maps to one hardcoded SQL
  fragment, and a value only ever reaches SQLite as a bound parameter or as a lookup
  against a small in-code whitelist substituted for a literal column name — there is no
  code path where document text becomes part of the SQL string. Visibility gating comes
  only from the *viewing* caller's own session, never from what the block's author could
  see at authoring time. An unknown key, duplicate key, or out-of-range value is a `400`
  parse error, never a silently empty or unfiltered result. `search:` delegates to the
  same `FtsSearch&` instance `/api/search` uses (hybrid semantic search when embeddings
  are configured) rather than reimplementing ranking; combining `search:` with
  `sort`/`order`/`orphans` is a parse error, since relevance order and DB order don't
  compose. All parameterized conditions are appended to one list, in the same order
  their binder is pushed, specifically to avoid SQLite's numbered-vs-anonymous `?`
  placeholder interaction (a plain `?` is assigned the next number after the largest
  explicit `?N` already seen *in text-parse order*, not construction order — mixing an
  explicitly-numbered placeholder ahead of a clause with its own anonymous placeholders
  silently mis-binds values).
- Heading-level "zoom" (`static/js/section-zoom.js`) hides/shows DOM siblings around a
  clicked heading; it operates on rendered heading structure, not on the markdown
  source, since the content model is a flat file with no addressable blocks. `h1` is
  excluded, since a document conventionally opens with one that already represents the
  whole document. Zoom state lives in a `#zoom=<slug>` hash set via
  `history.replaceState` rather than a bare hash assignment, so the Back button doesn't
  step through every zoom click individually.

## Attachments and versioning

- Attachments live in a co-located `<doc-name>.assets/` folder next to their document.
  No extension allowlist (the serving-side `isSafeToRenderInline` check is the actual
  safety boundary); filenames are sanitized to `[A-Za-z0-9._-]`; no app-level size cap
  (disk is the bound) — uploads are still bounded by Drogon's `setClientMaxBodySize`
  and the reverse proxy's own body-size limit. `GET /assets/{path...}` derives the
  owning document from the `.assets/` folder name to apply the same visibility gating as
  `/d/`, rather than storing a separate visibility flag on the file itself.
- MCP large-file upload avoids base64 entirely: local stdio uses `attach_file`'s
  `source_path` (a streamed `copy_file`); remote HTTP uses `attach_file_begin` (issues a
  UUID ticket over a normal Bearer-authenticated call) followed by
  `PUT /mcp/uploads/{id}` carrying the raw bytes with **no** `Authorization` header —
  the UUID itself is the secret for that one call. `source_path` is rejected on the
  remote transport, since it would let a caller read arbitrary files off the wiki host.
- `document_snapshots` holds pre-edit content: `DocumentService::update` snapshots a
  document's raw content before every overwrite; `create` snapshots nothing (there is no
  "before" state). `SnapshotStore::getContent` checks both `documentRowId` and
  `snapshotId` together, never `snapshotId` alone, closing off an IDOR-shaped bug class
  (guessing another document's snapshot id) by construction. Restoring a version itself
  snapshots the pre-restore state first, so a restore is exactly as undoable as any other
  edit.

## Frontend

- `shell.html` reads a saved theme choice from `localStorage` via a synchronous inline
  `<script>` at the very top of `<head>`, before any other stylesheet parses, and
  creates (not retargets) a `<link id="theme-link">` pointing at the right
  `css/themes/*.css` file. This app does a full page reload on every navigation, so a
  static `<link>` retargeted by a later script would start fetching the wrong theme
  first on every single reload, not just on first load.
- The three theme stylesheets (`classic`, `dark`, `green`) are fully self-sufficient
  files, each duplicating the full selector list with different values, rather than one
  shared base plus per-theme overrides — deliberate, since this project has no CSS
  build step or preprocessor, and three complete files mean a fourth theme later is
  "write one new file," never "figure out which of several files a given rule lives
  in." `[server].theme` sets the default for a browser with nothing saved yet
  (injected into `shell.html` the same way `[server].base_path` is); a reader's own
  saved choice always wins over it.
- A `<button>` styled only by a CSS class selector needs that class actually present on
  the element — the theme-toggle button once had the `id` `theme.js` needed but not the
  `class` every theme file's CSS actually targeted, so none of that styling ever
  applied. Worth stating because the symptom (a styling change "doing nothing") looks
  identical to a specificity problem.
- The Toast UI Editor's own `theme` option follows the active site theme
  (`<html data-theme>`, read at editor-construction time): `classic` → Toast UI's
  `"light"` (its actual documented default — confirmed against the vendored bundle
  itself, not assumed from the `"dark"` option's name), `dark`/`green` → `"dark"`. No
  live re-theming is needed, since a theme change already triggers a full page reload.
- Page-specific stylesheets (`edit.css`, the vendored Toast UI Editor CSS) must be
  injected from script *after* `<base>` is set, exactly like theme CSS, never as a
  static `<link href="css/...">` tag in the markup: the browser's HTML preload scanner
  fetches stylesheets in parallel with the first `<head>` script, resolving the URL
  against the *document* path, not a `<base>` a later script hasn't appended yet. Under
  a mounted subpath (e.g. `/wiki/edit/{doc}`) a static tag resolves to
  `/wiki/edit/css/edit.css` — this app's own SPA shell, served as `text/html` — which
  Chrome refuses to load as a stylesheet.
- Toast UI Editor insert of an attachment or link must go through
  `editor.exec("addLink"/"addImage")`, not `insertText("[name](url)")`: the latter is
  correct only in Markdown mode. In WYSIWYG it dumps the markdown syntax as literal text
  and auto-linkifies only the bracketed portion, leaving `](url)` visible.
- Toast UI Editor's WYSIWYG markdown writer cannot round-trip `[[wiki-link]]` syntax —
  it escapes the brackets on output. The editor maps `[[path|label]]` ↔
  `[label](wiki:path)` around `setMarkdown`/`getMarkdown` (`static/js/common.js`); the
  `wiki:` URL never reaches disk.
- A free-text input with a suggestions dropdown (`createTypeahead`, used for the edit
  form's Type/Tags fields) is a different widget from the closed-set multiselect used
  for search filters, even though both read from the same nav endpoints — Type/Tags must
  stay able to accept a genuinely new value. The dropdown renders as a portal appended
  to `document.body` with `position: fixed`, positioned from the input's own
  `getBoundingClientRect()`, rather than nested inside the form: Toast UI Editor nests
  several of its own positioned containers deep enough that a nested dropdown cannot
  reliably win a z-index comparison against the editor's WYSIWYG body text.
- Drogon's `registerHandlerViaRegex` matches regex handlers in registration order, first
  match wins. A general `"^/api/documents/(.*)$"` handler registered before a more
  specific `"^/api/documents/(.*)/raw$"` one will greedily swallow the `/raw` suffix in
  its own capture group — the more specific route must be registered first.

## Graph view

- `index::GraphQueries` backs `GET /api/graph` with the same fail-safe-private
  discipline as the rest of the app: an edge appears only when *both* endpoints are
  visible to the caller, so a private document cannot leak its existence via an edge
  from a public one. A "red link" (target doesn't currently exist) is silently excluded
  from the edge list rather than drawn as a placeholder node.
- `GET /api/graph?around={path}` (the per-document "local graph") returns the full
  connected component over visible edges, not a fixed 1-hop neighborhood — a private
  document is never a stepping stone to a further public one for an anonymous caller.
  `around=` is treated as untrusted input: resolved through `PathGuard` and an exact
  bound-path lookup, never `LIKE`.
- Layout (`static/js/graph-layout.js`) is a from-scratch Barnes-Hut force simulation run
  in a dedicated Worker, not a vendored physics library — repulsion is approximated via
  a quadtree above a size threshold and falls back to plain pairwise repulsion for
  smaller graphs (the local-graph case). Initial node placement is a deterministic
  circle, not random, so the same graph settles into a recognizably similar layout
  across reloads.
- Rendering (`static/js/graph-render.js`) picks the first backend that actually works
  from a capability ladder — WebGL, then canvas 2D, then SVG — probed with a real
  context/shader creation rather than checking whether a constructor merely exists. SVG
  nodes are real `<a href="/d/...">` elements for ordinary navigation and keyboard
  access; canvas/WebGL hit-test in JS and navigate the same URLs, backed by a
  visually-hidden `<nav>` of the same links.
- The full `/graph` page adds a toolbar once a vault is large enough for per-node labels
  to become unreadable: a text filter dims non-matching nodes without removing them from
  the layout (so typing doesn't restart the simulation); "hide unlinked" drops degree-0
  nodes before layout; label visibility has `auto`/`On hover`/`All` modes. The
  single-component local-graph widget skips this toolbar entirely.

## MCP and LLM integration

Read/search/write tools over stdio and an optional admin-toggleable remote HTTP
transport — see [MCP integration](mcp.md) for the full tool list, the write-access
gate, and the remote transport's own auth model. Optional cloud-assisted drafting (edit
page) and vault Q&A (sidebar chat) are covered in [LLM integration](llm.md); both route
through the same `QueryBlocks::parseAndRun`/`CalendarQueries::eventsBetween` engines
the HTTP API and MCP tools use, rather than a separate implementation per caller.

## Build

```sh
# one-time: clone + bootstrap vcpkg (not committed to git)
#
# Always a FULL clone, never --depth 1. vcpkg.json pins builtin-baseline to a
# specific commit; manifest mode resolves every dependency's version via
# `git show <that commit>:versions/baseline.json` against vcpkg's own repo
# history. A shallow clone only contains whatever commit happens to be
# upstream's current tip, which drifts away from an older pin with nothing
# about the clone command itself changing.
git clone https://github.com/microsoft/vcpkg.git vcpkg
./vcpkg/bootstrap-vcpkg.sh -disableMetrics

# configure + build (installs deps from vcpkg.json automatically)
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"

# run
./build/wiki-server        # listens on 127.0.0.1:8080, GET /healthz
```

Package manager is vcpkg in manifest mode; `vcpkg/` and `vcpkg_installed/` are
gitignored, never vendored. `overlay-ports/` carries local patches for a pinned
dependency version ahead of its own upstream fix landing in a tagged release (see
"Auth and security" above).
