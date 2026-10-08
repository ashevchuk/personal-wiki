# Architecture

## Overview

The core idea: markdown files on disk are the only real data. SQLite is just
a search index built on top of them — if it's lost or corrupted, a full
vault rescan rebuilds it from scratch, with nothing missing.

A few other decisions shape everything else in this document:

- The service is itself an MCP server (over stdio, and optionally remote
  HTTP) that LLM clients connect *to* — it is not a client that calls out to
  an LLM API on its own behalf. See [MCP integration](mcp.md).
- There is exactly one admin account (argon2id password hashing,
  SQLite-backed sessions) — no multi-user system.
- Every document has `visibility: public` or `private` in its front matter.
  If that field is missing or malformed, the document is treated as
  private — never the other way around. An anonymous request for a private
  document gets a plain `404`, not `403`, so even the fact that the
  document exists is never revealed.
- There's no server-side HTML templating. The C++ side is a pure JSON API;
  every page is the same static shell (`static/shell.html`) plus
  client-side JS that renders whatever `fetch()` returns. An earlier
  version used server-rendered fragments (Drogon views + htmx); both are
  gone now, along with the vendored htmx bundle.

## Component layout

Everything that touches the vault and the search index lives in one shared
library, `libwikicore` (`src/core/`, `src/vault/`, `src/index/`,
`src/config/`, `src/util/`). It has no dependency on Drogon or OpenSSL —
`src/auth/` and `src/controllers/` are web-only concerns and are
deliberately kept outside it.

Two executables link against `libwikicore`:

- **`wiki-server`** (`src/main.cpp` + `src/auth/` + `src/controllers/`) is
  the HTTP service, built on Drogon. It rescans the entire vault on every
  startup — the index is never trusted blindly — and can also be rescanned
  on demand via `--reindex` or `POST /api/admin/reindex`.
- **`wiki-mcp`** (`src/mcp_main.cpp` + `src/mcp/McpServer.cpp`) is the stdio
  MCP entrypoint, spawned directly by an MCP client. It has no per-request
  authentication — the act of spawning the process *is* the trust boundary.
  Unlike `wiki-server`, it does **not** rescan the vault at startup: an MCP
  client may spawn it frequently, and a full rescan on every spawn would
  break the "starts instantly" expectation. It simply trusts whatever index
  already exists.

## Storage and indexing

**Writing a document.** `vault::DocumentService` is the only code path
that writes a document. It assembles the front matter, writes the file
atomically (temp file + `rename()`, which is atomic on one filesystem), and
only then calls `index::IndexUpdater::upsertOne`. That order matters: the
index is never updated for content that didn't actually make it to disk.
Deleting a document doesn't remove it — it moves the document and its
`<stem>.assets/` folder into `.trash/` at the same relative path.

**Renaming and moving.** A rename or move re-paths the existing SQLite row
in place (`IndexUpdater::repathOne`) instead of deleting and re-inserting
it. That preserves `rowid_id`, which means a document's edit history
(`document_snapshots`) survives the move too. Moving a whole folder applies
the same rule across every path under it. Either operation also rewrites
any inbound `[[wiki-link]]`s and `assets/<stem>.assets/` references in
*other* documents, and reindexes those documents.

**Keeping the index current.** There are two mechanisms:

- `index::IndexBuilder::fullRescan()` runs on every `wiki-server` startup,
  and on demand. This is the authoritative one.
- `index::VaultWatcher` is a background thread (using inotify) that notices
  changes made while the server is already running — an external editor,
  `git pull`, a sync tool — usually within about a second, debounced per
  path. It's best-effort, not a correctness guarantee; `--reindex` is
  always the fallback if it ever misses something.

  Two implementation details make the watcher actually safe:
  - `VaultWatcher::start()` blocks until its first recursive watch
    registration has actually completed, using a `std::promise`/`std::future`
    the watcher thread fulfills. It does *not* return immediately and
    register the watch asynchronously — the kernel can't queue an inotify
    event for a watch that doesn't exist yet, so a change landing in that
    gap would be lost forever, not just delayed.
  - `VaultWatcher` uses its own SQLite connection, separate from the one
    Drogon's request threads share. `IndexUpdater::upsertOne`/`removeOne`
    each wrap a `BEGIN IMMEDIATE…COMMIT` sequence under a `std::mutex`,
    because two threads racing a `BEGIN` on the *same* connection handle is
    a hard SQLite error, not something that serializes safely on its own.
    Two separate connections to the same WAL-mode file coordinate
    correctly at the file level; one shared connection does not. Read-only
    queries (`allIndexedPaths`, `rowIdForPath`, `findPathByUuid`) skip the
    mutex on purpose — each is a single prepare/step/destroy with no
    transaction window for another thread to land inside.

**Search snippets.** FTS5's `snippet()` reads raw, unfiltered document text
and inserts its own match markers. Those markers are bound as control
bytes (`\x01`/`\x02`), never literal `<mark>`/`</mark>` in the SQL text.
At render time, the whole snippet is HTML-escaped *first*, and only
afterward are the control bytes swapped for real `<mark>` tags. Reversing
that order either breaks the tags or lets raw document content through
unescaped — there's no safe way to do it with literal markers in the SQL.

**Tag counts respect visibility.** A tag used only on private documents
contributes no count at all to an anonymous caller — not a zero displayed
next to a real tag name, which would still confirm the tag exists.

## Auth and security

### Registering a Drogon filter

Registering an `HttpFilter` by name has two separate requirements, and
missing either one fails silently at runtime (a `middleware X not found`
log line, not a build error):

1. `registerHandler` looks filters up in `DrClassMap` by their
   fully-qualified, demangled type name (e.g. `wikicore::auth::AuthFilter`,
   not the bare class name).
2. Nothing registers the filter with `DrClassMap` at all unless something
   in the program actually references `YourFilter::classTypeName()` — a
   static member of a class template is only instantiated if it's used
   somewhere. `main.cpp` has a forced call to this right before route
   registration, specifically to trigger that instantiation.

### CSRF

The CSRF token itself lives server-side, in `sessions.csrf_token`. It
reaches the browser through a second cookie (`wiki_csrf_token`, not
HttpOnly) set alongside the session cookie at login. `CsrfFilter` checks
the request against the server-side value — the cookie is only how the
value gets delivered, never trusted as the source of truth.

**Neither `AuthFilter` nor `CsrfFilter` blocks an unauthenticated request
by itself.** `AuthFilter` only annotates the request with what it found;
`CsrfFilter` lets a request with no session through unchecked, leaving
that decision to the handler. Every mutating handler calls
`requireAdminApi(req)` as its first line for exactly this reason — seeing
these filters listed on a route is not proof that the route is protected.
The handler code is what actually decides.

### Trusting the client's IP address

`auth::clientIp()` takes a `trustProxyHeaders` flag. `main.cpp` passes
`!cfg.tls.enabled` for it — derived from whether standalone TLS is on,
rather than a separate config setting, since that already tells you
whether a reverse proxy could possibly be sitting in front.

Only when that flag is true does the function trust `X-Real-IP` first,
then the **last** entry of `X-Forwarded-For` (never the first), falling
back to `req->getPeerAddr()` otherwise. The reason the *last* entry matters:
a correctly configured reverse proxy appends to `X-Forwarded-For`
(`$proxy_add_x_forwarded_for`) rather than overwriting it, so a client can
prepend any IP address it likes — the first entry is attacker-controlled,
the last is always the proxy's own append. But that's only true at all
when a proxy is actually there to have done the appending, which is
exactly what `tls.enabled = false` (the nginx-fronted default) asserts.

Any new code that needs the caller's real IP — an allowlist, a per-IP rate
limit — must go through this function with the right `trustProxyHeaders`
value. Never use `req->getPeerAddr()` directly, and never trust the header
unconditionally: in standalone TLS mode there is no proxy, so
`getPeerAddr()` is already the real caller, and the header is spoofable by
that same caller.

### Standalone TLS

`[tls]` in `config.toml` lets `wiki-server` terminate TLS itself, as a
second option alongside the nginx-fronted setup — not a replacement for it.

`src/server/CertWatcher` (kept outside `wikicore`, like `auth/` and
`controllers/`, because it needs OpenSSL) watches the certificate file's
directory for a certbot renewal and calls `drogon::app().reloadSSLFiles()`.
Before doing that, it independently checks that both the cert and key still
parse, *and* that the key actually matches the certificate
(`X509_check_private_key`). This matters because trantor's own reload path
throws on a mismatch, and an uncaught throw there kills that event-loop
thread — and certbot writes the cert and key symlinks as two separate
filesystem operations, so a transiently mismatched pair during a renewal
is a real possibility, not a hypothetical one.

`AppConfig::load()` does **not** reject a non-loopback `listen_addr`, with
or without `tls.enabled`. Docker's own `docker/config.docker.toml` needs
`0.0.0.0` with no `[tls]` table at all — in that setup, the container's
network namespace controls real exposure, not this value. There's no
single rule that's correct for every deployment shape, so this field stays
comment-only guidance rather than an enforced check.

### Smaller things worth knowing

- CIDR prefix parsing explicitly rejects a negative parsed value, rather
  than letting it fall through to the same sentinel used for "no prefix
  given" — which would otherwise silently become `/32`.
- Static files are served only from `static/`
  (`app().setDocumentRoot("static")`), never the project root. Serving from
  the project root would let Drogon's static-file fallback expose
  `config.toml` and source files over HTTP.
- `vault::createVaultBackup` shells out to the system `tar` via
  `fork()`+`execlp()`, never `system()`/`popen()`. Both of the latter run
  the command through `/bin/sh -c`, so a vault path containing a shell
  metacharacter becomes a parsing hazard; `execlp()` takes an explicit
  argv with no shell involved at all. The backup skips `.uploads-tmp/`
  (Drogon's transient multipart-upload staging directory) and
  `.mcp-uploads/` (one-shot remote-MCP upload tickets) — neither holds
  real content. The opt-in systemd timer (`systemd/wiki-backup.sh`)
  reimplements the same `tar` invocation standalone, rather than calling
  the admin HTTP endpoint, so a disaster-recovery backup still works even
  when `wiki-server` itself is down.
- Security response headers (CSP, `X-Content-Type-Options: nosniff`,
  `X-Frame-Options: DENY`, `Referrer-Policy: strict-origin-when-cross-origin`)
  are added via `registerPreSendingAdvice`, not
  `registerPostHandlingAdvice` — Drogon only runs static-file responses
  through the former. The CSP allows `script-src 'self' 'unsafe-inline'`
  because `shell.html` has two inline bootstrap scripts; `worker-src
  'self'` is spelled out explicitly for `graph-layout.js`'s dedicated
  Worker rather than relying on a `script-src` fallback.
  `frame-src https://www.youtube.com` is the one cross-origin iframe
  exception, for YouTube embeds.
- `systemd/wiki.service` runs with a long list of sandboxing options
  (`ProtectSystem=strict`, `NoNewPrivileges`, `ProtectHome`, `PrivateTmp`,
  a restricted `CapabilityBoundingSet`, and more). One option is
  deliberately *not* set: `SystemCallFilter=@system-service` crashed the
  real ARM/musl production target with a seccomp kill
  (`code=killed, status=31/SYS`) that x86_64/glibc testing never
  reproduced — the allowed syscall set under `@system-service` is itself
  architecture- and libc-dependent. `qemu-arm-static` can't catch this
  kind of regression either, since it emulates instructions, not
  kernel-level seccomp filtering.
- `wiki.env.example` / `EnvironmentFile=-/etc/opt/wiki/wiki.env` follows
  the FHS convention (`/opt/wiki` → `/etc/opt/wiki/`); the leading `-`
  keeps the unit startable when the file is absent. `AppConfig` doesn't
  read any config keys from the environment itself — the only consumers
  of environment variables are `CloudEmbeddingProvider`/`CloudChatClient`,
  which call `getenv()` on whatever `[embeddings].api_key_env` or
  `[llm].api_key_env` names (see [embeddings](embeddings.md),
  [llm](llm.md)). Admin credentials live in SQLite; sessions and
  remote-MCP tokens are random opaque values, not signed using anything
  from this file. A deployment using `provider = "none"` or `"local"`
  doesn't need this file at all.

### Error handling that avoids leaking paths

A path segment longer than the filesystem's `NAME_MAX` throws
`std::filesystem::filesystem_error`, and its `.what()` embeds the full
absolute on-disk path. Because of that, every mutating route's generic
`catch (const std::exception&)` has to come *after* an explicit
`catch (const std::filesystem::filesystem_error&)` that returns a clean
`400`/`"invalid path"` instead. This applies in `DocumentRoutes.cpp`,
`FolderRoutes.cpp`, `VersionRoutes.cpp`'s `document-restore` handler, and
`RemoteMcpRoutes.cpp`'s `create_document`/`update_document` — without the
specific catch, the generic one leaks the real vault path to the caller.
`RemoteMcpRoutes.cpp` still logs the full detail to `mcp_audit_log`
(admin-only) while returning only `"invalid path"` to the client.

### Testing

- `tests/integration/security_e2e.py` (run through `ctest`) boots a real
  server and drives it over actual HTTP: anonymous writes against every
  mutating route, CSRF, path traversal, session fixation (a session cookie
  chosen by an attacker before login is never adopted — the server always
  issues a fresh token on successful login), visibility gating checked
  across `/d/`, `/api/search`, `/api/nav/tree`, `/api/nav/tags`, and
  `/assets/` all at once, rate limiting, and a live `VaultWatcher` check.
- `tests/integration/stress_concurrency.py` / `stress_resources.py`
  (labeled `stress`, excluded by `ctest -LE stress`) separately cover
  concurrency races and resource-exhaustion behavior.
- `tests/fuzz/` (off by default, needs `WIKI_ENABLE_FUZZING` and Clang)
  carries coverage-guided libFuzzer harnesses for `PathGuard::resolve()`
  and `parseFrontMatter()` — the two parsers that handle raw, untrusted
  bytes directly.

### Other security notes

- Remote MCP's bearer-token comparison uses `auth::constantTimeEquals`
  (wrapping OpenSSL's `CRYPTO_memcmp`), not `==`. This check gates
  authentication itself for an anonymous caller on a public endpoint,
  unlike `CsrfFilter`'s comparison, which only guards an already
  authenticated session and doesn't need to be constant-time.
- No automated CVE scanner currently covers this project's vcpkg C/C++
  dependencies as a whole: neither OSV-Scanner nor Trivy has a vcpkg
  extractor, and GitHub's Dependency Graph doesn't list vcpkg among its
  supported ecosystems (only NuGet, for C/C++). A working manual
  spot-check is OSV.dev's commit-based query
  (`POST /v1/query {"commit": "<sha>"}`) against each dependency's exact
  pinned upstream commit — not something that can run as an automated CI
  scan, though. `overlay-ports/md4c/` carries a patch for `OSV-2022-126`
  (a heap-buffer-overflow read in `md_analyze_table_alignment()`, present
  in the pinned `release-0.5.3` and fixed upstream but not yet in a tagged
  release) — drop the overlay once `vcpkg.json`'s `builtin-baseline`
  advances past a release that includes the fix. GitHub's Dependabot
  alerts do cover `.github/workflows/ci.yml`'s GitHub Actions
  dependencies, which is a separate set from vcpkg's C/C++ packages.
- CodeQL's `paths`/`paths-ignore` settings have no effect once a compiled
  language is analyzed with real build steps (`build-mode: manual`) — this
  matches GitHub's own documentation, which says that setting only applies
  "when you analyze a compiled language without building the code." The
  CI workflow runs the dependency-installing CMake configure step *before*
  `codeql-action/init` turns on compiler tracing, so vcpkg's own
  from-source dependency builds happen outside the traced window and don't
  show up as alerts against vendored code.

## Content rendering pipeline

**Markdown engine.** This project uses **md4c**, not cmark-gfm — it's
lighter, plain C, and gets GFM extensions (tables, strikethrough,
tasklists) via parser flags. It runs with `MD_FLAG_NOHTMLBLOCKS`/`SPANS`,
meaning raw HTML passthrough is disabled entirely. That flag *is* the
sanitization — there's no separate sanitizing pass afterward.

**Front matter.** Parsed with **yaml-cpp**, not a hand-rolled parser.
Front matter gets hand-edited outside the app too, so real YAML edge
cases (the "Norway problem", arbitrary quoting, lists, dates) are better
handled by a battle-tested library than a custom one.

**`[[wiki-link]]` syntax.** A hand-rolled scanner (`util/WikiLinks.cpp`, no
`std::regex`) rewrites `[[wiki-link]]`/`[[target|label]]` into a plain
CommonMark `[label](d/target)` link *before* `renderMarkdownToHtml` ever
runs — md4c never sees the bracket syntax at all.
`normalizeTarget()` (trims the input, strips a leading `/`, appends `.md`
if missing) keeps the write side (`document_links`, via `IndexUpdater`) and
the read side (`NavQueries::backlinks`) in agreement about what a target
means. `target_path` is plain `TEXT`, not a foreign key, so a link to a
document that doesn't exist yet is recorded as a "red link" and starts
resolving automatically the moment a document lands at that path.

One subtlety: the generated href needs a `d/` prefix because
`shell.html` sets `<base href="{basePath}/">`, so a relative href resolves
against the mount root (route space, `/d/{path}`) — but
`normalizeTarget()`'s own output is in file space (the vault's actual
layout). The two happen to look identical for a simple target, but they
are not the same path space.

**YouTube embeds and Mermaid diagrams.** Both work around
`MD_FLAG_NOHTMLBLOCKS`/`SPANS` using the same marker-then-substitute
trick: a pre-pass turns a recognized construct into inert, ordinary
CommonMark — `![](youtube-embed:ID)` for a recognized YouTube URL (a
fenced ` ```mermaid ` block is already plain CommonMark and needs no
rewrite). md4c renders that as inert HTML, and only afterward does a
post-substitution step swap the resulting tag for a real
`<iframe>`/`<pre class="mermaid">`. The YouTube video ID is validated to
exactly 11 URL-safe characters at both ends of this round trip.
`rewriteYouTubeEmbeds` itself runs from inside `renderMarkdownToHtml`
(unlike `WikiLinks`, which the caller chains in beforehand) because
turning `<img>` into `<iframe>` requires touching md4c's own HTML output
directly.

A hand-typed `<img src="youtube-embed:ID">` in a document body can't forge
this substitution — literal HTML gets `&lt;`-escaped by md4c like any
other tag. A hand-typed *CommonMark* image (`![](youtube-embed:ID)`) is
different: it isn't raw HTML, so it does get substituted. That grants no
new capability today (the single admin who can write documents can already
embed any video via a real URL), but it does mean the substitution step is
reachable without going through `rewriteYouTubeEmbeds`'s own
URL-recognition logic first.

Mermaid rendering itself is lazy and entirely client-side
(`static/js/mermaid-render.js`): the ~5.3 MiB vendored `mermaid.min.js`
bundle is only fetched when a page actually contains a `pre.mermaid`
block, since the deployment target includes low-power SBC hardware over
possibly slow links. Diagram colors use `theme: "base"` with
`themeVariables` read live from the page's own CSS custom properties
(`--panel-bg`/`--fg`/`--fg-dim`), rather than a second hardcoded palette
that could drift out of sync with the real CSS. Because mermaid's theming
only accepts hex colors, a theme whose `--border` is an `rgba()` value
falls back to `--fg-dim` (which is hex in every theme) for diagram borders
and lines.

The editor's live preview plugs into Toast UI Editor's
`customHTMLRenderer.codeBlock` hook and reuses the same lazy-loader. That
hook's return value is only honored by the Markdown-mode Preview panel —
the WYSIWYG canvas always keeps a code block as its own editable widget.
Because the Preview panel is `display:none` whenever the Write tab is
active, and mermaid marks a block "already processed" after one render
attempt regardless of whether it succeeded, a diagram whose first render
happened while hidden would otherwise stay at zero height forever.
`refreshPreview()` checks the computed `display` value before rendering,
and a `MutationObserver` watches the Preview panel's own `display` toggle
— no public editor event fires for that specific tab switch.

**` ```query ` blocks.** These render as a live, re-queried-on-every-view
index or dashboard. `index::QueryBlocks::parseAndRun` never builds SQL from
the block's own text: the grammar is a fixed set of `key: value` lines
(`tag`, `type`, `folder`, `orphans`, `sort`, `order`, `limit`, `search`).
Each recognized key maps to one hardcoded SQL fragment, and a value only
ever reaches SQLite as a bound parameter, or as a lookup against a small
in-code whitelist substituted for a literal column name. There is no code
path where document text becomes part of the SQL string. Visibility
gating comes only from the *viewing* caller's own session — never from
what the block's author could see when they wrote it. An unknown key, a
duplicate key, or an out-of-range value is a `400` parse error, never a
silently empty or unfiltered result.

`search:` delegates to the same `FtsSearch&` instance `/api/search` uses
(including hybrid semantic search, when embeddings are configured) rather
than reimplementing ranking. Combining `search:` with
`sort`/`order`/`orphans` is a parse error, since relevance order and
database order don't compose.

One implementation detail worth knowing if you touch this code: every
parameterized condition is appended to one list, in the same order its
binder is pushed. This specifically avoids a SQLite footgun — a plain `?`
placeholder is assigned the next number after the largest explicit `?N`
already seen *in text-parse order*, not construction order. Placing an
explicitly-numbered placeholder ahead of a clause that has its own
anonymous placeholders would silently mis-bind values.

**Heading-level "zoom".** `static/js/section-zoom.js` hides and shows DOM
siblings around a clicked heading. It works on the rendered heading
structure, not the markdown source, since the content model is a flat
file with no individually addressable blocks. `h1` is excluded, since a
document conventionally opens with one that already represents the whole
document. Zoom state lives in a `#zoom=<slug>` hash set via
`history.replaceState`, rather than a plain hash assignment — that way the
Back button doesn't have to step through every individual zoom click.

## Attachments and versioning

**Attachments** live in a `<doc-name>.assets/` folder next to their
document. There's no extension allowlist — the real safety boundary is a
serving-side `isSafeToRenderInline` check. Filenames are sanitized to
`[A-Za-z0-9._-]`. There's no app-level size cap either (disk space is the
real bound); uploads are still limited by Drogon's
`setClientMaxBodySize` and whatever body-size limit the reverse proxy
enforces. `GET /assets/{path...}` derives the owning document from the
`.assets/` folder name, applying the same visibility gating as `/d/`,
rather than storing a separate visibility flag on the file itself.

**MCP large-file upload** avoids base64 entirely. The local stdio
transport uses `attach_file`'s `source_path` (a streamed `copy_file`); the
remote HTTP transport uses `attach_file_begin` (which issues a UUID ticket
over a normal Bearer-authenticated call), followed by
`PUT /mcp/uploads/{id}` carrying the raw bytes with **no**
`Authorization` header at all — the UUID itself is the secret for that one
call. `source_path` is rejected on the remote transport, since it would
otherwise let a caller read arbitrary files off the wiki host.

**Document history.** `document_snapshots` holds pre-edit content:
`DocumentService::update` snapshots a document's raw content before every
overwrite, while `create` snapshots nothing (there's no "before" state to
capture). `SnapshotStore::getContent` checks both `documentRowId` and
`snapshotId` together, never `snapshotId` alone — this closes off, by
construction, a whole class of bug where someone could guess another
document's snapshot id. Restoring an old version snapshots the
pre-restore state first, so restoring is exactly as undoable as any other
edit.

## Frontend

**Theme loading.** `shell.html` reads a saved theme choice from
`localStorage` via a synchronous inline `<script>` at the very top of
`<head>`, before any other stylesheet parses. It *creates* (rather than
retargets) a `<link id="theme-link">` pointing at the right
`css/themes/*.css` file. This matters because the app does a full page
reload on every navigation — a static `<link>` retargeted by a later
script would fetch the wrong theme first on *every single* reload, not
just the first load.

The three theme stylesheets (`classic`, `dark`, `green`) are each fully
self-sufficient files, duplicating the full selector list with different
values, rather than one shared base plus per-theme overrides. This is
deliberate: the project has no CSS build step or preprocessor, so three
complete files mean adding a fourth theme later is "write one new file,"
never "figure out which of several files a given rule lives in."
`[server].theme` sets the default for a browser with nothing saved yet
(injected into `shell.html` the same way `[server].base_path` is); a
reader's own saved choice always overrides it.

A real bug to remember: **a `<button>` styled only by a CSS class
selector needs that class actually present on the element.** The
theme-toggle button once had the `id` that `theme.js` needed, but not the
`class` every theme file's CSS actually targeted — so none of that
styling ever applied. The symptom (a styling change "doing nothing") looks
identical to a CSS specificity problem, which made it worth writing down.

**Editor theming.** Toast UI Editor's own `theme` option follows the
active site theme (read from `<html data-theme>` at editor-construction
time): `classic` maps to Toast UI's `"light"` (confirmed against the
vendored bundle itself to be its actual documented default — not assumed
just because `"dark"` sounds like the opposite), while `dark`/`green` map
to `"dark"`. No live re-theming logic is needed, since a theme change
already triggers a full page reload anyway.

**Stylesheet loading under a subpath.** Page-specific stylesheets
(`edit.css`, the vendored Toast UI Editor CSS) must be injected from a
script *after* `<base>` is set — exactly like theme CSS — never as a
static `<link href="css/...">` tag in the markup. The reason: the
browser's HTML preload scanner fetches stylesheets in parallel with the
first `<head>` script, resolving the URL against the *document* path, not
against a `<base>` a later script hasn't appended yet. Under a mounted
subpath (e.g. `/wiki/edit/{doc}`), a static tag would resolve to
`/wiki/edit/css/edit.css` — this app's own SPA shell, served as
`text/html` — which Chrome refuses to load as a stylesheet.

**Editor quirks worth knowing:**

- Inserting an attachment or link must go through
  `editor.exec("addLink"/"addImage")`, not `insertText("[name](url)")` —
  the latter is only correct in Markdown mode. In WYSIWYG mode it dumps
  the markdown syntax as literal text and auto-linkifies only the
  bracketed portion, leaving `](url)` visible on the page.
- Toast UI Editor's WYSIWYG markdown writer can't round-trip
  `[[wiki-link]]` syntax on its own — it escapes the brackets on output.
  The editor maps `[[path|label]]` to and from `[label](wiki:path)` around
  `setMarkdown`/`getMarkdown` (`static/js/common.js`); the `wiki:` URL
  never actually reaches disk.
- A free-text input with a suggestions dropdown (`createTypeahead`, used
  for the edit form's Type/Tags fields) is a different widget from the
  closed-set multiselect used for search filters, even though both read
  from the same nav endpoints — Type/Tags need to stay able to accept a
  genuinely new value. The dropdown renders as a portal appended to
  `document.body` with `position: fixed`, positioned from the input's own
  `getBoundingClientRect()`, rather than nested inside the form: Toast UI
  Editor nests several of its own positioned containers deep enough that a
  nested dropdown can't reliably win a z-index comparison against the
  editor's WYSIWYG body text.

**A routing gotcha.** Drogon's `registerHandlerViaRegex` matches regex
handlers in registration order — first match wins. A general
`"^/api/documents/(.*)$"` handler registered *before* a more specific
`"^/api/documents/(.*)/raw$"` one will greedily swallow the `/raw` suffix
into its own capture group. The more specific route has to be registered
first, or it's silently unreachable.

## Graph view

- `index::GraphQueries` backs `GET /api/graph` with the same
  fail-safe-private discipline as the rest of the app: an edge only
  appears when *both* endpoints are visible to the caller, so a private
  document can never leak its existence via an edge drawn from a public
  one. A "red link" (where the target doesn't currently exist) is
  silently excluded from the edge list, rather than drawn as a
  placeholder node.
- `GET /api/graph?around={path}` (the per-document "local graph") returns
  the whole connected component over visible edges — not a fixed 1-hop
  neighborhood — so a private document is never a stepping stone to a
  further public one for an anonymous caller. `around=` is treated as
  untrusted input: resolved through `PathGuard` and an exact bound-path
  lookup, never `LIKE`.
- Layout (`static/js/graph-layout.js`) is a from-scratch Barnes-Hut force
  simulation run in a dedicated Worker, not a vendored physics library.
  Repulsion is approximated via a quadtree above a size threshold, and
  falls back to plain pairwise repulsion for smaller graphs (the
  local-graph case). Initial node placement is a deterministic circle, not
  random, so the same graph settles into a recognizably similar layout
  across reloads.
- Rendering (`static/js/graph-render.js`) picks the first backend that
  actually works from a capability ladder — WebGL, then canvas 2D, then
  SVG — probed by actually trying to create a real context/shader, not
  just checking whether a constructor exists. SVG nodes are real
  `<a href="/d/...">` elements for ordinary navigation and keyboard
  access; canvas/WebGL hit-test in JS and navigate the same URLs, backed
  by a visually-hidden `<nav>` holding the same links.
- The full `/graph` page adds a toolbar once a vault is large enough for
  per-node labels to become unreadable: a text filter dims non-matching
  nodes without removing them from the layout (so typing doesn't restart
  the simulation); "hide unlinked" drops degree-0 nodes before layout;
  label visibility has `auto`/`On hover`/`All` modes. The single-component
  local-graph widget skips this toolbar entirely.

## MCP and LLM integration

Read, search, and write tools are available over stdio, and optionally
over a remote HTTP transport that's toggled on by an admin — see
[MCP integration](mcp.md) for the full tool list, the write-access gate,
and the remote transport's own auth model.

Optional cloud-assisted drafting (on the edit page) and vault Q&A (the
sidebar chat) are covered in [LLM integration](llm.md). Both of those
route through the same `QueryBlocks::parseAndRun`/
`CalendarQueries::eventsBetween` engines that the HTTP API and MCP tools
use, rather than each having its own separate implementation.

## Build

```sh
# one-time: clone + bootstrap vcpkg (not committed to git)
#
# Always a FULL clone, never --depth 1. vcpkg.json pins builtin-baseline to a
# specific commit; manifest mode resolves every dependency's version via
# `git show <that commit>:versions/baseline.json` against vcpkg's own repo
# history. A shallow clone only contains whatever commit happens to be
# upstream's current tip, which drifts away from an older pin over time with
# nothing about the clone command itself changing.
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

The package manager is vcpkg, in manifest mode. `vcpkg/` and
`vcpkg_installed/` are gitignored, never vendored. `overlay-ports/` carries
local patches for a pinned dependency version, ahead of its own upstream
fix landing in a tagged release (see the dependency-scanning notes under
"Auth and security" above).
