# Architecture

This file is a chronological log of the decisions actually made and each milestone's
spike/postmortem results — read the milestone sections below in order and they tell the
real build history, warts included, not a cleaned-up retrospective.

**Read this part first — it's the part that's still literally true.** The frontend
described in "Decisions made" and M2 below (Drogon CSP views + htmx) was the ORIGINAL
shape and is GONE. It was replaced by the mandate now in force project-wide: **no
server-side HTML/JS generation, ever — the C++ side is a pure JSON API, every page is a
static shell (`static/shell.html`) + client-side JS rendering from `fetch()` responses**
(see `CMakeLists.txt`'s own comment: "No CSP views (`views/*.csp`) anymore — the backend
is a JSON API only now"). `htmx` was vendored for the CSP-view era's server-rendered
fragments and removed once there was nothing left for it to swap — `static/js/htmx/`
does not exist in this repo. The CSP-view-specific gotchas below (M2) are kept as
engineering history — real lessons from real bugs — not as a description of the current
codebase; don't relearn them by touching a `.csp` file that also no longer exists.

## Decisions made (do not revisit without an explicit user request)

- **Storage**: markdown files on disk = source of truth. SQLite is only a secondary
  index (FTS5 + metadata), fully rebuildable via a full vault rescan.
- **MCP**: the service itself is an MCP server (stdio), not a client. Read/search only
  in the MVP (write access came later, see `docs/mcp.md`).
- **Auth**: a single admin, argon2id, SQLite-backed sessions. `visibility: public|private`
  in front-matter, defaults to `private` (fail-safe).
- **Frontend** *(original M0 decision — superseded, see the callout above)*: Drogon CSP
  views + htmx, Toast UI Editor scoped to the edit page only. No SPA build pipeline at
  runtime.
- **Deployment**: a bare binary + systemd. arm64 cross-compile — Phase 1.5/2; the MVP
  builds natively on the target Raspberry Pi.

## M0 — spike results

- **FTS5**: confirmed available (the system `sqlite3` CLI compiles and runs
  `CREATE VIRTUAL TABLE ... USING fts5(...)`). The vcpkg `sqlite3` port has the `fts5`
  feature explicitly requested (`vcpkg.json`) — it's not a default feature, without this
  it wouldn't be in the built library.
- **Markdown rendering**: chose **md4c** over `cmark-gfm`. Both ports exist in vcpkg;
  md4c is plain C, lighter, and GFM extensions (tables/strikethrough/tasklists) are
  enabled directly via parser flags, with no separate GFM fork of the library needed.
- **Front-matter YAML**: chose **yaml-cpp**, not a hand-rolled parser. Front-matter gets
  edited by the user directly in a text editor (outside the Web UI, synced back via
  `VaultWatcher`) — arbitrary quoting/lists/dates need to parse correctly; the
  "Norway problem" (`no`/`yes`/`on`/`off` as booleans) and other YAML pitfalls are
  better delegated to a battle-tested library than reinvented.
- **Package manager**: vcpkg, manifest mode, `builtin-baseline` pinned to a specific
  vcpkg commit (see `vcpkg.json`) for reproducible builds on another machine/RPi.
  vcpkg is **not vendored in git** (`vcpkg/` is in `.gitignore`) — it's cloned via a
  bootstrap step.

## M1 — auth, PathGuard-backed read: results

- **auth/ and controllers/ are NOT part of libwikicore.** vault/index/config/util stay
  in wikicore (needed by both wiki-server and the future wiki-mcp); auth/
  (sessions, argon2, CSRF, Drogon filters) and controllers/ are purely a web concern,
  linked only into wiki-server. MCP tools are read-only and don't need sessions.
- **Drogon HttpFilter — a naming trap.** `registerHandler(..., {Get, "AuthFilter"})`
  looks the filter up in `DrClassMap` by its **fully-qualified, demangled** type name
  (`__cxa_demangle(typeid(T).name())`), i.e. `"wikicore::auth::AuthFilter"`, not the
  bare `"AuthFilter"` — get it wrong and it fails silently at runtime with
  `middleware X not found` (a log line, not a crash, so it's easy to miss). Second
  gotcha: `DrObject<T>::alloc_` is a static member of a class template — the compiler
  only instantiates it (and thus registers the class) if it's actually ODR-used; no
  filter is ever constructed or referenced directly anywhere else, so without an
  explicit forced call to `AuthFilter::classTypeName()`/`CsrfFilter::classTypeName()`
  in `main()`, the registrar never gets linked in at all. Both gotchas are documented
  right in the code (`main.cpp`, the comment right before route registration) — don't
  remove that call, it's not "dead code".
- **CSRF token: two-cookie delivery.** The synchronizer token is stored server-side in
  `sessions.csrf_token`; it reaches the client via a separate, NOT-HttpOnly cookie
  (`wiki_csrf_token`), set alongside the session cookie at login. `CsrfFilter` checks
  the header/form field against the server-side value — the cookie is only the delivery
  channel, never the source of truth.
- **Fail-safe-private confirmed by tests and E2E**: missing/broken YAML front-matter, a
  missing `visibility` field, any value other than exactly `"public"` — all of it
  collapses to `private`. A private document returns `404` to an anonymous request, not
  `403` (don't reveal existence).
- **`--create-admin` CLI**: writes/overwrites the single admin row (`users`, `id=1`
  enforced via a CHECK constraint), password entered with terminal echo disabled
  (termios), never logged.

## M2 — CRUD, WYSIWYG, CSP views: results

> **CSP views (`.csp` files, `views/`) don't exist in this codebase anymore** — see the
> callout at the top of this file. Everything below in this section is a historical
> record of real bugs caught at the time, kept for the lessons (a fixed-key-lookup
> templating footgun is a footgun in any templating engine), not as current-state
> documentation. Nothing here should be "fixed" against today's code.

- **Drogon CSP `[[key]]` does NOT escape HTML.** Verified directly in the generated
  code (`drogon_ctl create view`): `[[key]]` compiles literally to
  `stream << *any_cast<std::string>(&viewData["key"])` — no escaping whatsoever. Every
  untrusted value (title, tags, anything from a document) MUST go through
  `util::escapeHtml` before `HttpViewData::insert`. Second gotcha: `[[key]]` is a
  fixed key lookup into the view's data, NOT a reference to a C++ variable of the same
  name from a `for` loop inside a code block; to output a value computed in a loop/
  condition, write straight to the output stream inside the code block instead of
  `[[key]]`. Third: `[[key]]` only renders values inserted as `std::string`/
  `const char*` — any other type silently produces empty output.
- **The CSP tag parser has no escape mechanism.** If a comment inside a `.csp` file
  mentions the tag syntax itself (`<%c++`/`%>`/`[[`/`]]`) in prose, the parser treats it
  as a real tag and breaks the file in half — caught on my own House Rules comment in
  `EditPage.csp` (fragments of the comment leaked straight into the page's HTML
  output). Takeaway: keep detailed explanations of CSP syntax out of `.csp` files
  (here, or in CMakeLists.txt) — inside a `.csp` file, only a short pointer.
- **`newHttpViewResponse("EditPage", ...)` hits the same DrClassMap trap** as
  HttpFilter: `DrTemplate<T>` also inherits `DrObject<T>`, so registration requires
  (a) `EditPage::classTypeName()` being ODR-used somewhere (a forced call in
  `main.cpp`) and (b) a name with no C++ namespace — `.csp` files are generated here
  without the `-n`/path-to-namespace flag specifically so the string `"EditPage"`
  matches whatever the class is actually registered under.
- **`documentRoot` = `static/`, NEVER the repo root** — otherwise Drogon's static file
  server would expose `config.toml`, source, and everything else under the git root
  over HTTP. Static URLs carry no `/static` prefix (e.g. `/js/edit.js`), because
  `documentRoot` already points straight at `static/`.
- **Toast UI Editor is vendored and COMMITTED** (`static/js/toastui-editor/`, with
  `SHA256SUMS`+`VENDORED.md` for provenance) — unlike `vcpkg/`, this isn't rebuilt every
  time; a checkout needs to build and deploy with no network access beyond vcpkg.
- **Atomic document write**: a temp file (`<name>.tmp-<uuid>`) in the same directory +
  `rename()` (atomic within one filesystem). Soft-delete: `rename()` the document and,
  if present, its `<stem>.assets/` folder into `.trash/<same relative path>` —
  separately, returning an error if the document moved but the assets folder didn't
  (doesn't try to roll the document itself back).
- **Attachments**: an extension allowlist (not a blocklist), filename sanitization
  (`[A-Za-z0-9._-]`, everything else → `_`), a 25 MiB cap, de-duped via a short UUID
  suffix on a name collision. `GET /assets/{path...}` resolves visibility through the
  OWNING document (`<stem>.assets/` → `<stem>.md`), not via a separate flag on the file
  itself.

## M2 postmortem: unauthenticated mutating routes

Found by my own E2E script, before the commit, not after: `POST /api/documents` with no
session at all returned **201 Created** — the document was genuinely created. Root
cause: `AuthFilter` only writes `kAttrUserId` into request attributes (blocks nothing),
and `CsrfFilter` DELIBERATELY passes a request with no session straight through
(nothing to check against), leaving the authorization check to the handler itself —
exactly as documented in `CsrfFilter.h`. For `GET /d/`/`GET /edit/` I did write that
check; for create/update/delete/upload I simply forgot, even though a comment in my own
code explicitly warned about it. Fix: `requireAdminApi()` — an explicit call as the
first line in EVERY mutating handler in `DocumentRoutes.cpp`. Lesson: `AuthFilter`/
`CsrfFilter` being listed in `{Post, "...AuthFilter", "...CsrfFilter"}` is NOT proof the
route is protected — both filters in this architecture deliberately don't block,
they only annotate/check a token. Verify by reading the code of every new mutating
handler, don't rely on "the filter's already attached".

## M3 — FTS5 search + navigation: results

- **FTS5 `snippet()` has the same hazard as CSP `[[key]]`, mirrored.** `snippet()`
  pulls raw text straight from `documents_fts` (unfiltered document markdown) and
  inserts its own markers around matches. Insert literal `<mark>`/`</mark>` as the
  markers and you can't escape the result afterward (breaks the tags); don't escape it
  and the document's own content becomes an XSS vector. Fix: the markers are control
  bytes `\x01`/`\x02`, passed as bind parameters to
  `snippet(documents_fts, 1, ?, ?, '...', 12)`, never literals in the SQL text; at
  render time, `escapeHtml()` the whole snippet FIRST, and only THEN swap
  `\x01`→`<mark>`, `\x02`→`</mark>`. The order of operations here is the entire point
  of the protection.
- **The rescan is unconditional on every startup** (`IndexBuilder::fullRescan()` in
  `main.cpp`, before route registration), not just on request. The DB is a disposable
  cache never trusted on faith: if a file was added/changed directly on disk (external
  editor, `git pull`), the startup rescan picks it up with no manual step. Verified
  E2E: a file dropped straight into the vault outside the app is unsearchable until a
  rescan — and disappears from the index (stale sweep) right after the file is deleted
  directly plus another rescan.
- **Nav/tag queries are visibility-aware just as strictly as everything else**: a
  private document contributes NEITHER a path to the tree NOR even +1 to a tag's
  count for an anonymous request — verified E2E (a tag used only by private documents
  gives a different count for admin vs. anon).
- **htmx was also vendored and committed at this point** (`static/js/htmx/`, the same
  `tools/build-editor-bundle/fetch.sh` vendored both bundles) — since removed, see the
  callout at the top of this file; it had nothing left to swap once the frontend moved
  off server-rendered fragments.

## M4 — MCP server: results

- **The hkr04/cpp-mcp spike succeeded on the first try.** Cloned, built in isolation
  (`cmake --build --target mcp`) against this environment's exact GCC 16.2.1/C++20 —
  compiles clean, no fallback to a hand-rolled JSON-RPC was needed (it was in the plan
  as a backup option). The library pulls `mcp_sse_client.cpp`/`httplib.h` into one
  inseparable package together with the stdio part (no CMake flag to split out just
  stdio) — without `MCP_SSL` this doesn't pull in OpenSSL, acceptable.
- **`FetchContent`, not vcpkg**: no port exists for cpp-mcp. Pinned to a specific
  commit. Its own CMakeLists.txt builds `examples/` unconditionally (no flag to turn
  it off) — accepted as a few extra seconds of build time rather than fighting someone
  else's build system to save time. The `mcp` target exposes include paths via
  **directory-scoped** `include_directories()`, not `target_include_directories()` —
  so they do NOT propagate to `wiki-mcp` automatically; had to add them manually
  (`${cpp_mcp_SOURCE_DIR}/include`, `.../common`).
- **`mcp::json` is a SEPARATE copy of nlohmann::json** (vendored inside cpp-mcp as
  `common/json.hpp`, an alias for `nlohmann::ordered_json`), not the same library the
  rest of the project gets from vcpkg. `McpServer.cpp` deliberately never includes the
  vcpkg `<nlohmann/json.hpp>` — only `mcp::json` everywhere, to avoid risking an ODR
  conflict between two copies of the same header in one compilation unit.
- **A JSON-RPC "notification" still prints a response line** (`{}`) in this library,
  even though `notifications/initialized` has no `id` and, per spec, needs no response.
  A client has to filter incoming lines by the expected `id`, not naively read "the
  next line" — caught by my own E2E script (`tools/list` initially returned an empty
  array because the test consumed the wrong line).
- **MCP visibility gets the same fail-safe-private discipline, duplicated at the
  tool-handler level**, not just through `includePrivate` leaking from config:
  `get_document` checks the resolved document's own `visibility` EVEN IF
  `id_or_path` resolved successfully — the same class of bug as M2 (a check skipped
  at the handler level), caught here ahead of time instead of after the fact.
- **`wiki-mcp` does NOT rescan the vault at startup** (unlike `wiki-server`) — an MCP
  client may spawn the process often, a full walk on every spawn would contradict the
  "starts instantly" requirement. It trusts the existing index; `wiki-server --reindex`
  is a separate, explicit step.
- **Verified with a scripted JSON-RPC client, not a live Claude Desktop** (a headless
  dev environment, no GUI) — 17/17 checks across two runs (`scope=admin` and
  `scope=public`) against the same index, including a private document's visibility
  actually flipping depending on scope. Details and a sample
  `claude_desktop_config.json` — `docs/mcp.md`.

## M5 — hardening & deployment: results

- **A real race condition in `VaultWatcher::start()`, caught by its own test, not by
  theory.** `start()` spawns a thread and returns immediately; the actual
  `inotify_add_watch()` registration on the root happens asynchronously inside the new
  thread. The "file in a just-created subdirectory" test failed reliably 3/3 times — a
  disk change made in the narrow window between `start()` and the initial tree walk
  finishing was lost FOREVER (the kernel can't queue an event for a watch that doesn't
  exist yet). The same class of window also existed in `main.cpp` between
  `vaultWatcher.start()` and `drogon::app().run()`. Fix: `start()` now blocks on a
  `std::promise`/`std::future` until the initial recursive walk finishes — "a change
  right after `start()` returns will be seen" is now part of the contract, not a
  timing accident.
- **`VaultWatcher` gets its OWN sqlite3 connection**, not the one Drogon's request
  threads share. `IndexUpdater::upsertOne`/`removeOne` wrap
  `BEGIN IMMEDIATE...COMMIT`; two threads trying to start a transaction on the SAME
  `sqlite3*` handle at the same time is "cannot start a transaction within a
  transaction", not safe serialization. WAL mode (already enabled in
  `Database::Database`) exists exactly for this: separate CONNECTIONS to the same file
  coordinate correctly at the file level, unlike one CONNECTION shared by several
  callers unaware of each other.
- **`wiki.env.example`/`EnvironmentFile` in the systemd unit — a stale artifact from
  M0**, caught while cross-checking against the actual code: no environment variable is
  actually read anywhere (admin credentials live in SQLite, sessions are random tokens
  with no secret-based signature). Without a leading `-` in front of
  `EnvironmentFile=`, systemd would refuse to start the unit if the file is missing —
  over a file the app doesn't need. Fixed:
  `EnvironmentFile=-/etc/wiki/wiki.env` (optional), the file stays as a documented hook
  for a future real secret (Phase 2, a remote MCP bearer token), not a silent promise
  the code doesn't keep.
- **`tests/integration/security_e2e.py` closes a gap I'd documented myself, four
  milestones running.** A consolidated, committed pass, wired through `ctest` (not a
  one-off bash session in a terminal): anonymous writes against every mutating route,
  CSRF, path traversal, **session fixation** (an attacker-chosen cookie token set
  BEFORE login — the server issues a fresh token after authentication, the old one is
  never accepted as authorized), visibility gating checked simultaneously across
  `/d/`, `/api/search`, `/api/nav/tree`, `/api/nav/tags`, `/assets/`, rate limiting, and
  a live `VaultWatcher` check (a file written directly to disk → becomes searchable
  with no `--reindex`, disappears on delete). 32/32 in a green run.
- **`cmake --install` verified live**: a real install tree (`--prefix /tmp/...`), a
  real run of the installed binary with `config.toml` copied from the example,
  `/healthz` and a static asset (htmx from `static/js/htmx/` at the time — since
  removed, the same check today would use e.g. `static/js/toastui-editor/`) both
  returned `200`.
- **Raspberry Pi — cross-compiled binary now verified live**, in a later session past
  M5 (this note was stale — said "NOT verified" while the actual armv7/musl binary had
  already been cross-built, `qemu-arm-static`-checked, and was running natively on real
  hardware as the actual production instance every later milestone's live testing
  happened against). **Native compilation directly ON the device is still NOT done** —
  see `docs/deployment.md`'s "Real-hardware verification status" for the precise,
  still-current distinction between the two (don't conflate them back together just
  because this line got fixed).

## Later additions past M5

Everything below landed after the M0–M5 milestone plan was complete, in later
sessions — `docs/mcp.md` and `docs/deployment.md` got updated incrementally as each one
shipped, this file didn't, until now. Same reasoning-first style as M0–M5: what was
decided, why, and what broke along the way that a plan wouldn't have predicted.

### Cmd-K quick-open

A global `Ctrl/Cmd+K` overlay (`static/js/quick-open.js`), filtering the same flat
`/api/nav/tree` list the sidebar's document tree already builds from — fetched once,
lazily, cached for the page's lifetime, not re-fetched per keystroke. The one thing
worth writing down: it explicitly steps aside whenever focus is already inside Toast UI
Editor's own contenteditable surface (WYSIWYG or Markdown mode), rather than assuming
no conflict. `Ctrl/Cmd+K` is a common "insert link" binding in that class of editor —
untested assumption either way risked either eating the editor's own shortcut or
double-handling the keypress, so the fix is to just not intercept at all near the
editor, full stop, rather than try to detect and coordinate the two.

### `[[wiki-links]]` + backlinks

Obsidian/MediaWiki-style `[[target]]` / `[[target|label]]` syntax, layered on top of
md4c rather than as a parser extension — a hand-rolled scanner
(`util/WikiLinks.cpp`, deliberately no `std::regex`, matching every other string-parsing
file in this codebase) rewrites it to a plain CommonMark `[label](d/target)` link BEFORE
`renderMarkdownToHtml` ever runs; md4c has no idea the syntax exists. Targets are
normalized once (`normalizeTarget`: trim, strip a leading `/`, append `.md` if missing)
so `[[notes/foo]]` and `[[notes/foo.md]]` mean the same document on both the write side
(`document_links` table, populated via `IndexUpdater`) and the read side
(`NavQueries::backlinks`) — no further munging needed at either end for them to compare
equal. `target_path` in `document_links` is plain TEXT, not a foreign key: a link to a
document that doesn't exist YET is still recorded as a "red link", and starts resolving
correctly the moment a document actually lands at that path, with no re-edit of the
document that linked to it required.

**A real, shipped bug, caught by a user clicking a real link, not by any test**: the
generated href used to be the bare `target` with no `d/` prefix. `shell.html` sets
`<base href="{basePath}/">`, so every relative href on the page resolves against the
MOUNT ROOT — but `normalizeTarget()`'s output is a path in FILE space (matching the
vault's own directory layout), while viewing a document is a ROUTE at `/d/{path}`, a
different path space that happens to look identical for a simple target. Every existing
test for `rewriteWikiLinksToMarkdownLinks` asserted the bare, broken shape as "correct"
— written by copying what the code produced at the time, not by independently deriving
what the href actually needed to resolve to. The M2 postmortem above is about a
different bug entirely (an unauthenticated route, not a broken link), but the same
underlying lesson applies here too: a test (or a filter being listed on a route) that
merely reflects what the code already does proves nothing about whether the code is
actually right. Fixed by prefixing with `d/`; the reverse relationship (backlinks) was
never affected — `extractWikiLinkTargets`/`document_links` don't build hrefs at all, so
a page's own "Linked from" section stayed correct the entire time this was broken,
which is exactly how the asymmetry got noticed (one direction worked, the other 404'd).

### Document versioning (snapshot / diff / restore)

`document_snapshots` was in the schema from the very start (per the original plan) but
unused until this. `DocumentService::update` snapshots a document's PRE-edit raw
content before every overwrite — `create` snapshots nothing (there's no "before" state
for a brand-new document). `SnapshotStore::getContent` deliberately checks BOTH
`documentRowId` and `snapshotId` together, never `snapshotId` alone — an IDOR-shaped bug
class (fetch someone else's/some other document's snapshot by guessing an id) guarded
against by construction, not by a permissions check bolted on after the fact. Restoring
a version itself snapshots the pre-restore state first, so a restore is exactly as
undoable as any other edit — there's no special "point of no return" version of a
write anywhere in this feature. The diff view (`static/js/diff.js`) is a small
hand-rolled LCS line diff, not a vendored library — the app already avoids adding a
frontend dependency for something this contained.

### MCP write access (Phase 2, local stdio — `create_document`/`update_document`)

Gated behind `[mcp].write_access` in `config.toml`, default **off** — a client gaining
write access to the vault is a conscious opt-in on rebuild/reconfigure, never a silent
capability bump. Every call, success or failure, is recorded in `mcp_audit_log`
(`index::McpAuditLog`) regardless of this setting's own history, specifically so "was
anything ever written by an MCP client, and did it succeed" stays answerable after the
fact even for a deployment that's since turned write access back off. `get_document`'s
own visibility check is re-verified at the handler level even though `includePrivate`
already gated the call that resolved the document — the identical discipline M2's
postmortem (above) established for the HTTP layer, applied here before an equivalent
bug could happen a second time in a different transport, not after.

### Remote MCP transport (HTTP)

A SEPARATE admin-toggleable `POST /mcp` route (`RemoteMcpRoutes.cpp`) letting an MCP
client reach the same tools over HTTPS instead of only a local stdio spawn — independent
of the stdio server above; turning one on touches nothing about the other.

- **Deliberately hand-built rather than using vendored cpp-mcp's own HTTP+SSE server.**
  Reading `mcp_server.cpp` directly turned up that its `set_auth_handler()` is set but
  never actually invoked anywhere in that library's request path — an unpatched,
  dead-code auth hook. Trusting it for a public endpoint would have shipped something
  that *looks* token-protected and isn't; this route reuses only the underlying
  `wikicore` services the stdio server also calls, gated by this app's own real, tested
  machinery instead (bearer token, hash-only in the DB — same discipline as session
  cookies — shown raw exactly once at generation; a dedicated `RateLimiter` instance,
  never sharing state/lockout budget with `/login`'s own; an optional CIDR allowlist,
  empty meaning no restriction — the token is the actual gate, the allowlist an
  optional extra layer on top).
- **A real, self-caught bug: `auth::ClientIp` originally trusted `X-Forwarded-For`'s
  FIRST entry.** Cross-referencing the actual deployed nginx config
  (`proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;` — APPENDS, never
  overwrites) showed the first entry is exactly what a client can freely spoof
  (`curl -H "X-Forwarded-For: <an-allowlisted-ip>"` would walk straight past an
  allowlist checking that entry); the LAST entry is always nginx's own append, exactly
  as trustworthy as `X-Real-IP` (which `proxy_set_header` always overwrites, never
  appends to). Fixed to prefer `X-Real-IP`, fall back to XFF's LAST entry — verified
  live against production with a direct spoofing simulation (a crafted header with an
  attacker-claimed allowlisted IP first, the real blocked IP appended after it,
  matching `$proxy_add_x_forwarded_for`'s exact shape) still correctly blocked.
- **A `/-1` CIDR prefix bug caught before shipping** (`auth/CidrMatch.cpp`): a literal
  `/-1` parsed successfully via `std::stoi` and was silently reinterpreted as "no prefix
  given" (→ `/32`, an exact match) because both cases shared the same sentinel value.
  Fixed by explicitly rejecting a negative parsed prefix rather than only checking the
  upper bound.

### Vault backup (Web UI button + opt-in systemd timer)

`vault::createVaultBackup` (`src/vault/BackupService.cpp`) shells out to the system
`tar` via `fork()`+`execlp()`, deliberately never `system()`/`popen()` — both of those
run the command through `/bin/sh -c "..."`, turning any shell metacharacter the vault
path happens to contain into a parsing hazard; `execlp()` takes an explicit argv with no
shell involved at all, so the path's actual content is inert regardless of what's in
it. Excludes `.uploads-tmp/` (Drogon's own transient multipart-upload staging buffer —
256 pre-created sharded subdirectories, never real content; confirmed by testing a real
backup and finding them cluttering the archive before adding the exclusion). The
opt-in `systemd` timer path (`systemd/wiki-backup.sh`) deliberately reimplements the
same tar invocation standalone rather than curling the admin HTTP endpoint above — a
disaster-recovery backup that only works while `wiki-server` happens to be up and an
admin session happens to exist defeats the entire point of one; it needs to keep
working whether the server is healthy, crashed, or mid-restart.

### `![youtube](url)` embeds

`md4c` runs with raw HTML passthrough deliberately disabled (`MD_FLAG_NOHTMLBLOCKS`/
`SPANS` — `MarkdownRenderer.cpp`'s own comment: that flag IS the sanitization, there's
no separate pass in front of it), so there's no way to
hand it a real `<iframe>` directly. Uses the exact same marker-then-substitute shape as
FTS5's `snippet()` (M3, above), for the identical reason: `rewriteYouTubeEmbeds`
(`util/YouTubeEmbed.h`, a markdown-level pre-pass) turns a recognized URL
(`youtube.com/watch?v=`, `youtu.be/`, `/shorts/`, `/embed/` — `v=` found anywhere in the
query string, not just first or alone) into `![](youtube-embed:ID)`, ordinary
CommonMark image syntax md4c renders as an inert `<img src="youtube-embed:ID">`; only
AFTER that HTML exists does `substituteYouTubeEmbeds` swap that exact tag for a real,
narrowly-templated iframe. `ID` is validated to exactly 11 URL-safe characters at BOTH
ends of this round trip, not once — the same double-check discipline as every other
trust boundary in this codebase. A hand-typed `<img src="youtube-embed:...">` in a
document body can't forge this: raw HTML text gets `&lt;`-escaped by md4c same as any
other literal tag (confirmed directly against a compiled md4c test, not assumed), so the
marker only ever appears unescaped when it came from the pre-pass itself. Unlike
`[[wiki-links]]` above (an external pre-pass the CALLER chains in before
`renderMarkdownToHtml`), this pre-pass is invoked FROM `renderMarkdownToHtml` itself —
turning `<img>` into `<iframe>` requires touching md4c's own HTML output, which only
`MarkdownRenderer.cpp` ever sees, so splitting the feature's two halves across two call
sites would only invite them drifting out of sync.

### Docker (multi-stage build)

Not a replacement for the native/cross-compile deployment path — Docker itself needs a
kernel/glibc too modern for the actual verified-on-real-hardware target (Debian 9
stretch); this is for trying the app on a normal x86_64/arm64 machine. `vcpkg.json` is
copied and installed BEFORE the rest of the source specifically so Docker's layer cache
survives ordinary source edits — only touching `vcpkg.json` invalidates the expensive
Drogon+OpenSSL+trantor-from-scratch layer. Runs as a fixed `uid:gid 1000:1000` rather
than whatever `useradd --system` would assign on its own, because `/data` is meant to be
a HOST bind mount and a bind mount's write permission is checked by raw uid — a username
doesn't cross that boundary at all.

**A real, currently-live bug caught while building this, unrelated to Docker
specifically**: `git clone --depth 1` for vcpkg — used in EVERY documented build path in
this repo at the time (README, this file, `docs/deployment.md`,
`docs/sbc-deployment.md`) — can silently fail to contain `vcpkg.json`'s own pinned
`builtin-baseline` commit. Manifest mode resolves every dependency's version via
`git show <that commit>:versions/baseline.json` against vcpkg's OWN history; a shallow
clone only has whatever commit happens to be vcpkg's current upstream tip, which drifts
away from an old pin over time with nothing about the clone command itself changing.
Confirmed live: a fresh `git clone --depth 1` of vcpkg on the day this was caught did
NOT contain this project's pinned baseline commit; a full clone did. This wasn't
Docker-specific at all — every documented build path had it, invisible locally only
because this dev machine's own `vcpkg/` checkout predates when it broke. Fixed
everywhere at once, `git clone` with no `--depth` flag from here on.

### Sidebar tag namespace grouping + filter

Once the vault had accumulated enough tags for the sidebar's flat `<ul>` of every tag
in use to become genuinely unwieldy on screen — the same problem the document tree
already had, and already had a fix for.

- **Reused the document tree's own grouping convention rather than inventing a second
  one.** `nav.js::buildTree` already splits a document's `path` on `/` into a
  collapsible folder tree; `buildTags` now does the identical thing to a tag STRING —
  `lang/cpp`, `lang/python`, `project/wiki` group into a `lang/`/`project/` tree,
  collapsed by default, same `.nav-arrow`/`.nav-children`/`collapsed` CSS classes, same
  shape entirely. A tag with no `/` stays a flat leaf, exactly as before. This is purely
  a client-side presentation convention — `/api/nav/tags` (`NavQueries::tagCounts`)
  still returns the same flat `{tag, count}` list it always has; the server has no idea
  any grouping happens (see `docs/mcp.md`'s own note on this, since an MCP client
  choosing tag values for `create_document`/`update_document` benefits from knowing the
  convention exists even though nothing server-side enforces or requires it).
- **Expand state gets its OWN localStorage key** (`wiki.expandedTagGroups`), deliberately
  separate from the document tree's `wiki.expandedFolders` — expanding `notes/` as a
  folder and `notes/` as a tag-namespace prefix are unrelated pieces of state; sharing
  one key would have been an accidental coupling, not a feature.
- **A real `<button>` for the group toggle, not a reuse of `.nav-folder-btn`** — that
  class is a `<span>` flex-wrapper around a SEPARATE arrow-`<button>` + label-`<a>` pair
  in the document tree (the label navigates to `/folder/{path}`; the arrow only
  toggles). A tag namespace group has no `/folder/{path}`-equivalent page to send a
  label click to, so there's no second action competing for the click — the whole row
  toggles, which is also just a bigger, easier target than the narrow arrow glyph
  alone. Caught by rendering it and looking, not by assuming a class named
  `.nav-folder-btn` would just work on a real `<button>` the way it does on a `<span>`.
- **Filter-as-you-type rebuilds the whole tree from scratch on every keystroke** — the
  same choice `quick-open.js` already made for its own filtered list, at a comparable
  scale, no debounce either place. A match forces every ancestor group open regardless
  of its persisted collapse state, and clearing the filter reverts to exactly that
  persisted state — the transient, search-driven expansion is never itself written to
  `localStorage`, confirmed live (expand `lang/`, filter for something else, clear the
  filter, `lang/` is still the only one open).

### Visual theme picker (classic / dark / green)

The single hardcoded green-on-black terminal look (`theme.css`) became three
swappable, independent stylesheets, picked from a small icon in the sidebar.

- **Three fully self-sufficient CSS files, not one file plus shared `:root`
  overrides.** `theme.css` was renamed to `css/themes/green.css` more or less
  unchanged; `dark.css` (a plain neutral dark UI) and `classic.css` (white
  background, MediaWiki-style blue links, serif headings) are new files that
  duplicate green.css's full selector list with different values, rather than
  factoring colors out into a shared base stylesheet. Deliberate: this project
  has no build step and no CSS preprocessor, so "shared base + three small
  diffs" would mean understanding two files to know what a given selector
  actually renders as; three complete files mean a fourth theme later is
  "write one new file," never "figure out which of several files a rule
  lives in." The genuinely theme-SPECIFIC pieces (green's Orbitron import,
  neon glow via `text-shadow`, shouting-caps buttons; the `#matrix-bg`
  digital-rain canvas) simply don't exist in `dark.css`/`classic.css` at all,
  rather than being toggled off by a variable.
- **Applied via a synchronously-injected `<link>`, not a static one with a
  later `href` swap.** `shell.html`'s bootstrap script (the very first thing
  in `<head>`, run before the base_path-inference script even finishes)
  reads `localStorage['wiki.theme']`, sets `<html data-theme="...">`, and
  `document.head.appendChild()`s a fresh `<link id="theme-link" href="css/
  themes/<theme>.css">` right there. A static `<link href="css/themes/
  green.css">` retargeted afterward by a second script would start fetching
  (and briefly render under) the wrong theme first — this app already does a
  full page reload on every single navigation (see `router.js`'s own
  no-History-API rationale), so that flash would happen on EVERY page view,
  not just first load.
- **`matrix.js` reacts to computed visibility, not a theme name.** Rather
  than teaching the canvas script about three theme names (two of which
  don't want it at all), `dark.css`/`classic.css` each just set
  `#matrix-bg { display: none; }`, and `matrix.js` bails out via
  `getComputedStyle(canvas).display === "none"` — the exact same guard shape
  it already used for `prefers-reduced-motion`. No JS-side coordination
  between the theme files and the canvas script is needed at all.
- **Theme changes reload the page rather than live-swapping anything.**
  `theme.js` writes the choice to `localStorage` and calls `location.reload()`
  — consistent with this app's existing full-reload-per-navigation
  architecture, and it sidesteps having to tear down/reinit `matrix.js`'s
  running `setInterval` or reconcile any other page state that assumed one
  theme was active.
- **Toast UI Editor's own dark-mode CSS gap-fix moved to `edit.css`, out of
  the theme files entirely.** At the time this fix was written, `pages/
  edit.js` hardcoded the editor's own `theme: "dark"` regardless of which
  SITE theme was active, so it applied identically no matter what — this
  was later corrected (see the "Editor theme follows the site theme"
  entry below) to actually vary per site theme, but the fix's own CSS
  selectors were already scoped under `.toastui-editor-dark`, so they stay
  correctly inert whenever the editor picks its light mode; only the
  comment explaining WHY the fix lives in `edit.css` needed updating once
  the hardcoding it originally described stopped being true. It used to
  live in (the file now called) `green.css` purely because that was the
  only theme file that existed yet; duplicating it into `dark.css`/
  `classic.css` too would have been the same fix copy-pasted for no
  reason.

### Editor theme follows the site theme

Found live, right after shipping the theme picker above: switching to `classic`
(a light theme) left the Toast UI Editor rendering as a solid black WYSIWYG
panel in the middle of an otherwise white page — `pages/edit.js` had hardcoded
the editor's own `theme` option to `"dark"`, harmless while green.css (also
dark) was the only site theme, wrong the moment a light one existed.

- **Reads `<html data-theme="...">` at editor-construction time**, set
  synchronously by `shell.html`'s own bootstrap script before `pages/edit.js`
  ever runs — no new plumbing needed, the signal already existed for exactly
  this kind of theme-aware decision. `classic` → Toast UI's own `"light"`
  theme; `green`/`dark` → `"dark"` (both have dark page backgrounds).
- **`"light"`, not `"default"`** — confirmed by grepping the vendored
  `toastui-editor.min.js` itself for its own built-in default value, rather
  than guessing from the "dark" counterpart's name. Passing an unsupported
  string would likely have been harmless in practice (it only ever changes
  a `toastui-editor-<value>` class name, and no CSS targets a `-default`
  variant), but there was no reason to rely on that rather than the value
  the library actually documents.
- **No live re-theming while the editor is open** — a theme change already
  triggers a full page reload (`theme.js`), same as every other navigation
  in this app, so the editor is always freshly constructed with the
  now-current theme rather than needing to react to a change mid-session.
- Verified live against the real production instance via an already-
  authenticated admin session (no credentials entered by the assistant at
  any point — see this session's own back-and-forth on why that boundary
  holds even for a disposable local test account): `classic` now shows
  the light editor, `green` still shows the dark one with the existing
  `#eee` text-contrast fix intact. The `editor.png` README screenshot had
  been captured BEFORE this fix and was unintentionally documenting the
  bug itself; replaced.
- **A real, shipped bug, not a hypothetical one: the toggle button was
  missing its own `class` attribute.** `shell.html`'s button had
  `id="theme-toggle-btn"` (needed by `theme.js`'s `getElementById`) but never
  `class="theme-toggle-btn"` — the class every CSS override in every
  `css/themes/*.css` file actually targeted. Across two separate rounds of
  "make this icon less visible" edits, none of that CSS ever matched
  anything; the button rendered off whatever the generic `button`/
  `.sidebar-links button` fallback rules produced, which is why removing its
  border/background/padding visibly changed nothing. A second, unrelated bug
  compounded the confusion while chasing the first: a local test
  `wiki-server` survived a `pkill -f wiki-server` (the compound shell call's
  exit code looked like a successful kill; it had matched something else),
  so the next attempt to relaunch it failed immediately with a silent
  `FATAL Address already in use`, and the OLD process kept serving the
  stale, pre-fix `shell.html` the whole time — turning a missing HTML
  attribute into what looked, for a while, like an unsolvable CSS
  specificity puzzle. Root-caused only by checking `getComputedStyle()` /
  `element.matches(':hover')` in a real, live browser instead of trusting a
  screenshot's appearance, and separately by `ps aux` instead of trusting
  `pkill`'s own exit code.

### Configurable default theme (`[server].theme`)

Every theme so far was a per-browser choice only — `theme.js`'s picker,
`localStorage`, nothing server-side. Once three real themes existed, "what does
a brand-new visitor see before they've ever touched the picker" became a real
question with no config-level answer: it was always green, unconditionally,
baked into shell.html's own bootstrap script.

- **Same injection mechanism `[server].base_path` already established** —
  `PageRoutes.cpp`'s `buildShellHtml()` bakes the configured value into every
  served `shell.html` as `window.__WIKI_DEFAULT_THEME__`, in the SAME
  injected `<script>` tag `__WIKI_KNOWN_BASE_PATH__` already uses (one
  `<head>`-marker replacement doing double duty now, not two separate ones).
- **Deliberately unvalidated on the C++ side** — `AppConfig::defaultTheme` is
  a plain pass-through string, the same treatment `mcpScope` already gets.
  shell.html's own bootstrap script already validates ANY theme value
  (whether from `localStorage` or this new global) against its `THEMES`
  array before ever using it; duplicating that allowlist server-side would
  just be the same check twice for no benefit. An unrecognized config value
  falls through to the hardcoded `"green"` default exactly as if
  `[server].theme` had never been set — confirmed live, not assumed.
- **Priority order, confirmed live**: a reader's own saved choice in
  `localStorage` always wins over this setting — it only ever decides what a
  genuinely fresh browser (nothing saved yet) lands on. Verified with a
  three-part live check: (1) a cookie-free headless Chromium against a
  scratch server configured `theme = "classic"` actually renders classic
  with an empty `localStorage`; (2) the same server reconfigured with a
  nonsense theme name still renders green, proving the client-side allowlist
  actually rejects garbage rather than that path just never being exercised;
  (3) pre-seeding `localStorage` with `"dark"` against that same
  nonsense-configured server still yields `dark`, proving the priority order
  holds in the one case that could have silently inverted it.
- **The one piece of this that couldn't be a lightweight `static/` push**:
  this touches `AppConfig`/`PageRoutes.cpp`, both compiled into the actual
  `wiki-server` binary — required the full cross-compile pipeline
  (`docs/deployment.md`'s "Cross-compilation" section), not the
  tar-and-atomic-swap `static/`-only ritual most of this session's other
  fixes used. Verified before shipping: `qemu-arm-static` against the cross-
  compiled `unit_tests` (all 126 cases, including three new ones for this
  field) AND against the actual `wiki-server` binary itself (real startup,
  real vault rescan, real config parse — not just "it compiled"), before
  ever copying it to the real device. Binaries swapped atomically
  (`wiki-server.old`/`wiki-mcp.old` alongside the new ones, never overwritten
  in place), service restarted, confirmed exactly one live process
  afterward, `__WIKI_KNOWN_BASE_PATH__`'s own injection re-verified
  unaffected as a regression check on the shared code path both globals now
  share.

## Two-binary layout

`libwikicore` (vault + index + MCP tool logic) — no dependency on Drogon/OpenSSL.
Linked into both executables:

- `wiki-server` — HTTP (Drogon), controllers, CSP views.
- `wiki-mcp` — stdio MCP entrypoint, spawned directly by Claude Desktop/Code, no HTTP
  stack overhead.

## Build

```sh
# one-time: clone + bootstrap vcpkg (not in git)
#
# A FULL clone, not --depth 1 -- confirmed live (2026-09-04, building the
# Docker image) that a fresh shallow clone of vcpkg TODAY does not
# contain vcpkg.json's own pinned builtin-baseline commit, and manifest
# mode needs `git show <that commit>:versions/baseline.json` to resolve
# every dependency's version. This isn't a one-off fluke: upstream vcpkg
# advances constantly, and --depth 1 only fetches whatever commit
# happens to be its CURRENT tip at clone time -- a baseline pinned any
# amount of time ago eventually falls outside that single commit's
# shallow history, and a shallow clone of a repo history that long
# doesn't get *safer* over time, only more likely to be missing it.
# --depth 1 looked harmless the day this baseline was pinned (the tip
# WAS recent then); it silently stopped working at some point since,
# with nothing about the command itself changing. A full clone is ~185MB
# (vs. ~30% less shallow) and costs nothing at runtime -- vcpkg/ is
# gitignored, never shipped, cloned once per machine/image build.
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
