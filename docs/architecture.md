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

## Later addition: sidebar tag namespace grouping + filter

Well past M5, once the vault had accumulated enough tags for the sidebar's flat `<ul>`
of every tag in use to become genuinely unwieldy on screen — the same problem the
document tree already had, and already had a fix for.

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
