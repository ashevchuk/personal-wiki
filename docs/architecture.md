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
does not exist in this repo. Confirm/prompt/alert from the JS pages go
through `WikiDialog` (`static/js/dialog.js`) — a themed `<dialog>`, not
the host browser's `window.prompt` (which cannot pick up this app's
palette and punched a grey OS box over the page on folder Rename/Move).
The CSP-view-specific gotchas below (M2) are kept as
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
- **Deployment** *(original M0 intent — native-on-Pi for MVP, arm64 cross-compile
  later; what actually shipped and was live-verified is musl-static armv7
  cross-compile, native-on-device never done — see `docs/deployment.md`)*: a
  bare binary + systemd.

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
  (doesn't try to roll the document itself back). Document rename/move
  (`DocumentService::rename`, `POST /api/documents/move`): same file + `.assets/`
  `rename()`, then inbound `[[wiki-link]]`s and `assets/<stem>.assets/` hrefs in
  other documents are rewritten and those sources reindexed. The SQLite row is
  *re-pathed in place* (`IndexUpdater::repathOne`) so `rowid_id` — and therefore
  `document_snapshots` / history — survives; `removeOne` + insert would mint a
  new row and drop it. Folder move (`FolderService::move`,
  `POST /api/folders/move`) is the same contract over a prefix: the directory
  rename is atomic (documents + nested `.assets/`), then inbound `[[wiki-link]]`s
  and `assets/<old-folder>/` hrefs whose vault path sat under that prefix are
  rewritten (inside the subtree and outside it) and those sources reindexed,
  with each moved document's index row re-pathed in place for the same
  history-preserving reason. A forgotten `.md` on create (and on rename's destination)
  is appended server-side; `.MD` is canonicalized to `.md`.
- **Attachments**: no extension allowlist (removed; serving-side
  `isSafeToRenderInline` is the actual safety boundary), filename sanitization
  (`[A-Za-z0-9._-]`, everything else → `_`), **no app-level size cap** (a 120 MiB
  PDF is a legitimate personal-wiki attachment; disk is the bound). HTTP uploads
  are still bounded by Drogon's `setClientMaxBodySize` (2 GiB, fits 32-bit
  `size_t` on armv7) and whatever `client_max_body_size` the reverse proxy uses
  (nginx defaults to 1m — raise it, see `docs/deployment.md`). MCP large files
  go through `attach_file`'s `source_path` (streamed `copy_file`, stdio only) or,
  on remote HTTP, `attach_file_begin` then `PUT /mcp/uploads/{id}` (raw body,
  bytes never enter the model). Not through base64. De-duped via a short UUID suffix on a name collision.
  `GET /assets/{path...}` resolves visibility through the
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
- **`wiki.env.example` / `EnvironmentFile=-/etc/opt/wiki/wiki.env`.**
  Install prefix is `/opt/wiki`, so FHS host config lives in
  `/etc/opt/wiki/` (not `/etc/sysconfig`, not `/etc/default`). The
  leading `-` keeps the unit startable when the file is absent.
  `AppConfig` still does not map config keys from the environment; the
  runtime secret consumers are `CloudEmbeddingProvider` and
  `CloudChatClient`, which `getenv()` whatever
  `[embeddings].api_key_env` / `[llm].api_key_env` name (see
  `docs/embeddings.md`, `docs/llm.md`, and `systemd/wiki.env.example`).
  Admin credentials live in SQLite; sessions and remote-MCP tokens are
  random opaque values, not signed from this file. Most deployments
  (`provider = "none"` / `"local"`, or a loopback OpenAI-compatible server
  that doesn't authenticate) never need the file. The file started as an
  empty M5 hook; the consumer arrived with cloud embeddings.
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

### MCP write access (Phase 2, local stdio — `create_document`/`update_document`/`attach_file`)

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
  optional extra layer on top). Large-file attach is a two-step capability URL:
  `attach_file_begin` (Bearer, like any other tool) issues a UUID ticket, then
  `PUT /mcp/uploads/{id}` accepts the raw bytes with **no** Authorization header
  (the UUID is the secret; same enabled / IP allowlist / writeEnabled gates).
  `source_path` is rejected here — that would read arbitrary files off the wiki host.
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
backup and finding them cluttering the archive before adding the exclusion) and
`.mcp-uploads/` (one-shot remote-MCP large-file tickets, equally transient). The
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
  neon glow via `text-shadow`, shouting-caps buttons) simply don't exist
  in `dark.css`/`classic.css` at all, rather than being toggled off by a
  variable.
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
- **Theme changes reload the page rather than live-swapping anything.**
  `theme.js` writes the choice to `localStorage` and calls `location.reload()`
  — consistent with this app's existing full-reload-per-navigation
  architecture, and it sidesteps having to reconcile any page state that
  assumed one theme was active.
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

### Stress testing (concurrency + resource exhaustion) — two real bugs found and fixed

Distinct from `security_e2e.py` (sequential logical correctness — auth, CSRF,
traversal, visibility). Two new `ctest` targets under a `stress` LABEL (excluded
from the default `-LE stress` sweep, run with plain `ctest` or `ctest -L stress`):
`stress_concurrency.py` fires check-then-act and shared-connection paths from many
threads at once; `stress_resources.py` hammers upload caps, concurrent search load
vs. `/healthz`, and pathological path lengths. See `tests/integration/_stress_common.py`
for the shared harness (own dynamically-picked port per script, deliberately NOT
shared with `security_e2e.py` so nothing here can destabilize the release gate).
Also added: coverage-guided libFuzzer harnesses (`tests/fuzz/`, off by default —
`WIKI_ENABLE_FUZZING`, Clang-only, not part of `ctest`) for `PathGuard::resolve()`
and `parseFrontMatter()`, the two parsers that take raw untrusted bytes directly.

Two real, previously-unknown bugs surfaced live and were fixed the same day
(2026-09-10):

1. **Absolute vault-path disclosure via generic exception handlers.** A `path`
   segment over the filesystem's NAME_MAX (255 bytes on ext4) throws
   `std::filesystem::filesystem_error` — its `.what()` embeds the full absolute
   on-disk path (BOTH paths, for `FolderService::move`'s own explicitly-constructed
   `filesystem_error`). Every mutating route's generic
   `catch (const std::exception& e) { ...jsonError(500, e.what())... }` was
   returning that straight to the caller instead of a clean `400` — found first in
   `DocumentRoutes.cpp` (`stress_resources.py`'s pathological-path check), then the
   identical pattern confirmed and fixed across `FolderRoutes.cpp` (both handlers),
   `VersionRoutes.cpp`'s `document-restore` handler (which was ALSO entirely missing
   a `PathTraversalError` catch — an independent second bug in the same handler,
   falling through to the same generic 500 for a condition every other mutating
   route already gave a clean 400 for), and `RemoteMcpRoutes.cpp`'s
   `create_document`/`update_document` — the highest-stakes copy of the four, since
   that's the public-HTTP remote MCP transport (see "Remote MCP transport (HTTP)"
   above). Fix: an explicit `catch (const std::filesystem::filesystem_error&)`
   before the generic catch in each handler, mirroring the read-route pattern that
   already existed one section up in `DocumentRoutes.cpp` (`filesystem_error` →
   clean `404`, no message leak) — `RemoteMcpRoutes.cpp` keeps the full `e.what()`
   in `mcp_audit_log` (admin-only, DB-stored, exactly the forensic detail that table
   exists for) while returning only `"invalid path"` to the calling MCP client.
   Verified live against each fixed route: manual curl repro with an 8000-byte path
   segment (both directly and, for the remote transport, over a real bearer-token
   `tools/call` JSON-RPC request with write access enabled) — clean `400`/tool-error,
   no path in the response body, audit log still shows the full detail.

2. **Real race condition in `IndexUpdater`, not a test artifact.** `upsertOne`/
   `removeOne` each wrap a `BEGIN IMMEDIATE...COMMIT/ROLLBACK` sequence as several
   separate SQLite C API calls against `db_` — one `sqlite3*` connection shared BY
   REFERENCE across every Drogon request-handling thread (`main.cpp`'s single live
   `IndexUpdater indexUpdater(db)`), with no mutex protecting that sequence.
   `main.cpp` already documents this EXACT failure mode ("two threads racing a BEGIN
   on the SAME connection handle is a 'cannot start a transaction within a
   transaction' error, not a safely-serialized one") — it's the reasoning
   `VaultWatcher` was given its own separate connection FOR — but that reasoning was
   never extended to request threads racing EACH OTHER on the connection they all
   share. `stress_concurrency.py`'s same-document-path check (12 concurrent PUTs to
   one path) reproduced this live as an actual `500`
   (`{"error":"statement failed: not an error"}` — SQLite's own error string, read
   after another thread's call on the shared connection had already overwritten it)
   on roughly 1 run in 8. Fixed with a `std::mutex` member on `IndexUpdater` (the
   same `std::mutex` + `std::lock_guard` shape `RateLimiter` already uses) guarding
   the whole transaction in both `upsertOne` and `removeOne`; the three read-only
   queries (`allIndexedPaths`, `rowIdForPath`, `findPathByUuid`) stay unguarded on
   purpose — each is a single prepare/step/destroy, no BEGIN/COMMIT window for
   another thread to land inside. Re-verified post-fix with 30×12 (360) concurrent
   same-path writes: zero bad results, where the pre-fix version needed under 96
   requests to reproduce.

### Memory-safety pass: ASan/UBSan across the whole binary (2026-09-10)

The `tests/fuzz/` libFuzzer harnesses only exercise two functions in isolation
(`PathGuard::resolve`, `parseFrontMatter`) for a short smoke run each — real
buffer-overflow/UB coverage of the actual `wiki-server`/`wiki-mcp` binaries needed a
separate pass. Built a throwaway `build-asan/` (GCC, `-fsanitize=address,undefined
-fno-sanitize-recover=all`, `wiki-server`/`wiki-mcp`/`unit_tests` all instrumented,
never committed — same disposable-verification-directory treatment as `build-fuzz/`)
and ran the FULL suite against it: `unit_tests`, `security_e2e`, and both stress
scripts, all 4/4 passing with zero ASan/UBSan reports anywhere in the output —
wider coverage than the fuzz harnesses alone, since this exercises JSON parsing,
multipart upload handling, and FTS5 snippet extraction, not just the two fuzzed
functions.

**Correction (2026-09-10, same day, caught while investigating the md4c finding
below): this pass did NOT actually instrument md4c or yaml-cpp.** `CMAKE_CXX_FLAGS`
only applies to this project's OWN targets (`wikicore`, `wiki-server`, `wiki-mcp`,
`unit_tests`) — vcpkg builds each dependency through its own port build scripts in a
separate CMake invocation that does not inherit the top-level project's ambient
`CMAKE_CXX_FLAGS`/`CXXFLAGS`. So "zero ASan/UBSan reports" above is real but narrower
than originally stated: it covers this project's own code exercising md4c/yaml-cpp
through their public APIs, not the sanitizer actually watching memory accesses
*inside* those libraries' own compiled code. A real heap-buffer-overflow inside
md4c itself (see below) was sitting the entire time this pass ran clean, precisely
because the instrumentation never reached the code where it lived. If this kind of
pass is redone, getting real coverage of a vcpkg dependency's own code requires
either compiling that one dependency out-of-vcpkg with sanitizer flags directly
(as the md4c investigation below ended up doing anyway, for an unrelated reason),
or a custom vcpkg triplet that injects sanitizer flags into the port build itself.

Followed with a dedicated adversarial battery (10 payloads, written directly to the
vault to bypass API validation and hit `parseFrontMatter`/`renderMarkdownToHtml` as
directly as possible): a bounded YAML anchor/alias expansion ("billion laughs"
style), 2000-deep YAML flow-sequence nesting, an unterminated front-matter block,
invalid/overlong UTF-8 bytes, 3000-deep markdown blockquote/list nesting, embedded
NUL/control characters, a single 500,000-byte markdown "word" with no whitespace,
tab-indented YAML, and astral-plane emoji mixed with malformed `[[wiki-link]]`
syntax. Server (same PID throughout, confirmed via `ps`, never restarted) survived
every one — `/healthz` stayed `200` after each, `server.log` stayed clean, no
sanitizer report. A manual pass over the codebase's own raw-pointer/buffer spots
(`grep` for `memcpy`/`reinterpret_cast`/manual index arithmetic) turned up nothing
of concern either: SQLite's `sqlite3_column_text` adapters are the usual
NUL-terminated-string case, `VaultWatcher`'s inotify-event loop is the textbook
kernel-API read pattern (not attacker-reachable over HTTP — requires local
filesystem write access to the vault, the same trust boundary as everything else in
`vault/`), `Uuid.cpp`'s `snprintf` target buffer is correctly sized, and
`YouTubeEmbed.cpp`'s marker-substitution bounds-checks every index before using it.
No buffer overflow, no use-after-free, no UB — clean, but this was a substantially
wider net than the fuzz harnesses alone, and worth being precise about what each
actually covers rather than treating "we fuzzed two functions" as "we checked for
buffer overflows."

One unrelated, non-memory-safety finding from the same adversarial pass: the
`substituteYouTubeEmbeds` doc comment in `MarkdownRenderer.cpp` claims a hand-typed
`<img src="youtube-embed:ID">` "can't be forged from a document's own text" — true
for literal HTML (md4c HTML-escapes it, confirmed), but incomplete: legitimate
CommonMark image syntax typed directly (`![](youtube-embed:ID)`) is NOT raw HTML,
isn't blocked by `MD_FLAG_NOHTMLSPANS`, and DOES get substituted into a real
`<iframe>` — bypassing `rewriteYouTubeEmbeds`' URL-recognition step entirely.
Confirmed live. Not a capability escalation in practice (the single admin who can
create documents can already embed any YouTube video ID by pasting a real URL, so
typing the marker syntax directly grants nothing new), so left as a documentation
precision gap rather than an urgent fix — flagged here so the comment's claim isn't
taken at face value if this codebase's trust model ever changes (e.g. multiple
editors with different privilege levels).

### Three follow-up hardening items (2026-09-10)

A follow-up security review after the stress-testing/ASan pass above surfaced three
more real fixes, not speculative additions, each verified live:

1. **Constant-time comparison for the remote MCP bearer token.**
   `McpRemoteConfig::verifyToken` compared the presented token's hash against the
   stored one with plain `==` — unlike `CsrfFilter`'s comparison (which has an
   explicit comment justifying why timing safety doesn't matter there, an
   already-authenticated session), this one gates AUTHENTICATION ITSELF for an
   anonymous caller on the public-internet remote MCP transport, the highest-stakes
   credential check in the codebase. Added `auth::constantTimeEquals` (wraps
   OpenSSL's `CRYPTO_memcmp`, next to the existing `sha256Hex`/`randomHexToken` in
   `Crypto.h`/`.cpp`) and switched `verifyToken` to it. New `CryptoTest.cpp` covers
   equal/unequal/different-length/empty cases plus a real `sha256Hex` round-trip.
2. **Security response headers**, added via `drogon::app().registerPreSendingAdvice`
   in `main.cpp` (not `registerPostHandlingAdvice` — Drogon's own docs note static
   file responses only go through the former, and this needed to cover
   `static/`-served JS/CSS too, not just JSON API responses): `Content-Security-
   Policy`, `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`,
   `Referrer-Policy: strict-origin-when-cross-origin`. Defense-in-depth backstop
   behind md4c's `MD_FLAG_NOHTMLBLOCKS/SPANS` (the actual primary defense), not a
   replacement for it. The CSP is deliberately not maximally strict:
   `script-src 'self' 'unsafe-inline'` because `shell.html` has two inline
   `<script>` blocks (the `PageRoutes.cpp` config-injection one and the anti-FOUC
   theme bootstrap right after it) that a nonce/hash-based policy would need
   updating on every `PageRoutes.cpp` edit — a known, accepted relaxation, not an
   oversight. `worker-src 'self'` is spelled out (rather than relying on the
   CSP3 fallback through `script-src`) because `graph-layout.js` runs as a
   dedicated Worker; a missing `worker-src` on a pickier engine would silently
   fall back to main-thread layout. Everything else stays strict:
   `object-src 'none'`, `frame-ancestors 'none'`,
   `frame-src https://www.youtube.com` as the one legitimate cross-origin
   iframe exception, `img-src 'self' https: data:` since markdown bodies can
   legitimately link any external image. Verified live in a real browser: login,
   Toast UI Editor (create/edit a document, all vendored JS/CSS loads), and a real
   YouTube embed (an actual `<iframe>` to `youtube.com` that loaded real YouTube
   content, confirming `frame-src` isn't blocking it) — zero CSP violations in the
   console across all of it (the only console entries were unrelated Chrome
   extension noise).
3. **`systemd/wiki.service` hardening**, extended well past the original
   `ProtectSystem=strict`/`NoNewPrivileges`/`ProtectHome`/`PrivateTmp`/
   `ReadWritePaths` set: `CapabilityBoundingSet=` (empty), `RestrictAddressFamilies`,
   `RestrictNamespaces`, `RestrictSUIDSGID`, `RestrictRealtime`, `LockPersonality`,
   `MemoryDenyWriteExecute`, the `Protect{KernelTunables,KernelModules,KernelLogs,
   ControlGroups,Clock,Hostname}` family, `PrivateDevices`, and
   `SystemCallFilter=@system-service`. Verified with an actual transient systemd
   unit (`systemd-run`, real root sudo on the dev machine, not just
   `systemd-analyze verify`) running the real `wiki-server` binary with this full
   directive set — caught two real test-setup pitfalls along the way, both
   artifacts of the TEST harness rather than the unit file: `PrivateTmp=yes` gives
   the service its own private `/tmp`, so a scratch sandbox living under `/tmp/`
   silently became invisible to the service; `ProtectHome=yes` does the identical
   thing to `/home`. Moving the scratch sandbox to `/opt/...` (matching the real
   `/opt/wiki` deployment path) fixed both. The one finding that WOULD matter for a
   real deployment change: `CapabilityBoundingSet=` empty strips
   `CAP_DAC_OVERRIDE`, so root running WITHOUT that capability against a directory
   it doesn't own fails with a confusing "attempt to write a readonly database" —
   harmless here since the documented deployment already runs as `User=wiki` owning
   `vault_data` (`chown wiki:wiki`, see docs/deployment.md), but re-verify this
   specific dependency if `User=`/`ReadWritePaths`/file ownership ever changes,
   rather than assuming it still holds. Once running as an unprivileged user that
   genuinely owns its data dir, the full hardened unit passed a real end-to-end
   exercise: `--create-admin`, login, CSRF-protected document create, FTS5 search,
   multipart attachment upload — all clean. Fully torn down afterward (transient
   unit stopped, scratch test user removed, sandbox directories deleted) — nothing
   from this verification pass was left running or installed on the dev machine.

   **Update, same day, real production deploy: `SystemCallFilter=@system-service`
   does NOT ship — removed after actually breaking the live service.** The
   x86_64/glibc verification above passed with `SystemCallFilter` included; the
   ARM cross-compiled musl-static binary, deployed to the real production target
   (systemd 232, real armv7 hardware — see docs/deployment.md), crashed
   immediately in a `Restart=on-failure` loop: `code=killed, status=31/SYS`, a
   seccomp SIGSYS kill. systemd 232 logs no syscall number for that kill, so the
   specific blocked call was never identified — musl vs. glibc and/or ARM32 vs.
   x86_64 is the most likely reason `@system-service`'s syscall set (which is
   itself architecture/libc-dependent, not a fixed list) behaved differently on
   the two targets. Rolled back within under a minute using the standard
   `.bak-<timestamp>` unit-file copy this repo's redeploy recipe always keeps
   (docs/deployment.md) — real downtime, but short and immediately caught by the
   post-restart verification step, not left silently broken.

   This also **falsified a specific claim the original comment block made**: it
   predicted that on an older systemd (232), unrecognized newer directives would
   log an "Unknown lvalue" warning and be skipped, harmlessly. That part turned
   out to be exactly right when the corrected unit (SystemCallFilter removed) was
   redeployed and its journal was checked: `RestrictSUIDSGID`, `LockPersonality`,
   `ProtectKernelLogs`, `ProtectClock`, `ProtectHostname` all logged exactly that
   "Unknown lvalue ... in section 'Service'" line on the real box and were
   harmlessly skipped, while the rest (`CapabilityBoundingSet`,
   `RestrictAddressFamilies`, `RestrictNamespaces`, `MemoryDenyWriteExecute`, the
   `ProtectKernel*`/`ProtectControlGroups` pair, `PrivateDevices`,
   `SystemCallArchitectures`) were recognized and are genuinely active on
   production right now. What the original comment got WRONG was treating
   `SystemCallFilter` as belonging to that same "maybe just an unrecognized key"
   risk category — it isn't: systemd 232 recognizes and enforces
   `SystemCallFilter` just fine, so the failure mode for a syscall-level filter
   is a live crash loop, categorically different from (and worse than) a
   silently-skipped unrecognized directive. `qemu-arm-static` could never have
   caught this either way, confirmed the hard way: it emulates instructions, not
   kernel-level seccomp filtering, so a `qemu-arm-static ./wiki-server
   --create-admin` smoke test (which this deploy DID run, and which passed) is
   structurally incapable of exercising `SystemCallFilter` at all.

   Current state (both verified live, production included): every directive
   below `PrivateDevices=yes` in the unit file except `SystemCallFilter` — which
   is gone — plus `SystemCallArchitectures=native` (an ABI-table restriction, not
   a syscall-level allowlist, and confirmed not implicated in the crash: a
   native-arch binary on native-arch hardware has nothing for this directive to
   reject in the first place).

### vcpkg dependency CVE scanning — investigated thoroughly, patched one real finding (2026-09-10)

Investigated whether automated CVE scanning of this project's vcpkg C/C++
dependencies is possible at all, empirically rather than from documentation alone,
and ended up finding and patching a real vulnerability along the way.

**What does NOT work, each verified live, not assumed:**
- `OSV-Scanner` (v2.5.1): has no extractor for `vcpkg.json` at all
  (`could not determine extractor`), and vcpkg's OWN generated SPDX SBOMs use a
  `pkg:vcpkg/<port>@...` PURL that isn't a registered package-url type — OSV-Scanner
  reports "No issues found" against them, which means zero packages were actually
  checked, not that zero vulnerabilities exist. The exact silent-wrong-answer shape
  `~/.claude/CLAUDE.md` warns about generally, hit here concretely.
- `Trivy` (v0.73.0): has no vcpkg/C-library extractor either; a filesystem scan of
  this repo (with `vcpkg/` cloned locally) found vulnerabilities only in npm/pip
  dependencies belonging to vcpkg's OWN tooling/docs scripts (e.g. nlohmann-json's
  mkdocs site, vcpkg's own azure-pipelines helper) — zero relevance to what actually
  ships in `wiki-server`/`wiki-mcp`.
- GitHub's Dependency Graph: the OFFICIAL supported-ecosystems table (fetched and
  quoted verbatim, not recalled from training data) lists Cargo/Composer/Deno/Go
  modules/Gradle/Maven/npm/pnpm/pip/Poetry/RubyGems/Swift PM/Bazel/Yarn and, for
  C/C++ specifically, only NuGet (`.vcxproj`/`.nuspec` — the MSBuild/Windows
  dependency model, unrelated to how this project consumes C++ libraries). vcpkg is
  absent from this list entirely.
- `microsoft/component-detection` (Microsoft's own tool, DOES have a real Vcpkg
  detector — verified by downloading the actual `linux-x64` release binary and
  running it against this repo's `build/vcpkg_installed/`): correctly identifies
  real upstream repo+version for every dependency (`openssl/openssl@openssl-3.6.4`,
  `drogonframework/drogon@v1.9.13`, etc.) — genuinely better output than the two
  tools above. But GitHub's own Dependency Submission API docs state, verbatim:
  "You will only get Dependabot alerts for dependencies that are from one of the
  supported ecosystems for the GitHub Advisory Database" — the same unsupported-list
  as the Dependency Graph itself. Detection working perfectly doesn't help if the
  downstream alerting system was never going to match on it.

**What DOES work, found by testing rather than by more documentation-reading:**
OSV.dev's query API accepts a **git commit hash directly** (`POST
https://api.osv.dev/v1/query {"commit": "<sha>"}`) instead of a package+ecosystem+
version triple — exactly the mechanism this ecosystem actually needs, since OSS-Fuzz
(which feeds a large fraction of OSV's own database) already tracks vulnerabilities
by affected git-commit-ranges in the upstream repo, independent of any package
manager. Resolved the real pinned commit for each of this project's vcpkg
dependencies (`git ls-remote --tags <repo>` against the exact tag each vcpkg port
pins) and queried each — six came back clean, one did not:

**`md4c` (`release-0.5.3`, `472c417005c2c71b8617de4f7b8d6b30411d78f4`) — `OSV-2022-126`,
a heap-buffer-overflow READ in `md_analyze_table_alignment()`, MEDIUM severity,
found by OSS-Fuzz.** The OSV record has an `introduced` commit but no `fixed` one,
and lists `release-0.5.3` explicitly among affected versions — this project's
currently-pinned version. Confirmed by reading the actual vulnerable code
(`src/md4c.c`): `while(CH(off) != '-') off++;` scanning a table's alignment/
delimiter row (`| :--- | ---: |`) with no bound at all, unlike the very next loop
in the same function which does check `off < end`. Found the fix upstream by
querying OSV against md4c's current HEAD (came back empty — meaning HEAD is NOT
in the affected range) and then diffing `release-0.5.3..HEAD`: commit `ecbb091`
("md_analyze_table_alignment: Bound the dash scan by the row end", merged
2026-06-17, matches the exact code and exact missing bound) — real, on `main`,
just never cut into a tagged release.

Attempted a live crash reproduction before patching (chosen deliberately over
patching blind): compiled md4c's `release-0.5.3` sources standalone with
`-fsanitize=address,undefined`, tried six hand-constructed malformed-table inputs
(none crashed — the exact OSS-Fuzz input wasn't guessable by hand), then built a
dedicated libFuzzer harness around `md_html()` at that same tag and ran it
~2 million iterations across two rounds (~4 minutes total) — still no crash. Not
a contradiction: OSS-Fuzz found this via a continuous, distributed, long-running
campaign, not a coincidence a short local session was likely to reproduce. Judged
the combination of (a) an authoritative OSV record sourced from `google/oss-fuzz-vulns`
naming this exact version, (b) personally reading and understanding the vulnerable
code, and (c) locating and reading the exact matching upstream fix as sufficient
evidence to act on without an independent local crash — that's a different, and
here acceptable, bar than "shipped this without reading the code at all."

**Fix**: `overlay-ports/md4c/` — a local vcpkg overlay port (wired in via the new
`vcpkg-configuration.json`'s `overlay-ports`), copying the real upstream vcpkg
port's files and adding one more patch,
`0001-md_analyze_table_alignment-fix-oob-read-OSV-2022-126.patch` (a clean
`git format-patch` of `ecbb091`, applies against `release-0.5.3` unmodified,
verified), plus `"port-version": 1` in the overlay's `vcpkg.json` so vcpkg's own
package hash correctly distinguishes it from the unpatched upstream port and
triggers a rebuild instead of reusing a stale binary cache entry. Verified the
overlay actually took effect by diffing vcpkg's OWN buildtree checkout before/after
(the patched line, `off < end && CH(off) != '-'`, present in the rebuilt source);
full `ctest` 4/4 clean afterward; and a real GFM table with all three alignment
types (`:---`, `:---:`, `---:`) rendered through the live server with correct
`align="left"/"center"/"right"` output — the fix only adds a bound, it doesn't
change any normal-input behavior, and this confirms that held. Drop this overlay
port (and `vcpkg-configuration.json` if nothing else ever needs it) once
`vcpkg.json`'s `builtin-baseline` advances to a commit where md4c's own pinned
version already contains this fix in a tagged release.

**Conclusion for future dependency-vulnerability work on this project**: there is no
drop-in automated solution for the vcpkg dependencies as a whole (verified, not
assumed) — the commit-based OSV.dev query is a real, working, MANUAL technique for
spot-checking a specific pinned dependency when there's a reason to suspect
something (as here), not a CI-automatable blanket scan, since it requires first
resolving each dependency's actual pinned commit by hand. GitHub's Dependabot alerts
now cover the GitHub Actions ecosystem in `.github/workflows/ci.yml` (enabled the
same day, see the `vulnerability-alerts` API) — genuinely useful, but for a
completely different, non-overlapping set of dependencies than vcpkg's C/C++ ones.

## CodeQL scoping — paths/paths-ignore does not work for a built compiled language

`.github/workflows/codeql.yml` originally tried to scope CodeQL to this project's
own `src/`/`tests/` via a `paths:` allowlist passed as inline config to
`codeql-action/init`. This looked correct on every push-triggered run (5–8 min)
for four days straight — zero stray alerts. The weekly scheduled run
(2026-09-14, 41 min — five times longer) told a different story: 70 new open
alerts, every single one inside `vcpkg/buildtrees/{openssl,c-ares,zlib}/...` —
none of it this project's code.

Root cause, confirmed two ways: (1) the actual `user-config.yaml` CodeQL wrote at
run time DID contain the correct `paths: [src, tests]` — pulled the real log
line, the allowlist was received intact, so this wasn't a YAML-passing bug; (2)
GitHub's own docs state `paths`/`paths-ignore` apply "when you analyze a compiled
language without building the code" and that scoping a built compiled language
requires altering "appropriate build steps in the workflow" instead — i.e. the
option is a documented no-op once a real build (`build-mode: manual`, as here) is
involved. The push runs looked clean only because `actions/cache` kept hitting
(unchanged `vcpkg.json` hash) and vcpkg had nothing left to rebuild — no
compilation of `openssl`/`c-ares`/`zlib` ever happened on those runs for CodeQL to
trace. The scheduled run hit a cold cache, rebuilt every vcpkg port from source,
and CodeQL — already tracing by then — dutifully flagged all of it.

**Fix**: moved the `Configure` step (the one that triggers vcpkg manifest-mode
install, i.e. actually compiles any cache-missed dependency) to run BEFORE
`codeql-action/init` turns on compiler tracing, instead of after. Dependency
builds now happen entirely outside the traced window; only `cmake --build`
(which, by then, is just compiling `src/`/`tests/` and linking already-built
deps) runs traced. This scopes by *when tracing starts*, not by a filter that
doesn't apply to this build mode — removed the now-known-ineffective `paths:`
config entirely rather than leaving it in as false reassurance. All 70 stray
alerts dismissed (`won't fix` — vendored/upstream build output, not this
project's code) after confirming none had a plausible connection to anything
this project's own build steps or overlay ports touch.

## Mermaid diagram rendering

A ```` ```mermaid ```` fenced code block now renders as a real diagram instead of a
plain code block, with no pre-parse rewrite needed the way `[[wiki-links]]` or
YouTube embeds require — a fenced code block with an info string is already a
first-class CommonMark/GFM construct md4c parses for free. `md4c-html.c`'s
`render_open_code_block()` (read directly, not assumed) always emits exactly
`<pre><code class="language-LANG">` for a fenced block with a known info
string, HTML-escaping the code content. `MarkdownRenderer::substituteMermaidBlocks`
is a pure post-substitution on that exact shape — same technique as
`substituteYouTubeEmbeds`, on a disjoint HTML shape — swapping it for
`<pre class="mermaid">`, the container mermaid.js's own default selector looks
for. Requires an EXACT `class="language-mermaid"` match (nothing appended
after `mermaid` in the fence's info string); anything else degrades to an
ordinary code block rather than guessing at a partial match — covered by a
dedicated test (`a fence info string that only STARTS WITH "mermaid"`).

**Rendering itself happens entirely client-side, lazily.** This app's
document content is injected via `fetch()` + `innerHTML` (`pages/view.js`),
never present at initial page load for mermaid.js's own `startOnLoad` to
catch — `static/js/mermaid-render.js`'s `renderIn(container)` is called right
after that injection, calls `mermaid.run({ nodes })` scoped to whatever
`pre.mermaid` elements are actually in the container. mermaid.js itself
(`static/js/mermaid/mermaid.min.js`, vendored, MIT, version pinned — see
`static/js/mermaid/VENDORED.md` and `tools/build-editor-bundle/fetch.sh`) is
~5.3 MiB (~1.5 MiB gzipped) — roughly 10× the size of the vendored Toast UI
Editor bundle, since it bundles its own layout engine per diagram type.
Unlike Toast UI Editor (loaded unconditionally from `shell.html` on every
page), this is deliberately NOT loaded from `shell.html`:
`mermaid-render.js`'s `renderIn()` no-ops, fetching nothing, whenever a
document has no `pre.mermaid` block at all — confirmed live via the browser's
own network log, not assumed: a document without a diagram shows zero
requests for `mermaid.min.js`; a document with one shows exactly one `200`.
Paying ~1.5 MiB for every single page view, on a project whose deployment
story explicitly includes weak SBC hardware over possibly slow links, for a
feature the overwhelming majority of documents will never use, would be the
wrong tradeoff.

**Diagram palette follows the site theme, not just container chrome.**
`mermaid-render.js`'s `mermaidTheme()` reads `<html data-theme>` (same
mechanism `pages/edit.js` already uses to pick Toast UI Editor's own light/
dark option) and passes mermaid.js `theme: "default"` for `classic`,
`"dark"` for `green`/`dark` — mermaid.js has its own internal node/edge
color scheme separate from any CSS, so this is a real, necessary config
call, not just page styling. The `pre.mermaid` CSS rule added to each
`css/themes/*.css` file only styles the container the resulting SVG lands
inside (background/border/padding, matching each theme's own `--panel-bg`/
`--border` variables) — confirmed live in both a dark and a light theme via
a real rendered diagram screenshot in each, not just by reading the CSS.

## Mermaid editor preview, syntax highlighting, and a real display:none bug

Two follow-ups to the mermaid work above, shipped together since both
needed a container styling pass anyway.

**Editor preview**: `static/js/mermaid-editor-preview.js` plugs Toast UI
Editor's `customHTMLRenderer.codeBlock` hook — the same technique
`youtube-embed-preview.js` uses for `image` nodes — to turn a
` ```mermaid ` block into the same `<pre class="mermaid">` shape the
server-side renderer produces, then reuses `mermaid-render.js`'s
lazy-loader against it rather than a second copy of that logic.
Empirically confirmed (a throwaway sandbox loading the real vendored
3.2.2 bundle directly, not this app, before writing any of this) that
`node.info` carries the fence's bare language string, `node.literal` the
raw content, and `type: "text"` content is HTML-escaped by the library
itself — tested with a live `<img src=x onerror=alert(1)>` payload,
which rendered as inert text, never executed. Also confirmed the one
hard limit this hook has: its return value is honored ONLY by the
Markdown-mode "Preview" panel (`.toastui-editor-md-preview`) — the
WYSIWYG canvas always keeps a code block as its own editable widget,
this hook's return value silently discarded, the same ProseMirror
collapse-to-default behavior `youtube-embed-preview.js` already
documents for an inline image node, just for a block node this time.
Live-scoped syntax highlighting was explicitly ruled OUT of the editor
entirely (a deliberate scope decision, not an oversight) — only the
document view page gets Prism.js (below).

**A real bug found shipping the preview, not a hypothetical**: the first
version rendered flowchart diagrams as a real `<svg>` with NO console
error, but at ~0px visible height — permanently. Root-caused by actually
measuring, not guessing: `getBoundingClientRect().height` was ~59px for
a broken flowchart vs. ~434px for a correctly-sized sequence diagram
pulled from the exact same page. mermaid's dagre-based flowchart layout
engine calls `getBBox()` on the live DOM to size nodes, which every
Chromium/WebKit browser returns as all-zero for anything inside a
`display:none` ancestor — Toast UI's Preview panel IS `display:none`
whenever the "Write" tab is active instead of "Preview". `sequenceDiagram`
was unaffected because its layout is computed, not DOM-measured — which
is exactly why this looked like a diagram-type-specific bug at first,
until the actual pixel measurements ruled that out. Worse: it never
self-healed even after switching to the now-visible Preview tab, because
mermaid marks a code block "already processed" and silently skips it on
every later `mermaid.run()` call — the broken zero-height render was
permanent, not transient.

Fixed two ways together: `refreshPreview()` now checks
`getComputedStyle(el).display !== "none"` before rendering at all, and a
`MutationObserver` (`watchPreviewVisibility()`, installed once per editor
mount) watches for the Preview panel's own `display` flipping to catch
the render Toast UI's Write/Preview tab toggle needs — confirmed
empirically that this toggle fires NO public editor event at all
(`editor.on("changeMode", ...)` only fires for the separate markdown/
wysiwyg switch, never a bare Write/Preview tab click), so without the
observer nothing would ever trigger the deferred first render. Re-verified
post-fix with the same `getBoundingClientRect()` measurement: both
diagrams on the same real production document now report their full,
correct height.

**Syntax highlighting**: `static/js/prism-highlight.js` lazy-loads a
custom Prism.js 1.30.0 bundle (`static/js/prism/`, core + a fixed
language set — see that directory's own `VENDORED.md` for the exact list
and why it's not vendored with any theme CSS) on the document VIEW page
only, following the exact same "don't fetch anything nobody asked for"
discipline as mermaid's own lazy-loader: a no-op unless the page actually
contains a `<pre><code class="language-X">` block md4c already produces
for free from a fenced block's info string. `.token.*` colors are
hand-written per `css/themes/*.css` file — classic gets a GitHub-Light-
style palette, dark a GitHub-Dark-style one, green deliberately stays
inside its own green-family hues (amber/cyan as the only two accent
colors, differentiated mostly by BRIGHTNESS rather than hue) rather than
bolting a generic multi-hue theme onto a green-terminal identity it would
visually clash with. Also the first real on-screen container styling
`<pre><code>` blocks ever had on this app outside `@media print` — before
this, a plain fenced code block rendered as an unstyled browser-default
box on every theme.

**Follow-up, same day**: two more small fixes to this same styling.
`border` dropped from both `pre.mermaid` and `pre code[class*="language-"]`
in every theme file (background/padding/radius kept) — a plain user
preference, no functional reason behind it.

More substantively: mermaid diagrams originally used mermaid's own
built-in `"dark"`/`"default"` theme, picked per site theme the same way
Toast UI Editor's own theme option is. Looked wrong live — mermaid's
built-in dark theme has ITS OWN fixed palette (grey node fills, white
borders) with zero connection to this app's actual green-terminal/dark/
classic colors, so a diagram looked like a generic mermaid widget bolted
onto the page rather than matching it. Fixed by switching to
`theme: "base"` with explicit `themeVariables`, read LIVE via
`getComputedStyle(document.documentElement)` against this app's own CSS
custom properties — `--panel-bg` for node fill, `--fg` for text,
`--fg-dim` for borders/lines — so a diagram always uses exactly the
active theme's real palette, with no second, hand-maintained copy of
these colors sitting in JS to drift out of sync with the CSS. `--border`
deliberately excluded from this: mermaid's theming engine only
recognizes hex colors (confirmed against its own docs, not assumed), and
`green.css`'s own `--border` is `rgba(0, 255, 0, 0.35)` — not hex, unlike
the other two themes' `--border` — so `--fg-dim` (hex in all three)
stands in for node borders/lines instead of special-casing one theme.

## ```query blocks — a whitelisted live index/dashboard, no raw SQL

Inspired by looking at how other self-hosted PKM tools (SiYuan's `/sql`
embed blocks, Obsidian's Bases) solve the same real problem this
project's own users hit: a hand-maintained list of links (a recipe
index, a project roadmap) goes stale the moment a new document is added
and nobody remembers to update the list. A ` ```query ` fenced block
solves it by re-querying the actual document index on every page view
instead.

**Rendering pipeline mirrors mermaid/YouTube exactly**:
`MarkdownRenderer::substituteQueryBlocks` is the same post-substitution
technique as `substituteMermaidBlocks` — md4c already parses a fenced
block's info string for free, so `<pre><code class="language-query">`
becomes `<pre class="query">` holding the raw (already HTML-escaped) DSL
text, nothing more. One deliberate difference from mermaid: this can't
be a pure client-side render, because it needs a database. wikicore
(where `MarkdownRenderer` lives) has no database handle at all by
design — see "Two-binary layout" below — and a query needs to re-run on
every view to be a *live* dashboard rather than a snapshot frozen at
save time. So `static/js/query-block.js` reads the placeholder's text
client-side and calls `GET /api/query`, which re-parses and re-runs the
DSL fresh, every single page load.

**Security model — the actual point of this feature, not an
afterthought**: `index::QueryBlocks::parseAndRun` (src/index/
QueryBlocks.cpp) never builds SQL from a query block's TEXT, structurally,
not just "carefully": the grammar is a fixed set of `key: value` lines
(`tag`, `type`, `folder`, `orphans`, `sort`, `order`, `limit`), and every
recognized key maps to one hardcoded SQL fragment already written into
this file. A value only ever reaches SQLite as a bound parameter (tag
names, a type string, a folder prefix, the limit) or gets looked up
against a small in-code whitelist and substituted for the corresponding
literal, hardcoded string (`sort: updated` → the literal column name
`d.updated_at`, never the caller's own spelling) — there's no code path
where document text becomes part of the SQL string at all, let alone an
unescaped one.

Visibility gating reuses the exact fail-safe-private mechanism
`NavQueries`/`FtsSearch` already established: `includePrivate` comes
only from the caller's own session (`GET /api/query`'s handler calls
`isAuthenticated(req)`, same as every other read-only route), never
anything a query block's own text can influence. This matters
concretely: a query block embedded in a PUBLIC document must never let
an anonymous visitor discover a private document's title/path just
because the ADMIN who wrote the block could see it themselves at
authoring time. `orphans: true` (documents with zero incoming
`[[wiki-link]]` backlinks) applies the same direction to the backlink
check itself — a private document's own outgoing link never counts
toward making a target look "not orphaned" for an anonymous caller,
mirroring `NavQueries::backlinks`'s own documented behavior exactly.
Draft/Chat `run_query_block` is the same `parseAndRun` with
`includePrivate` true — the agent routes are admin-only, never anonymous.

An unknown key, a duplicate key, or an out-of-range value (`limit: 0`,
`sort: nonsense`) is a `400` parse error with a clear message, never a
silently empty or unfiltered result — the same "don't let a helper's
failure read as a normal, if wrong, result" discipline `~/.claude/
CLAUDE.md`'s own global rule states for shelled-out scripts, applied
here to a hand-written DSL parser instead: a typo'd `tags:` instead of
`tag:` must be visibly wrong, not silently return everything (or
nothing) with no signal anything went sideways.

**A real bug found and fixed before this ever shipped, not
hypothetical**: the first working version implemented `orphans: true` as
a `LEFT JOIN document_links ... LEFT JOIN documents src ON ... AND
(?N = 1 OR ...)` with an EXPLICITLY NUMBERED placeholder, positioned in
the SQL text (the `FROM`/`JOIN` clause) ahead of the `WHERE` clause's own
plain, unnumbered `?` placeholders. SQLite's actual rule for a plain `?`:
it gets assigned the next number after the LARGEST parameter number
already seen, in TEXT-PARSE ORDER — not in whatever order the C++ code
happened to construct fragments in. Because the numbered `?N` appeared
earlier in the final SQL text than the WHERE clause's own anonymous `?`s,
every one of those silently got bumped to the WRONG bind position —
`type`'s value could land bound to the slot meant for `folder`, for
example. Caught by actually reasoning through SQLite's numbering rule
before shipping, not by a test failure (the bug is silent — wrong values
in the wrong slots, not a crash). Fixed by rewriting `orphans` as a
`NOT EXISTS` subquery living entirely inside `WHERE`, so every
parameterized condition is appended to the same list, in the same order
its binder is pushed — text order and construction order become the
same order by construction, closing off the whole bug class rather than
just this one instance of it.

**Verification**: 12 unit tests (`QueryBlocksTest.cpp`) — AND semantics
for multiple `tag:` values (not OR), a `folder:` value containing literal
`%`/`_` matched literally rather than as a SQL wildcard, `orphans`
visibility gating checked in BOTH directions (an anonymous caller not
crediting a private linker, an admin correctly seeing one), and a tag
value shaped like `x'; DROP TABLE documents; --` proving it's treated as
an inert literal — the documents table is confirmed intact and queryable
in the same test, right after. Four more checks in `security_e2e.py`
exercise the same properties over a real HTTP server (anon/admin split,
`400` on a typo'd key with an `error` field, the injection-shaped value
producing a normal `200` with zero rows rather than a `500`). Live-
verified against real production content afterward (`recipes/` folder
listing, dinner-only tag filter, a real orphans list, a deliberately
typo'd block rendering a visible red error line instead of a blank
table) — see the deployment memory note for the exact queries run.

**`search:` — delegating to `FtsSearch` instead of reimplementing it.**
A follow-up added a `search:` key giving a query block real full-text
(and, when embeddings are configured, hybrid semantic) search — turning
it from a structured filter into an embedded search box. Rather than
duplicating FTS5 MATCH/BM25/RRF logic a second time inside
`QueryBlocks.cpp`, `search:` builds a `FtsSearch::SearchQuery` and calls
the SAME `FtsSearch&` instance `/api/search` already uses (passed into
`QueryBlocks`'s constructor in `main.cpp`, where it's already
constructed first) — getting the exact same ranking, query-embedding
cache, and distance/candidate-count tuning `/search` has, for free, with
zero duplicated logic. `SearchQuery`'s own `tags`/`docTypes`/
`folderPrefix`/`includePrivate`/`limit` fields already matched
`QueryBlocks`'s own `tag`/`type`/`folder`/visibility/`limit` semantics
closely enough that the mapping is closer to a straight field copy than
a translation layer.

`sort`, `order`, and `orphans` have no defined meaning once results come
back relevance-ranked instead of DB-ordered — `FtsSearch` has no
backlink concept at all, and "sort by title" doesn't compose with "sort
by relevance." Combining either with `search:` is a parse error (checked
right after the DSL parsing loop, before the normal sort-default
resolution that would otherwise run unconditionally), not one silently
winning over the other — the same explicit-refusal discipline this
file's own key-parsing already applies to an unknown/duplicate key.

**Verification**: five more unit tests (search finds via FTS5 body
text, visibility gating holds through the delegation in both
directions, `tag`/`type`/`folder` still apply as AND filters on top of
`search:`, `search:` combined with `sort`/`order`/`orphans` is a parse
error, an empty `search:` value is rejected) plus two more
`security_e2e.py` checks (visibility gating over real HTTP, the
search+sort conflict producing a `400` with an `error` field). Live-
verified against real production content: `search: beet soup` matched
`recipes/dinner/borscht.md` and `recipes/breakfast/pancakes.md` via the
hybrid semantic path even though neither document contains that exact
phrase, and `search: beet soup` combined with `sort: title` rendered the
expected error instead of a table — see the deployment memory note for
the exact queries run.

## Heading-level "zoom"/focus mode — experimental, isolated in its own commit

Inspired by the same survey of other self-hosted PKM tools as the
`\`\`\`query` blocks above: SiYuan (and Notion/Roam/LogSeq) let you "zoom"
into a single block, temporarily hiding everything else so you can focus
on just that block's own subtree. This app's content model is a plain
markdown file, not individually addressable blocks, so a true block-level
version isn't on the table without a real architecture change — but a
HEADING-scoped version costs nothing: the rendered document already has
real h2-h6 structure, `static/js/section-zoom.js` just hides/shows DOM
siblings around whichever heading was clicked. h1 excluded on purpose —
a document's body conventionally opens with its own "# Title" h1, which
already IS "the whole document."

`pages/view.js` now wraps the title+rendered body in its own
`<div id="doc-body">` — previously flattened into the same `innerHTML`
as the breadcrumbs/action-button row/backlinks list, which would have
meant "zoom into this section" also hiding the Edit/Delete buttons and
breadcrumb trail sitting at the same DOM level. Every heading gets a
stable `id` (slugified from its own text, de-duplicated per page) as a
plain HTML anchor — useful on its own even without zoom — and the zoom
STATE lives in a separate `#zoom=<slug>` hash (via `history.replaceState`,
not a bare hash assignment, so the Back button doesn't step through
every zoom click one at a time), so a shared link lands already focused.

**Two real bugs caught live, not hypothetical**: (1) the "Focused on: X"
banner label read `heading.textContent` AFTER the zoom button had
already been appended as the heading's own DOM child — `textContent`
walks every descendant's text, so the banner read "shared_ptrZoom"
instead of "shared_ptr" the first time this was actually clicked. Fixed
by capturing the heading's real text into `dataset.zoomLabel` before the
button exists. (2) "restore zoom from a shared `#zoom=<slug>` link"
looked broken on a second live check — the page loaded fully expanded
despite the hash being present — but turned out to be the BROWSER
serving a cached copy of the previous `section-zoom.js` across reloads,
not an actual logic bug; a hard refresh (and a throwaway
`console.error`-based trace, since this environment's console reader
only surfaces error-level messages, not plain `console.log`) confirmed
the restore logic was already correct. A lesson in not trusting a
"looks broken" result without first ruling out stale caches.

Marked experimental and kept in its own isolated commit (touches only
`section-zoom.js`, `view.js`'s wrapper div, and each theme's CSS) on
request — a clean `git revert` away if it doesn't earn its keep.

## Full graph page and per-document local graph widget

The last item from the SiYuan/Obsidian survey worth actually building:
Obsidian's Graph View, a force-directed visualization of every note as a
node and every link as an edge. In a huge Obsidian vault this tends to
degrade into an unreadable "hairball"; at THIS app's real scale (a
personal vault, typically tens to low hundreds of documents) it stays
useful. Layout later moved to Barnes-Hut in a Worker so a larger vault
doesn't freeze the UI (see below). Confirmed live against real production
content, not assumed: the full graph correctly clustered
`demo/wiki-links-example`'s two cross-linked notes and
`notes/linux/systemd-timers.md`'s link to `wireguard-setup.md`, with
every other real document (most of this vault's actual content) sitting
isolated, exactly matching what's actually in `document_links`.

**Data source**: `index::GraphQueries` (nodes = visible documents,
edges = real `[[wiki-link]]`s between two documents that both currently
exist) backs one `GET /api/graph` endpoint, same fail-safe-private
gating as `NavQueries` throughout — an edge only appears when BOTH
endpoints are visible to the caller, so a private document can't leak
its existence via an edge pointing at it (or from it) even when the
other end is public; verified in both directions by a dedicated unit
test, not just one. A "red link" (the target document doesn't currently
exist) is silently excluded from the edge list rather than inventing a
placeholder node for it — a deliberate v1 scope decision (Obsidian's own
graph view draws a distinct "unresolved" node for this case, which this
app doesn't attempt yet), not a bug.

**Local graph is a second, filtered query.** `GET /api/graph?around={path}`
returns the connected component of that document over visible
`[[wiki-link]]` edges (undirected): the document itself plus every
visible document reachable from it at any depth, and every visible edge
among those nodes (the induced subgraph — an edge between two neighbors
is included even when it doesn't touch the center). v1 of this query
was a single bound join, 1-hop only; that hid everything past the first
neighbor (A→B→C showed A and B, never C) which made the rail look like
a duplicate of the backlinks/outgoing-links lists. Depth is still not
a client `hops=` query param — GraphRoutes ignores that rather than
trusting the caller to pick a number. A private document is never a
stepping stone: an anonymous caller does not reach a further public
document through one they cannot see. `around=` is untrusted caller
input: PathGuard first (same 404 as GET `/api/documents` for a
traversal/`filesystem_error`), then an exact bound path lookup, never
LIKE. Missing and private-to-anon are the same 404 — existence of
private content is not revealed. The full `GET /api/graph` (no
`around=`) is unchanged and still backs the dedicated graph page.

**Rendering is a from-scratch Barnes-Hut force layout**
(`static/js/graph-layout.js`), not a vendored physics library like
d3-force: repulsion approximated through a quadtree (O(n log n) per
iteration instead of the original pairwise O(n²)), spring attraction
along real edges, a weak pull toward center so a disconnected node
doesn't drift off-screen, velocity damping, run for a fixed iteration
count. Graphs small enough that the tree overhead would dominate (the
local-graph case) still use the exact pairwise repulsion; the quadtree
kicks in above that. The simulation runs in a dedicated Worker so a
large vault doesn't freeze the UI thread (same file also loads as a
page script for a sync fallback if Worker construction fails). Resize
does not re-run the simulation — cached coordinates are uniformly
scaled into the new box, always from the original layout space so a
chain of resizes cannot compound. Initial node placement is a
deterministic circle, not `Math.random()`, so the SAME graph settles
into a recognizable, only mildly different layout across reloads
rather than a jarring fresh scatter every time. Same "write it
ourselves when it's small, vendor when it's genuinely complex" split
this app already applies elsewhere (query-block.js/section-zoom.js vs.
mermaid.js/Prism.js); Barnes-Hut is the complex part of this algorithm,
still ours rather than d3-force.

Output is painted by a capability ladder in `graph-render.js`:
WebGL, then canvas 2D, then SVG-in-the-DOM. Each rung is probed with a
real `getContext` (and for WebGL, a shader compile), not "does
`WebGLRenderingContext` exist" — that constructor can be present while
context creation fails. Layout is the Worker/Barnes-Hut path above,
independent of which paint backend wins. SVG remains the last fallback
because nodes are real `<a href="/d/...">` (ordinary navigation, no
client-side router — this app already does a full page reload on every
navigation) built via `createElementNS` + DOM properties throughout, so a
document title reaches the page as inert text with no manual escaping
needed (unlike `query-block.js`'s HTML-string rendering, which genuinely
does need `WikiCommon.escapeHtml`). Canvas/WebGL hit-test in JS and
navigate the same URLs; a visually-hidden `<nav>` of the same links keeps
keyboard access when the paint is a bitmap. Both surfaces pan (click-drag)
and zoom (wheel toward the cursor); pointer capture starts only after an
8px drag so a click on a node still navigates.

**Two places this shows up**: a new `/graph` page (the whole vault,
wired into the sidebar) and a "Local graph" widget on the document view
page — a collapsed right rail (not inline after the backlinks; that
placement ate reading width) of this document's connected component,
omitted entirely (same "don't show an empty section" discipline the
backlinks list already follows) when the document genuinely has no
neighbors at all.

**`/graph` gained a toolbar** once the vault is large enough that
drawing every title under every node is unreadable (the hairball the
Obsidian survey already warned about). Layout stays client-side on the
same `GET /api/graph` payload. The filter box also hits
`GET /api/graph/matches?q=` (debounced) so a match in the document body
lights the node up too:

- **Filter** (title/path substring instantly, then a debounced FTS
  match over document body/title/tags via `GET /api/graph/matches?q=`):
  matches stay bright, the rest dim to ~40% opacity. Does not drop
  nodes from the layout, so typing doesn't send the simulation running
  again; pan/zoom is kept across keystrokes. Uses the same FTS5 MATCH
  as `/api/search` (visibility-gated, prefix terms) but returns paths
  only — no snippets, no ranked top-50, no hybrid semantic neighbors.
  Empty `q` is an empty path list, not every document.
- **Hide unlinked**: degree-0 documents (no `[[wiki-link]]` edge with
  another real, currently-existing document) are dropped before layout.
  That's the main readability lever for a vault that's mostly isolates.
  Persisted in `localStorage` (`wiki.graph.hideUnlinked`); the search
  box is not.
- **Labels**: `auto` (default: hover + degree ≥ 2 + zoomed-in past
  ~1.75×) / `On hover` / `All`. Persisted as `wiki.graph.labels`. The
  local-graph rail does not get this toolbar and still draws every
  label — it's already one connected component.

Two layout defaults, not toggles: node radius grows slightly with
degree (still capped below the local-graph "you are here" radius), and
isolates get a much weaker pull toward the center so they sit on the
periphery instead of piling on top of the real clusters.

**A real bug found via a user screenshot: labels on two directly-linked
nodes overlapped each other.** Each node's own label
(`static/js/graph-render.js`) is `text-anchor: middle`, centered
directly under its node — and the original `SPRING_LENGTH` (the target
rest distance the spring force pulls a connected pair toward) was
`90`px. A typical multi-word document title at this theme's own
font-size/font-family renders 80-110px wide on its own; two
directly-connected nodes settling near that 90px rest length left
essentially no room for either label without the two colliding,
regardless of which theme was active (confirmed on both green and
classic after the fix, using the exact two-document scenario from the
report — a document linking to another via a real `[[wiki-link]]`).
Fixed by raising `SPRING_LENGTH` to `150` — no per-label text-width
measurement needed (this app's real vaults are tens of documents with
short, human-written titles, not the kind of scale that would ever
need that precision); the repulsion/spring balance re-settles on its
own at the new target distance, same simulation, just tuned to leave
realistic multi-word titles room to breathe.

## Print/Download/Upload buttons, and a real dead-route bug found building them

Three small, purely additive UI actions: the view page's "Print" button
(renamed from "Print / Export PDF" — the button never generated a PDF
itself, `window.print()` just hands off to the browser's own print
dialog, so the old label overpromised) gained a "Download" button right
next to it, and the edit page's "Attach file" button gained an "Upload"
button right next to it. Both new buttons are deliberately thin:

- **Download** (`static/js/pages/view.js`) fetches
  `GET /api/documents/{path}/raw`, wraps the response in a `Blob`, and
  drives a throwaway `<a download>` — no server-side "export" endpoint
  needed, `/raw` already returns the exact on-disk bytes (front-matter
  included) with the same fail-safe-private gating as every other read
  route.
- **Upload** (`static/js/pages/edit.js`) is the reverse: `FileReader`
  reads a locally-picked `.md` file, a new small `parseFrontMatterClientSide`
  helper splits it into front-matter fields + body (deliberately NOT a
  general YAML parser — it only understands the exact flat shape
  `FrontMatter::serializeFrontMatter`, src/vault/FrontMatter.cpp, actually
  writes: scalar `title`/`type`/`visibility`, `tags` as a flow-sequence
  `[a, b]`), populates the matching form fields, and replaces the editor's
  content with the body. Nothing touches the network — the file only
  ever reaches the server if the user hits Save afterward. If the editor
  already has non-empty content, a themed confirm (`WikiDialog`, not
  `window.confirm`) guards against silently discarding unsaved work; a
  brand-new empty document skips the prompt entirely.

**A real, previously-shipped bug found and fixed building Download, not
hypothetical**: `GET /api/documents/{path}/raw` (`DocumentRoutes.cpp`)
already existed — but was completely unreachable, on every single
request, since the route was first added. Drogon's
`registerHandlerViaRegex` matches regex handlers in REGISTRATION order,
first match wins; the general `GET /api/documents/{path...}` handler's
own `"^/api/documents/(.*)$"` was registered BEFORE the more specific
`"^/api/documents/(.*)/raw$"`, and `(.*)` is greedy enough to swallow a
trailing `/raw` as part of its own capture group. Every request to
`/raw` matched the general handler first, which then 404'd (no document
literally named `notes/foo.md/raw` exists), and the dedicated `/raw`
handler below it never ran at all. Caught immediately when Download's
own `fetch()` came back 404 for a document confirmed to exist — fixed by
moving the `/raw` handler's registration above the general one (order
between DIFFERENT regex handlers matters here the same way filter
registration order already mattered elsewhere in this codebase, just a
different Drogon mechanism). A comment at the new registration site now
explains why the order can never be swapped back.

**Verification**: two new `security_e2e.py` checks (anon gets the
literal public document's on-disk bytes including its `---` front-matter
block, not JSON; anon gets `404` for a private document's raw bytes;
admin gets the private document's raw bytes) — this is an HTTP-routing
bug, not a unit-testable one, so `QueryBlocksTest`-style C++ tests
wouldn't have caught it either way. Live-verified afterward: `curl` against
the real `/raw` route returned the expected front-matter + body for a
real document; the Upload path was verified end-to-end in a real browser
(a hand-crafted `.md` file with `title`/`tags: [drink, strong, party]`/
`type: recipe`/`visibility: private` front-matter correctly populated
every form field and the editor body, and the resulting Save round-tripped
correctly — confirmed via the saved document's own tag cloud entries).

## Type/Tags typeahead on the edit form

The edit form's `Type` and `Tags` fields used to be plain `<input
type="text">` boxes with zero discoverability of what already exists in
the vault — a typo (`recipie` instead of `recipe`) silently created a
new, disconnected type/tag rather than erroring or suggesting the real
one. Both fields gained a typeahead: suggestions from the SAME already-
existing, already-visibility-gated `GET /api/nav/types`/`GET /api/nav/
tags` endpoints the tag cloud and search page's own filter already use,
filtered client-side as you type.

**Deliberately NOT search.js's `createMultiSelect`** (the checkbox
popover backing the search page's tag/type filters), even though the
data source is identical — that widget assumes a CLOSED set (you can
only ever filter by a tag/type that already exists). `Type`/`Tags` are
free text on this form; the first document of a genuinely new type, or
the first use of a new tag, has to stay possible. So `edit.js`'s new
`createTypeahead` is a plain text input with a suggestions dropdown
layered on top — pick one, or keep typing your own value, either always
works. Two modes: `"single"` (Type) replaces the whole field; `"token"`
(Tags) only replaces the comma-separated segment currently being typed,
so picking a suggestion for the second tag doesn't clobber the first one
already written — and already-picked tags are excluded from their own
field's suggestion list, so the dropdown never tempts a duplicate.

Deliberately caret-position-agnostic for `"token"` mode: always treats
the LAST comma-separated segment as "what's being typed right now",
regardless of where the text cursor actually sits. Covers the
overwhelmingly common case (typing new tags onto the end of the list)
with far less code than real caret-aware token editing — editing a tag
in the middle of an existing list just won't get suggestions scoped to
it, a deliberate simplicity trade-off matching this codebase's "write it
ourselves when small" pattern, not an oversight.

CSS follows the exact same split `.ms-select`/`.ms-menu` (search.js's
own dropdown) already established: layout-only rules (position,
sizing, the `[hidden]`-vs-author-rule specificity override — see that
existing rule's own comment for why an explicit `.typeahead-menu[hidden]
{ display: none; }` is required, not optional) live once in `edit.css`
since they're identical regardless of active theme; colors get their own
small block in each of the three `css/themes/*.css` files, matching
that file's own palette.

**Two real bugs found and fixed via user-reported screenshots, not caught
in the first round of testing**: a user screenshot showed the dropdown's
lower rows visually overlapping the document body text beneath it, and
the `Public` checkbox sitting noticeably higher than its neighboring
Title/Tags inputs on the same row.

The checkbox misalignment turned out to be pre-existing, unrelated to
this feature specifically — `.field-row`'s `align-items: center`
centers each label's own BOX within the row, but a text+input label
(column layout: label text ABOVE its input) is taller than the
single-row `.visibility-toggle` (checkbox + "Public", nothing above
it), so centering the shorter box against the taller one leaves the
checkbox sitting above the actual input baseline it's meant to line up
with — confirmed via `getBoundingClientRect()`: Title's own input and
the checkbox had a ~10px vertical CENTER mismatch even before this
session touched anything. Fixed by switching `.field-row` to
`align-items: flex-end` instead — every label's BOTTOM edge lines up
now (an input's bottom edge sits right at its own label's bottom;
nothing below it in the column), regardless of how many lines of label
text sit above it.

The overlap bug took considerably more digging, and the eventual root
cause was NOT the first several things suspected (max-height/overflow
clipping, incorrect auto-height calculation, missing z-index — all
ruled out one at a time via `getComputedStyle()`/`getBoundingClientRect()`
checks that came back looking entirely correct). The actual cause,
found by walking the WYSIWYG canvas's own ancestor chain: Toast UI
Editor's `#editor` nests SEVERAL of its own `position: relative`/
`absolute` containers several layers deep (`.toastui-editor-main`,
`.toastui-editor-main-container`, the ProseMirror root itself) —
deep enough that giving `.field-row` its own explicit
`position: relative; z-index: 1` reliably beat the editor's plain
(unpositioned) TOOLBAR, but not consistently the WYSIWYG body text
several positioned layers further in. Rather than keep chasing
z-index numbers against an unpredictable nested stacking nightmare,
`createTypeahead`'s dropdown is now rendered as a **portal**: appended
directly to `document.body` (not nested inside `.typeahead-wrap`) with
`position: fixed` and its `top`/`left`/`min-width` computed from the
input's own `getBoundingClientRect()` (`positionMenu()`, re-run on
every open/re-render — `getBoundingClientRect()` is already
viewport-relative, the same coordinate space `position: fixed` uses, so
no scroll-offset math is needed). This sidesteps ancestor
stacking-context interaction with the editor entirely: the menu's
stacking rank is compared directly against `#editor` at the same
(body-level) depth, never nested three layers inside it — the standard,
well-established fix for "a dropdown trapped by a sibling's z-index
games", used throughout real-world component libraries for exactly this
class of problem.

Live-verified in a real browser on both the green and classic themes,
against the SAME document that originally reproduced the bug (a long
real-world body — `Load .TAP`/`Load .SNA` documentation with a table —
tall enough for the dropdown's full row count to reach into the body
text below the editor toolbar): all 4 matching tag suggestions now
render fully, with an unbroken border/background, on top of the
document's own heading text; Arrow-key navigation + Enter correctly
picks a suggestion and appends it to the Tags field with a trailing
comma-space, ready for the next tag; the `Public` checkbox now sits
flush with the Title/Type/Tags inputs on the same row.

## Edit-page attachment insert, and stylesheet MIME refusals under `/edit/`

Two frontend-only follow-ups to the attachment list (the list itself,
`GET`/`DELETE /api/attachments/...`, and MCP `attach_file` landed
earlier — see `docs/mcp.md`). Neither needed a rebuild.

**Double-click a filename in the list to insert it into the editor.**
The list is a view of the owning document's `.assets/` folder, not a
picker that rewrites markdown on delete; inserting is a separate
gesture. Raster images (`image/*` except SVG, same rule as
`AttachToDocument`) become an image node; everything else a regular
link. The "Attach file" button uses the same helper after the upload
lands, so both paths write the same shape.

**`editor.insertText("[name](url)")` is the wrong API in WYSIWYG.**
It is correct in Markdown mode. In WYSIWYG Toast UI dumps the brackets
as literal text, then auto-linkifies the `[name]` piece and leaves
`](url)` visible as leftover characters — the same broken insert from
both double-click and Attach file. The toolbar's own link/image popups
go through `editor.exec("addLink", {linkUrl, linkText})` /
`editor.exec("addImage", {imageUrl, altText})`; those commands write
the right node in BOTH modes (markdown-mode variants insert the
`[]()` / `![]()` source). Verified live in WYSIWYG: a double-clicked
filename became a real clickable link, not leftover markdown syntax.

**Static `<link href="css/edit.css">` (and Toast UI's own CSS) in
`shell.html` shipped a real MIME-type console error on every `/edit/`
load under a subpath.** Theme CSS already avoided this by being
created in JS after `<base>` exists (see "Visual theme picker"
above). `edit.css` + `toastui-editor.css` + `toastui-editor-dark.css`
were still static tags. The HTML preload scanner fetches stylesheets
in parallel with the first `<head>` script, resolving URLs against
the DOCUMENT path, not against a `<base>` that script hasn't appended
yet. On `/wiki/edit/{doc}` that meant `css/edit.css` →
`/wiki/edit/css/edit.css`, which is this same SPA shell (`text/html`);
Chrome then refused it (`MIME type is not a supported stylesheet type,
and strict MIME checking is enabled`). The page still "worked"
because a later navigation or cache sometimes recovered the real
files, but the console was red on every fresh edit-page load. Fixed
the same way theme CSS already was: those three `<link>`s are now
`createElement`d in the theme bootstrap script, after `<base>` exists,
so they resolve against `/wiki/` like everything else. Putting static
stylesheet tags back in `shell.html`'s markup re-breaks this.

An unrelated `share-modal.js` `Cannot read properties of null
(reading 'addEventListener')` in the same console is a browser
extension, not this repo.

## Editor Draft agent

Optional cloud drafting on the edit page (`docs/llm.md`). wiki-server is
an OpenAI-compatible chat *client*; tools run locally; `propose_draft`
fills the editor; Save is the only disk write. MCP is not in this path.
Follow-up edits use `append_to_draft` / `insert_in_draft` /
`replace_in_draft` rather than rewriting the whole body.
`search_documents` is `FtsSearch::search` (hybrid when embeddings are on).
Account lists `compose:` rows next to MCP audit.
Ambiguous instructions are supposed to come back as questions in the Draft
panel, not as a silent rewrite. Assistant text streams into the panel
(`stream:true` to the chat API, SSE to the browser; poll is the fallback).
The panel stashes the editor selection
(and the caret, for insert) across that focus change from Toast UI's
own markdown/WYSIWYG model, not `window.getSelection()` (that jumps to
the start on blur); a chip in the panel
is the stand-in once Toast UI drops the visible caret / highlight. Save stays on the edit page
and keeps the session; View (next to Save) goes back to the document.
Revert undoes the last applied draft because
`setMarkdown` wipes Toast UI history. A selected follow-up sends the
model an excerpt around that span, not the whole body.

`[llm].provider = "none"` (default) hides the Draft button and the
sidebar Chat icon. `system_prompt` in config.toml replaces the compiled
Draft prompt when non-empty; `chat_system_prompt` does the same for
Chat.

**Toast UI WYSIWYG cannot round-trip `[[wiki-link]]` as source.** The
markdown writer treats those brackets as punctuation and emits
`\[\[path\|label\]\]`; a `widgetRules` attempt to keep them as widgets
overlapped real documents and leaked `$$widgetN$$` markers (caught live
on `demo/wiki-links-example`). The editor therefore maps
`[[path|label]]` ↔ `[label](wiki:path)` around `setMarkdown` /
`getMarkdown` (`static/js/common.js`), and `propose_draft` also
unescapes model-over-escaped punctuation. The `wiki:` URL never hits
disk.

## Sidebar Chat

Floating vault Q&A (`docs/llm.md`), opened from a sidebar icon shown
only when `GET /api/session` has `agentEnabled` (llm configured and
the caller is the admin). Same cloud client and SSE as Draft; read-only
tools; audit prefix `chat:`. Each send includes the open wiki page
(`view`) so "this document" / "this folder" can call `get_current_view`
then `get_document` / `list_documents`. `run_query_block` executes a
query-block fence's DSL (`QueryBlocks::parseAndRun`, same as
`GET /api/query`). `list_document_history` / `diff_document_history`
read `document_snapshots` (snapshot body vs current, same as the
History page). Chat history is SQLite
`agent_chats` (salvaged on index rebuild); the floating panel has a
left sidebar for New / rename / delete (list width in `localStorage`).
State of the *open* thread
survives wiki navigation via the session id in `sessionStorage` (not
DELETE on `pagehide`).
`kind: "chat"` on `POST /api/agent/sessions`.

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
